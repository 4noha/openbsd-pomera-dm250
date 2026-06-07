/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 4noha */
/*
 * panctl/mux.c — frame reassembly and dispatch + outbound send.
 *
 * Inbound state machine:
 *   READ_HEADER  (filling hdr_buf[0..6))
 *   READ_PAYLOAD (filling payload[0..hdr.length))
 *
 * On a complete frame, dispatch via the registered callback and reset to
 * READ_HEADER. Partial bytes are remembered across mux_feed calls.
 */
#include "mux.h"
#include "frame.h"

#include <stdlib.h>
#include <string.h>

enum mux_phase {
	MUX_READ_HEADER,
	MUX_READ_PAYLOAD,
};

struct mux_session {
	struct mux_callbacks cb;

	enum mux_phase phase;
	uint8_t  hdr_buf[FRAME_HEADER_SIZE];
	size_t   hdr_filled;

	struct frame_hdr current;

	uint8_t *payload;
	size_t   payload_cap;
	size_t   payload_filled;
};

static void
report_error(struct mux_session *m, const char *what)
{
	if (m->cb.on_protocol_error != NULL)
		m->cb.on_protocol_error(m->cb.ctx, what);
}

static int
ensure_payload_cap(struct mux_session *m, size_t need)
{
	if (need <= m->payload_cap)
		return 0;

	/* Grow geometrically up to FRAME_MAX_PAYLOAD. */
	size_t cap = m->payload_cap ? m->payload_cap : 256;
	while (cap < need)
		cap *= 2;
	if (cap > (size_t)FRAME_MAX_PAYLOAD)
		cap = (size_t)FRAME_MAX_PAYLOAD;
	if (cap < need)
		return -1;

	uint8_t *p = (uint8_t *)realloc(m->payload, cap);
	if (p == NULL)
		return -1;
	m->payload = p;
	m->payload_cap = cap;
	return 0;
}

struct mux_session *
mux_create(const struct mux_callbacks *cb)
{
	if (cb == NULL || cb->write == NULL)
		return NULL;
	struct mux_session *m = (struct mux_session *)calloc(1, sizeof *m);
	if (m == NULL)
		return NULL;
	m->cb = *cb;
	m->phase = MUX_READ_HEADER;
	return m;
}

void
mux_destroy(struct mux_session *m)
{
	if (m == NULL)
		return;
	free(m->payload);
	free(m);
}

/*
 * Dispatch a completed frame. Returns 0 normally, -1 if dispatch detected
 * a protocol violation (e.g. unknown type, malformed payload, version
 * mismatch).
 */
static int
dispatch_frame(struct mux_session *m)
{
	const struct frame_hdr *h = &m->current;
	const uint8_t *p = m->payload;
	size_t len = (size_t)h->length;

	if (h->ver != PROTO_VERSION) {
		report_error(m, "version mismatch");
		return -1;
	}

	switch (h->type) {
	case FRAME_TYPE_HELLO: {
		struct hello_payload hp;
		if (hello_decode(p, len, &hp) != 0) {
			report_error(m, "malformed HELLO");
			return -1;
		}
		if (m->cb.on_hello != NULL)
			m->cb.on_hello(m->cb.ctx, &hp);
		return 0;
	}
	case FRAME_TYPE_BYE: {
		uint16_t reason = 0;
		if (len >= 2)
			reason = read_u16_be(p);
		if (m->cb.on_bye != NULL)
			m->cb.on_bye(m->cb.ctx, reason);
		return 0;
	}
	case FRAME_TYPE_PING:
		if (m->cb.on_ping != NULL)
			m->cb.on_ping(m->cb.ctx, p, len);
		return 0;
	case FRAME_TYPE_PONG:
		if (m->cb.on_pong != NULL)
			m->cb.on_pong(m->cb.ctx, p, len);
		return 0;

	case FRAME_TYPE_TCP_OPEN_ACK:
		if (len < 2) {
			report_error(m, "TCP_OPEN_ACK too short");
			return -1;
		}
		if (m->cb.on_tcp_open_ack != NULL)
			m->cb.on_tcp_open_ack(m->cb.ctx, h->stream_id,
					       read_u16_be(p));
		return 0;
	case FRAME_TYPE_TCP_DATA:
		if (len == 0) {
			report_error(m, "TCP_DATA empty");
			return -1;
		}
		if (m->cb.on_tcp_data != NULL)
			m->cb.on_tcp_data(m->cb.ctx, h->stream_id, p, len);
		return 0;
	case FRAME_TYPE_TCP_CLOSE_WR:
		if (m->cb.on_tcp_close_wr != NULL)
			m->cb.on_tcp_close_wr(m->cb.ctx, h->stream_id);
		return 0;
	case FRAME_TYPE_TCP_RST: {
		uint16_t reason = 0;
		if (len >= 2)
			reason = read_u16_be(p);
		if (m->cb.on_tcp_rst != NULL)
			m->cb.on_tcp_rst(m->cb.ctx, h->stream_id, reason);
		return 0;
	}
	case FRAME_TYPE_TCP_WINDOW:
		if (len < 4) {
			report_error(m, "TCP_WINDOW too short");
			return -1;
		}
		if (m->cb.on_tcp_window != NULL)
			m->cb.on_tcp_window(m->cb.ctx, h->stream_id,
					    read_u32_be(p));
		return 0;

	case FRAME_TYPE_UDP_BIND_ACK:
		if (len < 2) {
			report_error(m, "UDP_BIND_ACK too short");
			return -1;
		}
		if (m->cb.on_udp_bind_ack != NULL)
			m->cb.on_udp_bind_ack(m->cb.ctx, h->stream_id,
					      read_u16_be(p));
		return 0;
	case FRAME_TYPE_UDP_PACKET: {
		struct endpoint ep;
		size_t consumed = 0;
		if (endpoint_decode(p, len, 0, &ep, &consumed) != 0) {
			report_error(m, "UDP_PACKET endpoint malformed");
			return -1;
		}
		const uint8_t *dg = p + consumed;
		size_t dg_len = len - consumed;
		if (m->cb.on_udp_packet != NULL)
			m->cb.on_udp_packet(m->cb.ctx, h->stream_id, &ep, dg, dg_len);
		return 0;
	}
	case FRAME_TYPE_UDP_CLOSE:
		if (m->cb.on_udp_close != NULL)
			m->cb.on_udp_close(m->cb.ctx, h->stream_id);
		return 0;

	/*
	 * Frames that, per PROTOCOL.md §3, only flow C→S (pomera→Android).
	 * Receiving these on pomera is a protocol violation by Android.
	 * Same for unknown types.
	 */
	case FRAME_TYPE_TCP_OPEN:
	case FRAME_TYPE_UDP_BIND:
		report_error(m, "unexpected C->S frame from server");
		return -1;
	default:
		report_error(m, "unknown frame type");
		return -1;
	}
}

int
mux_feed(struct mux_session *m, const uint8_t *buf, size_t len)
{
	size_t i = 0;

	while (i < len) {
		if (m->phase == MUX_READ_HEADER) {
			size_t want = FRAME_HEADER_SIZE - m->hdr_filled;
			size_t avail = len - i;
			size_t take = (avail < want) ? avail : want;
			memcpy(m->hdr_buf + m->hdr_filled, buf + i, take);
			m->hdr_filled += take;
			i += take;

			if (m->hdr_filled < FRAME_HEADER_SIZE)
				return 0;

			frame_hdr_decode(m->hdr_buf, &m->current);
			m->hdr_filled = 0;
			m->payload_filled = 0;

			if (m->current.length > 0) {
				if (ensure_payload_cap(m, m->current.length) != 0) {
					report_error(m, "payload alloc failed");
					return -1;
				}
				m->phase = MUX_READ_PAYLOAD;
			} else {
				if (dispatch_frame(m) != 0)
					return -1;
				/* phase already MUX_READ_HEADER */
			}
		} else { /* MUX_READ_PAYLOAD */
			size_t want = (size_t)m->current.length - m->payload_filled;
			size_t avail = len - i;
			size_t take = (avail < want) ? avail : want;
			memcpy(m->payload + m->payload_filled, buf + i, take);
			m->payload_filled += take;
			i += take;

			if (m->payload_filled < (size_t)m->current.length)
				return 0;

			if (dispatch_frame(m) != 0)
				return -1;
			m->phase = MUX_READ_HEADER;
		}
	}
	return 0;
}

/*
 * Outbound send helpers. All build the frame in an on-stack buffer
 * (header + small payload) or a heap buffer (TCP_DATA / UDP_PACKET
 * which can be up to 64 KiB). The single write() callback gets the
 * whole frame in one call so RFCOMM SAR doesn't interleave with other
 * streams (the layer above mux serializes writes).
 */

static int
write_frame_small(struct mux_session *m, uint8_t type, uint16_t stream_id,
		  const uint8_t *payload, uint16_t payload_len)
{
	uint8_t buf[FRAME_HEADER_SIZE + 64];
	if (payload_len > 64) {
		/* Caller must use write_frame_alloc(). */
		return -1;
	}
	frame_hdr_encode(buf, type, stream_id, payload_len);
	if (payload_len > 0)
		memcpy(buf + FRAME_HEADER_SIZE, payload, payload_len);
	size_t total = (size_t)FRAME_HEADER_SIZE + payload_len;
	long n = m->cb.write(m->cb.ctx, buf, total);
	return (n == (long)total) ? 0 : -1;
}

static int
write_frame_alloc(struct mux_session *m, uint8_t type, uint16_t stream_id,
		  const uint8_t *p1, size_t p1_len,
		  const uint8_t *p2, size_t p2_len)
{
	size_t payload_len = p1_len + p2_len;
	if (payload_len > (size_t)FRAME_MAX_PAYLOAD)
		return -1;
	size_t total = (size_t)FRAME_HEADER_SIZE + payload_len;
	uint8_t *buf = (uint8_t *)malloc(total);
	if (buf == NULL)
		return -1;
	frame_hdr_encode(buf, type, stream_id, (uint16_t)payload_len);
	if (p1_len > 0)
		memcpy(buf + FRAME_HEADER_SIZE, p1, p1_len);
	if (p2_len > 0)
		memcpy(buf + FRAME_HEADER_SIZE + p1_len, p2, p2_len);
	long n = m->cb.write(m->cb.ctx, buf, total);
	free(buf);
	return (n == (long)total) ? 0 : -1;
}

int
mux_send_hello(struct mux_session *m, const struct hello_payload *h)
{
	uint8_t p[6];
	hello_encode(p, h);
	return write_frame_small(m, FRAME_TYPE_HELLO, 0, p, sizeof p);
}

int
mux_send_bye(struct mux_session *m, uint16_t reason)
{
	uint8_t p[2];
	write_u16_be(p, reason);
	return write_frame_small(m, FRAME_TYPE_BYE, 0, p, sizeof p);
}

int
mux_send_ping(struct mux_session *m, const uint8_t *nonce, size_t nonce_len)
{
	if (nonce_len > 64)
		return write_frame_alloc(m, FRAME_TYPE_PING, 0, nonce, nonce_len, NULL, 0);
	return write_frame_small(m, FRAME_TYPE_PING, 0, nonce, (uint16_t)nonce_len);
}

int
mux_send_pong(struct mux_session *m, const uint8_t *nonce, size_t nonce_len)
{
	if (nonce_len > 64)
		return write_frame_alloc(m, FRAME_TYPE_PONG, 0, nonce, nonce_len, NULL, 0);
	return write_frame_small(m, FRAME_TYPE_PONG, 0, nonce, (uint16_t)nonce_len);
}

int
mux_send_tcp_open(struct mux_session *m, uint16_t stream_id,
		  const struct endpoint *dst)
{
	uint8_t p[1 + 2 + 1 + 16];
	size_t n = 0;
	if (endpoint_encode(p, sizeof p, dst, &n) != 0)
		return -1;
	return write_frame_small(m, FRAME_TYPE_TCP_OPEN, stream_id, p, (uint16_t)n);
}

int
mux_send_tcp_open_ack(struct mux_session *m, uint16_t stream_id, uint16_t status)
{
	uint8_t p[2];
	write_u16_be(p, status);
	return write_frame_small(m, FRAME_TYPE_TCP_OPEN_ACK, stream_id, p, sizeof p);
}

int
mux_send_tcp_data(struct mux_session *m, uint16_t stream_id,
		  const uint8_t *buf, size_t len)
{
	if (len == 0 || len > (size_t)FRAME_MAX_PAYLOAD)
		return -1;
	return write_frame_alloc(m, FRAME_TYPE_TCP_DATA, stream_id, buf, len, NULL, 0);
}

int
mux_send_tcp_close_wr(struct mux_session *m, uint16_t stream_id)
{
	return write_frame_small(m, FRAME_TYPE_TCP_CLOSE_WR, stream_id, NULL, 0);
}

int
mux_send_tcp_rst(struct mux_session *m, uint16_t stream_id, uint16_t reason)
{
	uint8_t p[2];
	write_u16_be(p, reason);
	return write_frame_small(m, FRAME_TYPE_TCP_RST, stream_id, p, sizeof p);
}

int
mux_send_tcp_window(struct mux_session *m, uint16_t stream_id, uint32_t credit)
{
	uint8_t p[4];
	write_u32_be(p, credit);
	return write_frame_small(m, FRAME_TYPE_TCP_WINDOW, stream_id, p, sizeof p);
}

int
mux_send_udp_bind(struct mux_session *m, uint16_t stream_id)
{
	return write_frame_small(m, FRAME_TYPE_UDP_BIND, stream_id, NULL, 0);
}

int
mux_send_udp_bind_ack(struct mux_session *m, uint16_t stream_id, uint16_t status)
{
	uint8_t p[2];
	write_u16_be(p, status);
	return write_frame_small(m, FRAME_TYPE_UDP_BIND_ACK, stream_id, p, sizeof p);
}

int
mux_send_udp_packet(struct mux_session *m, uint16_t stream_id,
		    const struct endpoint *remote,
		    const uint8_t *datagram, size_t len)
{
	uint8_t ep_buf[1 + 2 + 1 + 16];
	size_t ep_len = 0;
	if (endpoint_encode(ep_buf, sizeof ep_buf, remote, &ep_len) != 0)
		return -1;
	return write_frame_alloc(m, FRAME_TYPE_UDP_PACKET, stream_id,
				 ep_buf, ep_len, datagram, len);
}

int
mux_send_udp_close(struct mux_session *m, uint16_t stream_id)
{
	return write_frame_small(m, FRAME_TYPE_UDP_CLOSE, stream_id, NULL, 0);
}
