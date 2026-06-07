/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 4noha */
/*
 * panctl/mux.h — RFCOMM mux session (frame reassembly + dispatch + send).
 *
 * This layer sits between frame.c (pure encoding) and whatever transports
 * RFCOMM bytes (BTstack callback, a test mock, a pipe). The transport calls
 * mux_feed(bytes) on inbound; the upper layer (panctl main + divert) calls
 * mux_send_* on outbound. Frame reassembly across partial reads happens
 * inside this layer, so callers don't need to buffer themselves.
 *
 * No threading primitives, no malloc beyond the session itself + payload
 * reassembly buffer. The caller is expected to serialize calls (BTstack
 * runs single-threaded over the run loop, panctl will too).
 */
#ifndef PANCTL_MUX_H
#define PANCTL_MUX_H

#include "frame.h"

#include <stddef.h>
#include <stdint.h>

struct mux_session; /* opaque */

/*
 * Callback table. Caller owns ctx. Inbound handlers may be left NULL if the
 * caller doesn't care about that event type — mux.c will simply skip the
 * callback (but still process the frame and advance the reader).
 *
 * write() is the only mandatory field: it transports outbound bytes onto
 * RFCOMM. Returning a value != (ssize_t)len signals a transport error and
 * mux_send_* will propagate -1.
 */
struct mux_callbacks {
	void *ctx;
	/* Outbound bytes to RFCOMM. Returns bytes written; <0 on error. */
	long (*write)(void *ctx, const uint8_t *buf, size_t len);

	/* Control frames (PROTOCOL.md §3.1-3.3). */
	void (*on_hello)(void *ctx, const struct hello_payload *h);
	void (*on_bye)(void *ctx, uint16_t reason);
	void (*on_ping)(void *ctx, const uint8_t *nonce, size_t len);
	void (*on_pong)(void *ctx, const uint8_t *nonce, size_t len);

	/* TCP frames (PROTOCOL.md §3.4-3.9). */
	void (*on_tcp_open_ack)(void *ctx, uint16_t stream_id, uint16_t status);
	void (*on_tcp_data)(void *ctx, uint16_t stream_id,
			   const uint8_t *buf, size_t len);
	void (*on_tcp_close_wr)(void *ctx, uint16_t stream_id);
	void (*on_tcp_rst)(void *ctx, uint16_t stream_id, uint16_t reason);
	void (*on_tcp_window)(void *ctx, uint16_t stream_id, uint32_t credit);

	/* UDP frames (PROTOCOL.md §3.10-3.13). */
	void (*on_udp_bind_ack)(void *ctx, uint16_t stream_id, uint16_t status);
	void (*on_udp_packet)(void *ctx, uint16_t stream_id,
			      const struct endpoint *remote,
			      const uint8_t *datagram, size_t len);
	void (*on_udp_close)(void *ctx, uint16_t stream_id);

	/*
	 * Protocol violation observed (unknown type, version mismatch, malformed
	 * payload). PROTOCOL.md §2 mandates BYE + RFCOMM close on these; the
	 * mux layer itself does not send BYE — the caller must, because it owns
	 * the RFCOMM lifecycle.
	 */
	void (*on_protocol_error)(void *ctx, const char *what);
};

/*
 * Create / destroy a session. Returns NULL on allocation failure.
 * The callbacks pointer is copied; caller may free the struct after this.
 */
struct mux_session *mux_create(const struct mux_callbacks *cb);
void mux_destroy(struct mux_session *m);

/*
 * Push inbound bytes from the transport. May invoke zero or more callbacks
 * in order. Returns 0 on success, -1 on protocol violation (caller should
 * tear down the session). Allocation failure during payload buffer growth
 * is reported via on_protocol_error and returns -1.
 */
int mux_feed(struct mux_session *m, const uint8_t *buf, size_t len);

/*
 * Send-side API. Each function builds one frame and writes it via the
 * `write` callback. Returns 0 on success, -1 if the callback returned a
 * short write or error. Callers must hold whatever lock guards the write
 * direction (the layer above mux is responsible for serialization).
 */
int mux_send_hello(struct mux_session *m, const struct hello_payload *h);
int mux_send_bye(struct mux_session *m, uint16_t reason);
int mux_send_ping(struct mux_session *m, const uint8_t *nonce, size_t nonce_len);
int mux_send_pong(struct mux_session *m, const uint8_t *nonce, size_t nonce_len);

int mux_send_tcp_open(struct mux_session *m, uint16_t stream_id,
		      const struct endpoint *dst);
int mux_send_tcp_open_ack(struct mux_session *m, uint16_t stream_id,
			  uint16_t status);
int mux_send_tcp_data(struct mux_session *m, uint16_t stream_id,
		      const uint8_t *buf, size_t len);
int mux_send_tcp_close_wr(struct mux_session *m, uint16_t stream_id);
int mux_send_tcp_rst(struct mux_session *m, uint16_t stream_id, uint16_t reason);
int mux_send_tcp_window(struct mux_session *m, uint16_t stream_id, uint32_t credit);

int mux_send_udp_bind(struct mux_session *m, uint16_t stream_id);
int mux_send_udp_bind_ack(struct mux_session *m, uint16_t stream_id, uint16_t status);
int mux_send_udp_packet(struct mux_session *m, uint16_t stream_id,
			const struct endpoint *remote,
			const uint8_t *datagram, size_t len);
int mux_send_udp_close(struct mux_session *m, uint16_t stream_id);

#endif /* PANCTL_MUX_H */
