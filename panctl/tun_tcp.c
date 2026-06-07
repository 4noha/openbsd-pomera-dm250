/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 4noha */
/*
 * panctl/tun_tcp.c — userland TCP-over-tun, v1 (multi-connection).
 *
 * Holds an array of MAX_TCP_CONNS slot structs, each tracking one 5-tuple
 * TCP flow. Lookups are linear scans (small N — Tailscale + a handful of
 * concurrent app connections fit easily).
 *
 * Scope (unchanged from v0, plus multi-conn):
 *   * No retransmission timer. We rely on the local kernel to retransmit
 *     unacked segments; we keep ACKing what we get.
 *   * No TCP options beyond MSS (advertised in our SYN-ACK).
 *   * No window scaling, no SACK, no PAWS. Fixed 32 KiB receive window.
 *   * IPv4 only. ICMP/IPv6 silently dropped by the caller (divert.c demux).
 *
 * Sequence number scheme: each conn picks its ISN as
 *     0xC0FFEE00 + (slot_index * 0x10000)
 * so a slot reuse can't accidentally inherit live seq numbers from the
 * previous tenant within a normal kernel TIME_WAIT window.
 */

#include "tun_tcp.h"
#include "frame.h"
#include "ipv4.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ============================ constants ============================ */

#define MAX_TCP_CONNS    128

#define IP_PROTO_TCP     6
#define TCP_HDR_MIN     20
#define TCP_FLAG_FIN  0x01
#define TCP_FLAG_SYN  0x02
#define TCP_FLAG_RST  0x04
#define TCP_FLAG_PSH  0x08
#define TCP_FLAG_ACK  0x10

#define OUR_ISN_BASE  0xC0FFEE00u
#define OUR_MSS       900
#define OUR_WINDOW    32768

#define TUN_AF_HDR    4

#define STREAM_ID_BASE 0xE000

/* ============================ bytewise helpers ============================ */

static uint16_t rd16(const uint8_t *b) { return (uint16_t)((b[0] << 8) | b[1]); }

static uint32_t
rd32(const uint8_t *b)
{
	return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16)
	     | ((uint32_t)b[2] << 8)  | (uint32_t)b[3];
}

static void wr16(uint8_t *b, uint16_t v) { b[0] = (uint8_t)(v >> 8); b[1] = (uint8_t)v; }

static void
wr32(uint8_t *b, uint32_t v)
{
	b[0] = (uint8_t)(v >> 24); b[1] = (uint8_t)(v >> 16);
	b[2] = (uint8_t)(v >>  8); b[3] = (uint8_t)v;
}

static uint16_t
tcp_checksum(const uint8_t src[4], const uint8_t dst[4],
             const uint8_t *tcp, size_t tcp_len)
{
	uint8_t pseudo[12];
	memcpy(pseudo + 0, src, 4);
	memcpy(pseudo + 4, dst, 4);
	pseudo[8]  = 0;
	pseudo[9]  = IP_PROTO_TCP;
	wr16(pseudo + 10, (uint16_t)tcp_len);

	uint8_t scratch[12 + 1500];
	if (tcp_len > sizeof scratch - 12)
		return 0;
	memcpy(scratch, pseudo, 12);
	memcpy(scratch + 12, tcp, tcp_len);
	return ipv4_checksum(scratch, 12 + tcp_len);
}

/* ============================ state ============================ */

enum tcp_state {
	TS_CLOSED,
	TS_SYN_RCVD,
	TS_ESTAB,
	TS_FIN_LOCAL,
	TS_FIN_REMOTE,
	TS_CLOSING,
};

struct tcp_conn {
	enum tcp_state state;
	uint16_t stream_id;

	uint8_t  src_ip[4];
	uint16_t src_port;
	uint8_t  dst_ip[4];
	uint16_t dst_port;

	uint32_t peer_seq_next;
	uint32_t our_seq_next;
	uint16_t ip_id;
};

struct tun_tcp {
	int tun_fd;
	struct mux_session *mux;
	struct tcp_conn conns[MAX_TCP_CONNS];
	uint16_t syn_age[MAX_TCP_CONNS];  /* ticks since SYN_RCVD entered */
};

/* If a slot stays in SYN_RCVD for more than this many sweeps, recycle. */
#define SYN_RCVD_MAX_AGE 30

/* ============================ slot helpers ============================ */

static struct tcp_conn *
conn_find_by_5tuple(struct tun_tcp *t,
                    const uint8_t src_ip[4], uint16_t src_port,
                    const uint8_t dst_ip[4], uint16_t dst_port)
{
	for (size_t i = 0; i < MAX_TCP_CONNS; i++) {
		struct tcp_conn *c = &t->conns[i];
		if (c->state == TS_CLOSED) continue;
		if (c->src_port != src_port || c->dst_port != dst_port) continue;
		if (memcmp(c->src_ip, src_ip, 4) != 0) continue;
		if (memcmp(c->dst_ip, dst_ip, 4) != 0) continue;
		return c;
	}
	return NULL;
}

static struct tcp_conn *
conn_find_by_sid(struct tun_tcp *t, uint16_t sid)
{
	for (size_t i = 0; i < MAX_TCP_CONNS; i++) {
		struct tcp_conn *c = &t->conns[i];
		if (c->state != TS_CLOSED && c->stream_id == sid)
			return c;
	}
	return NULL;
}

static struct tcp_conn *
conn_alloc(struct tun_tcp *t, size_t *out_idx)
{
	for (size_t i = 0; i < MAX_TCP_CONNS; i++) {
		if (t->conns[i].state == TS_CLOSED) {
			if (out_idx) *out_idx = i;
			return &t->conns[i];
		}
	}
	return NULL;
}

static void
conn_reset(struct tcp_conn *c)
{
	memset(c, 0, sizeof *c);
	c->state = TS_CLOSED;
}

/* ============================ packet build ============================ */

/*
 * Build an IPv4+TCP segment for one specific connection (used for SYN-ACK,
 * data, ACK, FIN, RST). Writes into buf including the OpenBSD tun AF prefix.
 */
static int
build_segment(struct tcp_conn *c, uint8_t flags,
              const uint8_t *payload, size_t payload_len,
              uint8_t *buf, size_t cap)
{
	size_t opt_len = (flags & TCP_FLAG_SYN) ? 4 : 0;  /* just MSS option */
	size_t tcp_hdr_len = TCP_HDR_MIN + opt_len;
	size_t total = TUN_AF_HDR + IPV4_HEADER_MIN + tcp_hdr_len + payload_len;
	if (total > cap)
		return -1;

	buf[0] = 0; buf[1] = 0; buf[2] = 0; buf[3] = 2;  /* AF_INET */
	uint8_t *ip = buf + TUN_AF_HDR;

	ip[0] = 0x45;
	ip[1] = 0;
	wr16(ip + 2, (uint16_t)(IPV4_HEADER_MIN + tcp_hdr_len + payload_len));
	wr16(ip + 4, c->ip_id++);
	wr16(ip + 6, 0x4000);
	ip[8] = 64;
	ip[9] = IP_PROTO_TCP;
	wr16(ip + 10, 0);
	memcpy(ip + 12, c->dst_ip, 4);   /* src on this packet = real remote */
	memcpy(ip + 16, c->src_ip, 4);   /* dst = local */
	wr16(ip + 10, ipv4_checksum(ip, IPV4_HEADER_MIN));

	uint8_t *tcp = ip + IPV4_HEADER_MIN;
	wr16(tcp + 0, c->dst_port);
	wr16(tcp + 2, c->src_port);
	wr32(tcp + 4, c->our_seq_next);
	wr32(tcp + 8, c->peer_seq_next);
	tcp[12] = (uint8_t)((tcp_hdr_len / 4) << 4);
	tcp[13] = flags;
	wr16(tcp + 14, OUR_WINDOW);
	wr16(tcp + 16, 0);
	wr16(tcp + 18, 0);

	if (flags & TCP_FLAG_SYN) {
		tcp[20] = 2; tcp[21] = 4;
		wr16(tcp + 22, OUR_MSS);
	}
	if (payload_len > 0)
		memcpy(tcp + tcp_hdr_len, payload, payload_len);

	wr16(tcp + 16, tcp_checksum(ip + 12, ip + 16, tcp,
	                            tcp_hdr_len + payload_len));
	return (int)total;
}

static void
write_segment(struct tun_tcp *t, struct tcp_conn *c, uint8_t flags,
              const uint8_t *payload, size_t payload_len)
{
	uint8_t buf[TUN_AF_HDR + IPV4_HEADER_MIN + TCP_HDR_MIN + 4 + 1024];
	int n = build_segment(c, flags, payload, payload_len, buf, sizeof buf);
	if (n < 0)
		return;
	(void)write(t->tun_fd, buf, (size_t)n);
}

/* ============================ inbound (tun → mux) ============================ */

void
tun_tcp_handle_packet(struct tun_tcp *t, const uint8_t *ip, size_t len)
{
	if (len < IPV4_HEADER_MIN) return;
	if ((ip[0] >> 4) != 4) return;
	size_t ip_hdr_len = (size_t)(ip[0] & 0x0F) * 4;
	if (ip_hdr_len < IPV4_HEADER_MIN || ip_hdr_len > len) return;
	if (ip[9] != IP_PROTO_TCP) return;
	uint16_t total_len = rd16(ip + 2);
	if (total_len < ip_hdr_len || (size_t)total_len > len) return;

	const uint8_t *tcp = ip + ip_hdr_len;
	size_t tcp_total = (size_t)total_len - ip_hdr_len;
	if (tcp_total < TCP_HDR_MIN) return;

	uint16_t src_port = rd16(tcp + 0);
	uint16_t dst_port = rd16(tcp + 2);
	uint32_t seq      = rd32(tcp + 4);
	uint8_t  flags    = tcp[13];
	size_t   tcp_hdr_len = (size_t)((tcp[12] >> 4) & 0x0F) * 4;
	if (tcp_hdr_len < TCP_HDR_MIN || tcp_hdr_len > tcp_total) return;
	const uint8_t *payload = tcp + tcp_hdr_len;
	size_t payload_len = tcp_total - tcp_hdr_len;

	const uint8_t *src_ip = ip + 12;
	const uint8_t *dst_ip = ip + 16;

	/* SYN: try to allocate a new slot (or reuse existing match). */
	if (flags & TCP_FLAG_SYN) {
		struct tcp_conn *existing = conn_find_by_5tuple(t, src_ip, src_port,
		                                                dst_ip, dst_port);
		if (existing != NULL) {
			/* Duplicate SYN (re-tx). Stay quiet; the SYN-ACK we'll send
			 * after TCP_OPEN_ACK or the SYN-ACK we already sent will
			 * be enough. */
			return;
		}
		size_t slot_idx = 0;
		struct tcp_conn *c = conn_alloc(t, &slot_idx);
		if (c == NULL) {
			fprintf(stderr, "[tun-tcp] no free slot for SYN from %u.%u.%u.%u:%u\n",
			    src_ip[0], src_ip[1], src_ip[2], src_ip[3], src_port);
			return;
		}
		conn_reset(c);
		memcpy(c->src_ip, src_ip, 4);
		c->src_port = src_port;
		memcpy(c->dst_ip, dst_ip, 4);
		c->dst_port = dst_port;
		c->peer_seq_next = seq + 1;
		c->our_seq_next  = OUR_ISN_BASE + ((uint32_t)slot_idx << 16);
		c->ip_id         = 1;
		c->stream_id     = (uint16_t)(STREAM_ID_BASE + slot_idx);
		c->state = TS_SYN_RCVD;

		struct endpoint dst = { .addr_type = ADDR_TYPE_IPV4, .addr_len = 4 };
		dst.port = c->dst_port;
		memcpy(dst.addr, c->dst_ip, 4);
		(void)mux_send_tcp_open(t->mux, c->stream_id, &dst);
		return;
	}

	/* Non-SYN: must find an existing conn. */
	struct tcp_conn *c = conn_find_by_5tuple(t, src_ip, src_port,
	                                         dst_ip, dst_port);
	if (c == NULL)
		return;  /* stray packet */

	if (flags & TCP_FLAG_RST) {
		(void)mux_send_tcp_rst(t->mux, c->stream_id, RST_REASON_PEER_RESET);
		conn_reset(c);
		return;
	}

	if (c->state == TS_SYN_RCVD) {
		if (flags & TCP_FLAG_ACK)
			c->state = TS_ESTAB;
	}

	if (payload_len > 0) {
		(void)mux_send_tcp_data(t->mux, c->stream_id, payload, payload_len);
		c->peer_seq_next = seq + (uint32_t)payload_len;
		write_segment(t, c, TCP_FLAG_ACK, NULL, 0);
	}

	if (flags & TCP_FLAG_FIN) {
		c->peer_seq_next = seq + (uint32_t)payload_len + 1;
		(void)mux_send_tcp_close_wr(t->mux, c->stream_id);
		write_segment(t, c, TCP_FLAG_ACK, NULL, 0);
		if (c->state == TS_FIN_REMOTE) {
			c->state = TS_CLOSING;
			conn_reset(c);
		} else {
			c->state = TS_FIN_LOCAL;
		}
	}
}

/* ============================ outbound (mux → tun) ============================ */

int
tun_tcp_on_tcp_open_ack(struct tun_tcp *t, uint16_t sid, uint16_t status)
{
	struct tcp_conn *c = conn_find_by_sid(t, sid);
	if (c == NULL)
		return 0;
	if (status != 0) {
		fprintf(stderr, "[tun-tcp] sid=%u open ack status=0x%04x\n",
		    sid, status);
		write_segment(t, c, TCP_FLAG_RST | TCP_FLAG_ACK, NULL, 0);
		conn_reset(c);
		return 1;
	}
	write_segment(t, c, TCP_FLAG_SYN | TCP_FLAG_ACK, NULL, 0);
	c->our_seq_next++;
	return 1;
}

int
tun_tcp_on_tcp_data(struct tun_tcp *t, uint16_t sid,
                    const uint8_t *data, size_t len)
{
	struct tcp_conn *c = conn_find_by_sid(t, sid);
	if (c == NULL)
		return 0;
	const uint8_t *p = data;
	size_t left = len;
	while (left > 0) {
		size_t take = left > OUR_MSS ? OUR_MSS : left;
		write_segment(t, c, TCP_FLAG_ACK | TCP_FLAG_PSH, p, take);
		c->our_seq_next += (uint32_t)take;
		p += take;
		left -= take;
	}
	return 1;
}

int
tun_tcp_on_tcp_close_wr(struct tun_tcp *t, uint16_t sid)
{
	struct tcp_conn *c = conn_find_by_sid(t, sid);
	if (c == NULL)
		return 0;
	write_segment(t, c, TCP_FLAG_FIN | TCP_FLAG_ACK, NULL, 0);
	c->our_seq_next++;
	if (c->state == TS_FIN_LOCAL) {
		c->state = TS_CLOSING;
		conn_reset(c);
	} else {
		c->state = TS_FIN_REMOTE;
	}
	return 1;
}

int
tun_tcp_on_tcp_rst(struct tun_tcp *t, uint16_t sid, uint16_t reason)
{
	struct tcp_conn *c = conn_find_by_sid(t, sid);
	if (c == NULL)
		return 0;
	fprintf(stderr, "[tun-tcp] sid=%u mux RST reason=0x%04x\n", sid, reason);
	write_segment(t, c, TCP_FLAG_RST | TCP_FLAG_ACK, NULL, 0);
	conn_reset(c);
	return 1;
}

/* ============================ ctor / dtor ============================ */

struct tun_tcp *
tun_tcp_create(int tun_fd, struct mux_session *mux)
{
	struct tun_tcp *t = (struct tun_tcp *)calloc(1, sizeof *t);
	if (t == NULL)
		return NULL;
	t->tun_fd = tun_fd;
	t->mux = mux;
	for (size_t i = 0; i < MAX_TCP_CONNS; i++)
		t->conns[i].state = TS_CLOSED;
	return t;
}

void
tun_tcp_destroy(struct tun_tcp *t)
{
	if (t == NULL)
		return;
	free(t);
}

void
tun_tcp_sweep(struct tun_tcp *t)
{
	if (t == NULL)
		return;
	for (size_t i = 0; i < MAX_TCP_CONNS; i++) {
		struct tcp_conn *c = &t->conns[i];
		if (c->state == TS_SYN_RCVD) {
			if (++t->syn_age[i] > SYN_RCVD_MAX_AGE) {
				fprintf(stderr,
				    "[tun-tcp] stale SYN_RCVD slot=%zu sid=%u, recycling\n",
				    i, c->stream_id);
				/* Best-effort tell peer to stop holding the
				 * stream id (likely already abandoned). */
				(void)mux_send_tcp_rst(t->mux, c->stream_id,
				    RST_REASON_OTHER);
				conn_reset(c);
				t->syn_age[i] = 0;
			}
		} else {
			t->syn_age[i] = 0;
		}
	}
}
