/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 4noha */
/*
 * panctl/frame.h — mux protocol wire format (PROTOCOL.md v0/draft)
 *
 * Pure encoding/decoding only. No I/O, no state machine, no allocation
 * beyond the caller-provided buffers. Mirrors android_app's
 * mux/Frame.kt so that the two implementations can be diffed byte-for-byte
 * against PROTOCOL.md §9 test vectors.
 */
#ifndef PANCTL_FRAME_H
#define PANCTL_FRAME_H

#include <stddef.h>
#include <stdint.h>

/* Current mux protocol version. PROTOCOL.md §2. */
#define PROTO_VERSION ((uint8_t)0x00)

/* PROTOCOL.md §3 frame types. */
#define FRAME_TYPE_HELLO         ((uint8_t)0x01)
#define FRAME_TYPE_BYE           ((uint8_t)0x02)
#define FRAME_TYPE_PING          ((uint8_t)0x03)
#define FRAME_TYPE_PONG          ((uint8_t)0x04)
#define FRAME_TYPE_TCP_OPEN      ((uint8_t)0x10)
#define FRAME_TYPE_TCP_OPEN_ACK  ((uint8_t)0x11)
#define FRAME_TYPE_TCP_DATA      ((uint8_t)0x12)
#define FRAME_TYPE_TCP_CLOSE_WR  ((uint8_t)0x13)
#define FRAME_TYPE_TCP_RST       ((uint8_t)0x14)
#define FRAME_TYPE_TCP_WINDOW    ((uint8_t)0x15)
#define FRAME_TYPE_UDP_BIND      ((uint8_t)0x20)
#define FRAME_TYPE_UDP_BIND_ACK  ((uint8_t)0x21)
#define FRAME_TYPE_UDP_PACKET    ((uint8_t)0x22)
#define FRAME_TYPE_UDP_CLOSE     ((uint8_t)0x23)

/* PROTOCOL.md §3.4 address types. v0 emits only IPv4. */
#define ADDR_TYPE_IPV4    ((uint8_t)0x01)
#define ADDR_TYPE_IPV6    ((uint8_t)0x02)
#define ADDR_TYPE_DOMAIN  ((uint8_t)0x03)

/* PROTOCOL.md §3.5 ack status codes. */
#define ACK_STATUS_OK            ((uint16_t)0x0000)
#define ACK_STATUS_ECONNREFUSED  ((uint16_t)0x0001)
#define ACK_STATUS_EHOSTUNREACH  ((uint16_t)0x0002)
#define ACK_STATUS_ETIMEDOUT     ((uint16_t)0x0003)
#define ACK_STATUS_EAFNOSUPPORT  ((uint16_t)0x0004)
#define ACK_STATUS_EOTHER        ((uint16_t)0x00FF)

/* PROTOCOL.md §3.8 TCP_RST reason codes. */
#define RST_REASON_PEER_RESET     ((uint16_t)0x0001)
#define RST_REASON_IDLE_TIMEOUT   ((uint16_t)0x0002)
#define RST_REASON_FLOW_VIOLATION ((uint16_t)0x0003)
#define RST_REASON_OTHER          ((uint16_t)0x00FF)

/* PROTOCOL.md §2: header is 6 bytes. */
#define FRAME_HEADER_SIZE  6
#define FRAME_MAX_PAYLOAD  65535

/* Decoded frame header. The payload itself stays in the caller's buffer. */
struct frame_hdr {
	uint8_t  ver;
	uint8_t  type;
	uint16_t stream_id;
	uint16_t length;
};

/* HELLO payload (PROTOCOL.md §3.1) — 6 bytes. */
struct hello_payload {
	uint8_t  proto_ver;
	uint8_t  flags;
	uint16_t max_streams;
	uint16_t initial_win_kib;
};

/* Address + port endpoint (TCP_OPEN §3.4, UDP_PACKET §3.12). */
struct endpoint {
	uint8_t  addr_type;
	uint16_t port;
	uint8_t  addr_len;
	uint8_t  addr[16];  /* IPv4: 4, IPv6: 16. DOMAIN deferred. */
};

/* Big-endian scalar helpers. */
uint16_t read_u16_be(const uint8_t *buf);
uint32_t read_u32_be(const uint8_t *buf);
void     write_u16_be(uint8_t *buf, uint16_t v);
void     write_u32_be(uint8_t *buf, uint32_t v);

/*
 * Write a 6-byte frame header into buf. The caller must guarantee buf has
 * at least FRAME_HEADER_SIZE bytes. Returns 0.
 */
int frame_hdr_encode(uint8_t *buf, uint8_t type, uint16_t stream_id,
		     uint16_t length);

/*
 * Parse a 6-byte header. The caller must guarantee buf has at least
 * FRAME_HEADER_SIZE bytes.
 */
void frame_hdr_decode(const uint8_t *buf, struct frame_hdr *out);

void hello_encode(uint8_t *buf, const struct hello_payload *h);
/* Returns 0 on success, -1 if len != 6. */
int  hello_decode(const uint8_t *buf, size_t len, struct hello_payload *out);

/*
 * Encode endpoint into [buf, buf+cap). Writes addr_type|port_be|addr_len|addr.
 * On success, sets *out_len to bytes written (1+2+1+addr_len) and returns 0.
 * Returns -1 if cap is insufficient or addr_len out of bounds.
 */
int endpoint_encode(uint8_t *buf, size_t cap, const struct endpoint *ep,
		    size_t *out_len);

/*
 * Decode endpoint starting at buf[offset]. On success, *consumed is set to
 * 4+addr_len and returns 0. Returns -1 on truncation or addr_len overflow.
 */
int endpoint_decode(const uint8_t *buf, size_t len, size_t offset,
		    struct endpoint *out, size_t *consumed);

#endif  /* PANCTL_FRAME_H */
