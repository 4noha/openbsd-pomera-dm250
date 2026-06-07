/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 4noha */
/*
 * Feature macros: OpenBSD hides BSD types (u_int / u_short) under
 * <netinet/ip.h>, <net/if_tun.h> etc. when _POSIX_C_SOURCE is defined.
 * divert.c is BSD-only code so we explicitly opt back in to the BSD
 * namespace. Must come before any header include.
 */
#if defined(__OpenBSD__)
#  undef  _POSIX_C_SOURCE
#  define __BSD_VISIBLE 1
#endif

/*
 * panctl/divert.c — pomera 側 outbound intercept (OpenBSD implementation).
 *
 * NOTE ON PORTABILITY:
 *   The whole body is guarded by `#if defined(__OpenBSD__)`. On macOS this
 *   file compiles to an empty stub so the build system can include it in
 *   non-OpenBSD CI without errors. Actual logic only runs on OpenBSD.
 *
 * Architecture:
 *   - TCP path: pf divert-to redirects outbound TCP to a local listen
 *     socket. Each accept() yields a new fd whose getsockname returns the
 *     *original destination* of the diverted connection. We allocate a
 *     mux stream_id, send TCP_OPEN, then bridge bytes.
 *
 *   - UDP path (PF mode): pf divert-packet sends raw outbound IPv4+UDP
 *     packets to our IPPROTO_DIVERT raw socket. We parse them with
 *     ipv4_udp_parse, look up / allocate a mux UDP stream per source port,
 *     send UDP_BIND + UDP_PACKET. Replies arriving via mux are wrapped
 *     in synthetic IPv4+UDP (ipv4_udp_build) and write()-ed back to the
 *     same divert socket, which re-injects them into the stack as if
 *     coming from the network — the original local app sees them on its
 *     bound UDP socket.
 *
 *   - UDP path (TUN mode): /dev/tunN in L3 mode. Read IPv4 packets,
 *     filter to UDP, otherwise identical to PF mode. Replies go back
 *     out the tun device. Used as the 04-outbound-intercept.md escape
 *     hatch when divert-packet proves unreliable.
 *
 *   Control frames (HELLO/BYE/PING/PONG/protocol_error) stay with the
 *   caller (panctl/main.c). Stream callbacks are owned here.
 *
 * Stream tables (TCP and UDP both keyed by mux stream_id u16) use flat
 * arrays of 65536 pointers. ~1 MiB total at full population, but typical
 * use is dozens, not thousands; calloc'd once at create time.
 *
 * No threads; single event loop driven by the caller via divert_handle_fd.
 *
 * NOT YET IMPLEMENTED (annotated TODOs):
 *   - Flow control (TCP_WINDOW): pomera-side window accounting. v0
 *     advertises a fixed 64 KiB and trusts the peer to credit back; with
 *     RFCOMM's 1-2 Mbps ceiling, head-of-line within a single TCP stream
 *     is fine.
 *   - Per-stream send-queue / backpressure when mux write blocks. Not
 *     possible without async I/O on the RFCOMM side (BTstack layer).
 *   - UDP idle timeout cleanup: the Android side closes after 120 s,
 *     but pomera-side mapping (local_src_port → stream_id) persists.
 *     We refresh it on UDP_PACKET inbound.
 */

#include "divert.h"
#include "frame.h"
#include "ipv4.h"
#include "mux.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__OpenBSD__)

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <net/if_tun.h>
#include <fcntl.h>
#include <unistd.h>

/* OpenBSD's IPPROTO_DIVERT is in <netinet/in.h>. Confirm at compile time. */
#ifndef IPPROTO_DIVERT
#  error "IPPROTO_DIVERT not defined; this file is OpenBSD-only"
#endif

#define MAX_STREAMS 0x10000

/*
 * OpenBSD tun(4) prepends a 4-byte big-endian address-family header
 * (AF_INET = 2 → 00 00 00 02) to each raw IPv4 frame. We skip it on read
 * and re-add it on write.
 */
#define TUN_AF_HDR 4

struct tcp_stream {
	uint16_t stream_id;
	int      fd;             /* accepted socket */
	int      open_acked;     /* 1 once TCP_OPEN_ACK status OK received */
	int      half_closed_local;  /* we sent TCP_CLOSE_WR */
	int      half_closed_remote; /* peer sent TCP_CLOSE_WR */
};

struct udp_stream {
	uint16_t stream_id;
	uint8_t  local_src_addr[4];
	uint16_t local_src_port;
	int      bind_acked;
};

struct divert {
	struct divert_config cfg;

	int tcp_listen_fd;        /* divert-to AF_INET listen socket */
	int udp_fd;               /* divert-packet RAW socket or tun_fd alias */
	int udp_is_tun;           /* 1 if udp_fd is /dev/tunN, 0 if raw divert */

	divert_tun_other_cb tun_other_cb;
	void *tun_other_cb_ctx;

	/* Stream tables. NULL = unallocated. */
	struct tcp_stream *tcp[MAX_STREAMS];
	struct udp_stream *udp[MAX_STREAMS];

	/*
	 * Reverse lookup: source port → stream_id, used to coalesce
	 * outbound UDP from the same local port onto a single mux stream.
	 * 0 means "no mapping". stream_id is at most 0xFFFF so 16-bit fits.
	 */
	uint16_t udp_port_to_stream[65536];

	uint32_t next_stream_id;  /* monotone, wraps after 0xFFFF */

	uint16_t ipv4_id_counter; /* for IP identification on reply injection */
};

/* ============================ helpers ============================ */

static struct divert *g_divert_for_mux;  /* set during create for callbacks */

static uint16_t
alloc_stream_id(struct divert *d)
{
	for (uint32_t i = 0; i < 0xFFFF; i++) {
		uint16_t id = (uint16_t)((d->next_stream_id + i) & 0xFFFF);
		if (id == 0)
			continue;
		if (d->tcp[id] == NULL && d->udp[id] == NULL) {
			d->next_stream_id = (uint32_t)(id + 1);
			return id;
		}
	}
	return 0;  /* exhausted */
}

static void
tcp_stream_free(struct divert *d, uint16_t stream_id)
{
	struct tcp_stream *s = d->tcp[stream_id];
	if (s == NULL)
		return;
	if (s->fd >= 0)
		close(s->fd);
	free(s);
	d->tcp[stream_id] = NULL;
}

static void
udp_stream_free(struct divert *d, uint16_t stream_id)
{
	struct udp_stream *u = d->udp[stream_id];
	if (u == NULL)
		return;
	/* Reverse map entry. */
	if (d->udp_port_to_stream[u->local_src_port] == stream_id)
		d->udp_port_to_stream[u->local_src_port] = 0;
	free(u);
	d->udp[stream_id] = NULL;
}

/* ============================ mux callbacks ============================ */

static void
cb_tcp_open_ack(void *ctx, uint16_t stream_id, uint16_t status)
{
	struct divert *d = (struct divert *)ctx;
	struct tcp_stream *s = d->tcp[stream_id];
	if (s == NULL)
		return;
	if (status != ACK_STATUS_OK) {
		fprintf(stderr, "tcp[%u] OPEN_ACK status=0x%04x; closing\n",
		    stream_id, status);
		tcp_stream_free(d, stream_id);
		return;
	}
	s->open_acked = 1;
}

static void
cb_tcp_data(void *ctx, uint16_t stream_id, const uint8_t *buf, size_t len)
{
	struct divert *d = (struct divert *)ctx;
	struct tcp_stream *s = d->tcp[stream_id];
	if (s == NULL || s->fd < 0)
		return;
	/*
	 * Write everything to the diverted local socket. send() may not consume
	 * the whole buffer; loop. If the socket is closed, signal RST upstream.
	 */
	size_t off = 0;
	while (off < len) {
		ssize_t n = write(s->fd, buf + off, len - off);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			mux_send_tcp_rst(d->cfg.mux, stream_id, RST_REASON_PEER_RESET);
			tcp_stream_free(d, stream_id);
			return;
		}
		off += (size_t)n;
	}
}

static void
cb_tcp_close_wr(void *ctx, uint16_t stream_id)
{
	struct divert *d = (struct divert *)ctx;
	struct tcp_stream *s = d->tcp[stream_id];
	if (s == NULL || s->fd < 0)
		return;
	shutdown(s->fd, SHUT_WR);
	s->half_closed_remote = 1;
	if (s->half_closed_local)
		tcp_stream_free(d, stream_id);
}

static void
cb_tcp_rst(void *ctx, uint16_t stream_id, uint16_t reason)
{
	struct divert *d = (struct divert *)ctx;
	(void)reason;
	tcp_stream_free(d, stream_id);
}

static void
cb_tcp_window(void *ctx, uint16_t stream_id, uint32_t credit)
{
	/*
	 * v0: we do not enforce send-side flow control. Android initial_win
	 * is 64 KiB and the RFCOMM ceiling (~2 Mbps) means a TCP stream can't
	 * outpace it within ssh/tmux. Future work: per-stream credit accounting.
	 */
	(void)ctx;
	(void)stream_id;
	(void)credit;
}

static void
cb_udp_bind_ack(void *ctx, uint16_t stream_id, uint16_t status)
{
	struct divert *d = (struct divert *)ctx;
	struct udp_stream *u = d->udp[stream_id];
	if (u == NULL)
		return;
	if (status != ACK_STATUS_OK) {
		fprintf(stderr, "udp[%u] BIND_ACK status=0x%04x; closing\n",
		    stream_id, status);
		udp_stream_free(d, stream_id);
		return;
	}
	u->bind_acked = 1;
}

static void
cb_udp_packet(void *ctx, uint16_t stream_id, const struct endpoint *remote,
	      const uint8_t *datagram, size_t len)
{
	struct divert *d = (struct divert *)ctx;
	struct udp_stream *u = d->udp[stream_id];
	if (u == NULL)
		return;
	if (remote->addr_type != ADDR_TYPE_IPV4)
		return;
	/*
	 * Synthesize a UDP packet appearing to come from (remote->addr:port)
	 * destined for (u->local_src_addr:port), and inject via the divert
	 * socket (PF mode) or tun fd (TUN mode).
	 */
	uint8_t pkt[1500 + IPV4_HEADER_MIN + UDP_HEADER_LEN];
	if (len + IPV4_HEADER_MIN + UDP_HEADER_LEN > sizeof pkt)
		return;
	int n = ipv4_udp_build(pkt, sizeof pkt,
	                       remote->addr, remote->port,
	                       u->local_src_addr, u->local_src_port,
	                       d->ipv4_id_counter++, 64,
	                       datagram, len);
	if (n < 0)
		return;

	if (d->udp_is_tun) {
		/* Prepend AF_INET header for OpenBSD tun(4). */
		uint8_t framed[TUN_AF_HDR + sizeof pkt];
		framed[0] = 0; framed[1] = 0; framed[2] = 0; framed[3] = 2; /* AF_INET */
		memcpy(framed + TUN_AF_HDR, pkt, (size_t)n);
		(void)write(d->udp_fd, framed, (size_t)n + TUN_AF_HDR);
	} else {
		/* divert(4) write re-injects into the host stack. The address
		 * passed in sendto is ignored (or used as the divert rule
		 * direction tag in some OpenBSD versions); pass a zero sockaddr
		 * to stay portable across releases. */
		struct sockaddr_in sin = { 0 };
		sin.sin_family = AF_INET;
		(void)sendto(d->udp_fd, pkt, (size_t)n, 0,
		             (struct sockaddr *)&sin, sizeof sin);
	}
}

static void
cb_udp_close(void *ctx, uint16_t stream_id)
{
	struct divert *d = (struct divert *)ctx;
	udp_stream_free(d, stream_id);
}

static void
install_mux_callbacks(struct mux_session *mux, struct divert *d)
{
	/*
	 * We can't install a fresh callback table here because mux_create
	 * already captured a copy. Instead the convention is: caller passes
	 * us a session created with a callbacks struct whose stream-related
	 * fields will be re-pointed by divert_create. Realistically we
	 * recreate the session, but that would discard any in-flight state.
	 *
	 * For v0 we register via a single global indirection: panctl/main.c
	 * creates the session with thin trampolines that look up g_divert_for_mux
	 * and forward. This keeps mux's callback model lock-free and avoids
	 * touching the session struct internals. The trampolines live in
	 * panctl/main.c (TBD).
	 *
	 * Here we just record the divert pointer so the trampolines can find
	 * it. This relies on at most one divert session at a time, which is
	 * true for panctl (one RFCOMM connection at a time).
	 */
	(void)mux;
	g_divert_for_mux = d;
}

/* These are the public trampoline entry points the caller installs into mux. */

void divert_dispatch_tcp_open_ack(void *_ctx, uint16_t sid, uint16_t status) {
	(void)_ctx;
	if (g_divert_for_mux) cb_tcp_open_ack(g_divert_for_mux, sid, status);
}
void divert_dispatch_tcp_data(void *_ctx, uint16_t sid, const uint8_t *b, size_t l) {
	(void)_ctx;
	if (g_divert_for_mux) cb_tcp_data(g_divert_for_mux, sid, b, l);
}
void divert_dispatch_tcp_close_wr(void *_ctx, uint16_t sid) {
	(void)_ctx;
	if (g_divert_for_mux) cb_tcp_close_wr(g_divert_for_mux, sid);
}
void divert_dispatch_tcp_rst(void *_ctx, uint16_t sid, uint16_t reason) {
	(void)_ctx;
	if (g_divert_for_mux) cb_tcp_rst(g_divert_for_mux, sid, reason);
}
void divert_dispatch_tcp_window(void *_ctx, uint16_t sid, uint32_t credit) {
	(void)_ctx;
	if (g_divert_for_mux) cb_tcp_window(g_divert_for_mux, sid, credit);
}
void divert_dispatch_udp_bind_ack(void *_ctx, uint16_t sid, uint16_t status) {
	(void)_ctx;
	if (g_divert_for_mux) cb_udp_bind_ack(g_divert_for_mux, sid, status);
}
void divert_dispatch_udp_packet(void *_ctx, uint16_t sid,
		const struct endpoint *remote, const uint8_t *d, size_t l) {
	(void)_ctx;
	if (g_divert_for_mux) cb_udp_packet(g_divert_for_mux, sid, remote, d, l);
}
void divert_dispatch_udp_close(void *_ctx, uint16_t sid) {
	(void)_ctx;
	if (g_divert_for_mux) cb_udp_close(g_divert_for_mux, sid);
}

/* ============================ TCP setup ============================ */

static int
open_pf_tcp_listen(uint16_t port)
{
	int s = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
	if (s < 0)
		return -1;
	int yes = 1;
	(void)setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

	struct sockaddr_in sin = { 0 };
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sin.sin_port = htons(port);
	if (bind(s, (struct sockaddr *)&sin, sizeof sin) < 0)
		goto err;
	if (listen(s, 64) < 0)
		goto err;
	return s;
err:
	close(s);
	return -1;
}

/* Called when the TCP listen socket has a new connection. */
static int
handle_tcp_accept(struct divert *d)
{
	for (;;) {
		struct sockaddr_in peer;
		socklen_t plen = sizeof peer;
		int afd = accept4(d->tcp_listen_fd, (struct sockaddr *)&peer, &plen,
				  SOCK_NONBLOCK);
		if (afd < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return 0;
			if (errno == EINTR)
				continue;
			return -1;
		}

		/*
		 * Original destination via getsockname on the accepted fd.
		 * OpenBSD pf divert-to preserves the original 5-tuple here.
		 */
		struct sockaddr_in orig;
		socklen_t olen = sizeof orig;
		if (getsockname(afd, (struct sockaddr *)&orig, &olen) < 0) {
			close(afd);
			continue;
		}

		uint16_t sid = alloc_stream_id(d);
		if (sid == 0) {
			close(afd);
			continue;
		}
		struct tcp_stream *s = (struct tcp_stream *)calloc(1, sizeof *s);
		if (s == NULL) {
			close(afd);
			continue;
		}
		s->stream_id = sid;
		s->fd = afd;
		d->tcp[sid] = s;

		struct endpoint ep = { .addr_type = ADDR_TYPE_IPV4, .addr_len = 4 };
		ep.port = ntohs(orig.sin_port);
		memcpy(ep.addr, &orig.sin_addr.s_addr, 4);
		if (mux_send_tcp_open(d->cfg.mux, sid, &ep) < 0) {
			tcp_stream_free(d, sid);
			continue;
		}
	}
}

/* ============================ UDP setup ============================ */

static int
open_pf_udp_divert(uint16_t port)
{
	int s = socket(AF_INET, SOCK_RAW | SOCK_NONBLOCK, IPPROTO_DIVERT);
	if (s < 0)
		return -1;
	struct sockaddr_in sin = { 0 };
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sin.sin_port = htons(port);
	if (bind(s, (struct sockaddr *)&sin, sizeof sin) < 0) {
		close(s);
		return -1;
	}
	return s;
}

static int
open_tun(const char *dev)
{
	int fd = open(dev, O_RDWR | O_NONBLOCK);
	if (fd < 0)
		return -1;
	/*
	 * Default tun(4) on OpenBSD is L3 mode (raw IPv4 frames), no extra
	 * mode byte. If we ever switch to L2 (tap-like), set IFF_LINK0 via
	 * SIOCSIFFLAGS — leave as L3 for v0.
	 */
	return fd;
}

/*
 * Parse one IPv4+UDP packet from the divert/tun socket and forward to mux.
 * Allocates a UDP stream if this source port hasn't been seen yet.
 */
static int
handle_outbound_udp_packet(struct divert *d, const uint8_t *pkt, size_t len)
{
	struct ipv4_udp_view v;
	if (ipv4_udp_parse(pkt, len, &v) != 0)
		return 0;  /* non-UDP or malformed; drop silently */

	uint16_t sid = d->udp_port_to_stream[v.src_port];
	if (sid == 0 || d->udp[sid] == NULL) {
		sid = alloc_stream_id(d);
		if (sid == 0)
			return 0;
		struct udp_stream *u = (struct udp_stream *)calloc(1, sizeof *u);
		if (u == NULL)
			return 0;
		u->stream_id = sid;
		memcpy(u->local_src_addr, v.src_addr, 4);
		u->local_src_port = v.src_port;
		d->udp[sid] = u;
		d->udp_port_to_stream[v.src_port] = sid;
		(void)mux_send_udp_bind(d->cfg.mux, sid);
	}

	struct endpoint remote = { .addr_type = ADDR_TYPE_IPV4, .addr_len = 4 };
	remote.port = v.dst_port;
	memcpy(remote.addr, v.dst_addr, 4);

	(void)mux_send_udp_packet(d->cfg.mux, sid, &remote,
	                          v.datagram, v.datagram_len);
	return 0;
}

static int
handle_udp_fd(struct divert *d)
{
	for (;;) {
		uint8_t buf[2048];
		ssize_t n;
		if (d->udp_is_tun) {
			n = read(d->udp_fd, buf, sizeof buf);
		} else {
			struct sockaddr_in from;
			socklen_t flen = sizeof from;
			n = recvfrom(d->udp_fd, buf, sizeof buf, 0,
				     (struct sockaddr *)&from, &flen);
		}
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return 0;
			if (errno == EINTR)
				continue;
			return -1;
		}
		const uint8_t *ip = buf;
		size_t iplen = (size_t)n;
		if (d->udp_is_tun) {
			if (iplen < TUN_AF_HDR)
				continue;
			/* Drop non-IPv4 (IPv6/CLNP) frames; we only forward v4. */
			uint32_t af = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16)
			            | ((uint32_t)buf[2] << 8) | (uint32_t)buf[3];
			if (af != 2 /* AF_INET */)
				continue;
			ip += TUN_AF_HDR;
			iplen -= TUN_AF_HDR;
		}
		/* Demux by IP protocol on the tun path. */
		if (d->udp_is_tun && iplen >= 20) {
			uint8_t proto = ip[9];
			if (proto != 17 /* UDP */) {
				if (d->tun_other_cb != NULL)
					d->tun_other_cb(d->tun_other_cb_ctx, ip, iplen);
				continue;
			}
		}
		handle_outbound_udp_packet(d, ip, iplen);
	}
}

/* Handle data on an accepted TCP socket (outbound bytes from local app). */
static int
handle_tcp_fd(struct divert *d, int fd, struct tcp_stream *s)
{
	for (;;) {
		uint8_t buf[2048];
		ssize_t n = read(fd, buf, sizeof buf);
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return 0;
			if (errno == EINTR)
				continue;
			mux_send_tcp_rst(d->cfg.mux, s->stream_id, RST_REASON_PEER_RESET);
			tcp_stream_free(d, s->stream_id);
			return 0;
		}
		if (n == 0) {
			mux_send_tcp_close_wr(d->cfg.mux, s->stream_id);
			s->half_closed_local = 1;
			if (s->half_closed_remote)
				tcp_stream_free(d, s->stream_id);
			return 0;
		}
		if (mux_send_tcp_data(d->cfg.mux, s->stream_id, buf, (size_t)n) < 0) {
			mux_send_tcp_rst(d->cfg.mux, s->stream_id, RST_REASON_OTHER);
			tcp_stream_free(d, s->stream_id);
			return -1;
		}
	}
}

/* ============================ public API ============================ */

struct divert *
divert_create(const struct divert_config *cfg)
{
	if (cfg == NULL || cfg->mux == NULL)
		return NULL;
	struct divert *d = (struct divert *)calloc(1, sizeof *d);
	if (d == NULL)
		return NULL;
	d->cfg = *cfg;
	d->tcp_listen_fd = -1;
	d->udp_fd = -1;
	d->udp_is_tun = (cfg->udp_mode == DIVERT_UDP_MODE_TUN) ? 1 : 0;

	d->tcp_listen_fd = open_pf_tcp_listen(cfg->pf_tcp_port);
	if (d->tcp_listen_fd < 0)
		goto err;

	if (cfg->udp_mode == DIVERT_UDP_MODE_PF) {
		d->udp_fd = open_pf_udp_divert(cfg->pf_udp_port);
	} else {
		d->udp_fd = open_tun(cfg->tun_dev ? cfg->tun_dev : "/dev/tun0");
	}
	if (d->udp_fd < 0)
		goto err;

	install_mux_callbacks(cfg->mux, d);
	d->next_stream_id = 1;
	d->ipv4_id_counter = 1;
	return d;
err:
	divert_destroy(d);
	return NULL;
}

void
divert_destroy(struct divert *d)
{
	if (d == NULL)
		return;
	if (g_divert_for_mux == d)
		g_divert_for_mux = NULL;
	if (d->tcp_listen_fd >= 0)
		close(d->tcp_listen_fd);
	if (d->udp_fd >= 0)
		close(d->udp_fd);
	for (size_t i = 0; i < MAX_STREAMS; i++) {
		if (d->tcp[i] != NULL) {
			if (d->tcp[i]->fd >= 0)
				close(d->tcp[i]->fd);
			free(d->tcp[i]);
		}
		free(d->udp[i]);
	}
	free(d);
}

int
divert_get_fds(struct divert *d, int *out, size_t cap, size_t *n)
{
	size_t k = 0;
	if (cap >= 1) out[k++] = d->tcp_listen_fd;
	if (cap >= 2) out[k++] = d->udp_fd;
	/*
	 * Accepted TCP fds are not in this list. Caller polls these two
	 * for new connections / UDP packets; accepted TCP sockets are
	 * iterated by divert_each_active_tcp_fd (TBD) when polling. For
	 * v0 the caller may simply call divert_handle_active_tcp_fds()
	 * after every wakeup; cheap because most streams idle.
	 */
	if (n) *n = k;
	return (cap >= 2) ? 0 : -1;
}

/*
 * For panctl/main.c convenience: invoke handle_tcp_fd on every active
 * TCP stream. Non-blocking sockets return EAGAIN quickly so the sweep
 * is O(active streams). Not optimal for thousands of streams; fine for
 * pomera scale (dozens).
 */
void
divert_drain_active_tcp(struct divert *d)
{
	for (size_t i = 1; i < MAX_STREAMS; i++) {
		struct tcp_stream *s = d->tcp[i];
		if (s == NULL || s->fd < 0)
			continue;
		if (!s->open_acked)
			continue;
		(void)handle_tcp_fd(d, s->fd, s);
	}
}

int
divert_handle_fd(struct divert *d, int fd)
{
	if (fd == d->tcp_listen_fd)
		return handle_tcp_accept(d);
	if (fd == d->udp_fd)
		return handle_udp_fd(d);

	/* fd belongs to an accepted TCP stream? Linear scan; small N. */
	for (size_t i = 1; i < MAX_STREAMS; i++) {
		struct tcp_stream *s = d->tcp[i];
		if (s != NULL && s->fd == fd)
			return handle_tcp_fd(d, fd, s);
	}
	return -1;
}

int
divert_get_tun_fd(const struct divert *d)
{
	return (d != NULL && d->udp_is_tun) ? d->udp_fd : -1;
}

void
divert_set_tun_other_cb(struct divert *d, divert_tun_other_cb cb, void *ctx)
{
	if (d == NULL)
		return;
	d->tun_other_cb = cb;
	d->tun_other_cb_ctx = ctx;
}

#else  /* !__OpenBSD__ */

/*
 * On non-OpenBSD platforms divert_create returns NULL with errno=ENOSYS.
 * Allows panctl to compile against the same divert.h header for planning
 * and CI on developer machines.
 */

struct divert {
	int unused;
};

struct divert *
divert_create(const struct divert_config *cfg)
{
	(void)cfg;
	errno = ENOSYS;
	return NULL;
}

void
divert_destroy(struct divert *d)
{
	(void)d;
}

int
divert_get_fds(struct divert *d, int *out, size_t cap, size_t *n)
{
	(void)d; (void)out; (void)cap;
	if (n) *n = 0;
	return 0;
}

int
divert_handle_fd(struct divert *d, int fd)
{
	(void)d; (void)fd;
	return -1;
}

int
divert_get_tun_fd(const struct divert *d) { (void)d; return -1; }

void
divert_set_tun_other_cb(struct divert *d, divert_tun_other_cb cb, void *ctx)
{ (void)d; (void)cb; (void)ctx; }

#endif  /* __OpenBSD__ */
