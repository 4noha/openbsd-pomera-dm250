/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 4noha */
/*
 * panctl/test_mux.c — unit tests for the mux session layer.
 *
 * Verifies:
 *   - mux_send_* produces the same bytes as PROTOCOL.md §9 / android_app
 *   - mux_feed reassembles frames across arbitrarily small chunks
 *   - dispatch invokes the right callback with decoded values
 *   - protocol violations are surfaced via on_protocol_error
 *
 * Stays in pure userspace memory: a mock "RFCOMM" buffer is the only
 * transport. No BTstack, no sockets, no pf.
 */
#include "mux.h"
#include "frame.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failed;

#define EXPECT(cond, msg) do {                                          \
	if (!(cond)) {                                                  \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); \
		g_failed = 1;                                           \
		return;                                                 \
	}                                                               \
} while (0)

static void
hex_print(FILE *f, const char *label, const uint8_t *buf, size_t len)
{
	fprintf(f, "%s", label);
	for (size_t i = 0; i < len; i++)
		fprintf(f, "%02X%s", buf[i], (i + 1 == len) ? "" : " ");
	fprintf(f, "\n");
}

#define EXPECT_BYTES(got, got_len, want, want_len, label) do {          \
	if ((got_len) != (want_len) || memcmp((got), (want), (want_len)) != 0) { \
		fprintf(stderr, "FAIL %s:%d: %s (got=%zu want=%zu)\n",  \
			__FILE__, __LINE__, label,                      \
			(size_t)(got_len), (size_t)(want_len));         \
		hex_print(stderr, "  got:  ", (got), (got_len));        \
		hex_print(stderr, "  want: ", (want), (want_len));      \
		g_failed = 1;                                           \
		return;                                                 \
	}                                                               \
} while (0)

/*
 * Mock transport: write() appends to a buffer; "feed" calls splice the
 * inbound side. Mock also collects dispatch events into a structured log
 * so tests can assert on order + values without callback-per-test boilerplate.
 */
struct mock {
	uint8_t  outbuf[8192];
	size_t   outlen;

	/* Event log: each entry is {tag, stream_id, scalar, body...}. */
	struct event {
		const char *tag;
		uint16_t stream_id;
		uint32_t scalar;   /* status, reason, credit, etc. */
		struct hello_payload hello;
		struct endpoint endpoint;
		uint8_t  body[2048];
		size_t   body_len;
	} events[32];
	size_t event_count;

	int  protocol_errors;
	const char *last_error;

	/* Force write() to short-write for one call to exercise -1 paths. */
	int  force_short_write;
};

static long
mock_write(void *ctx, const uint8_t *buf, size_t len)
{
	struct mock *m = (struct mock *)ctx;
	if (m->force_short_write) {
		m->force_short_write = 0;
		return (long)(len > 0 ? len - 1 : 0);
	}
	if (m->outlen + len > sizeof m->outbuf)
		return -1;
	memcpy(m->outbuf + m->outlen, buf, len);
	m->outlen += len;
	return (long)len;
}

static struct event *
push_event(struct mock *m, const char *tag, uint16_t stream_id)
{
	if (m->event_count >= sizeof m->events / sizeof m->events[0]) {
		fprintf(stderr, "event log overflow\n");
		exit(2);
	}
	struct event *e = &m->events[m->event_count++];
	memset(e, 0, sizeof *e);
	e->tag = tag;
	e->stream_id = stream_id;
	return e;
}

static void mock_hello(void *ctx, const struct hello_payload *h) {
	push_event((struct mock *)ctx, "HELLO", 0)->hello = *h;
}
static void mock_bye(void *ctx, uint16_t reason) {
	push_event((struct mock *)ctx, "BYE", 0)->scalar = reason;
}
static void mock_ping(void *ctx, const uint8_t *nonce, size_t len) {
	struct event *e = push_event((struct mock *)ctx, "PING", 0);
	if (len > sizeof e->body) len = sizeof e->body;
	memcpy(e->body, nonce, len);
	e->body_len = len;
}
static void mock_pong(void *ctx, const uint8_t *nonce, size_t len) {
	struct event *e = push_event((struct mock *)ctx, "PONG", 0);
	if (len > sizeof e->body) len = sizeof e->body;
	memcpy(e->body, nonce, len);
	e->body_len = len;
}
static void mock_tcp_open_ack(void *ctx, uint16_t sid, uint16_t status) {
	push_event((struct mock *)ctx, "TCP_OPEN_ACK", sid)->scalar = status;
}
static void mock_tcp_data(void *ctx, uint16_t sid, const uint8_t *buf, size_t len) {
	struct event *e = push_event((struct mock *)ctx, "TCP_DATA", sid);
	if (len > sizeof e->body) len = sizeof e->body;
	memcpy(e->body, buf, len);
	e->body_len = len;
}
static void mock_tcp_close_wr(void *ctx, uint16_t sid) {
	push_event((struct mock *)ctx, "TCP_CLOSE_WR", sid);
}
static void mock_tcp_rst(void *ctx, uint16_t sid, uint16_t reason) {
	push_event((struct mock *)ctx, "TCP_RST", sid)->scalar = reason;
}
static void mock_tcp_window(void *ctx, uint16_t sid, uint32_t credit) {
	push_event((struct mock *)ctx, "TCP_WINDOW", sid)->scalar = credit;
}
static void mock_udp_bind_ack(void *ctx, uint16_t sid, uint16_t status) {
	push_event((struct mock *)ctx, "UDP_BIND_ACK", sid)->scalar = status;
}
static void mock_udp_packet(void *ctx, uint16_t sid, const struct endpoint *remote,
			    const uint8_t *datagram, size_t len) {
	struct event *e = push_event((struct mock *)ctx, "UDP_PACKET", sid);
	e->endpoint = *remote;
	if (len > sizeof e->body) len = sizeof e->body;
	memcpy(e->body, datagram, len);
	e->body_len = len;
}
static void mock_udp_close(void *ctx, uint16_t sid) {
	push_event((struct mock *)ctx, "UDP_CLOSE", sid);
}
static void mock_protocol_error(void *ctx, const char *what) {
	struct mock *m = (struct mock *)ctx;
	m->protocol_errors++;
	m->last_error = what;
}

static struct mux_session *
make_session(struct mock *m)
{
	struct mux_callbacks cb = {
		.ctx = m,
		.write = mock_write,
		.on_hello = mock_hello,
		.on_bye = mock_bye,
		.on_ping = mock_ping,
		.on_pong = mock_pong,
		.on_tcp_open_ack = mock_tcp_open_ack,
		.on_tcp_data = mock_tcp_data,
		.on_tcp_close_wr = mock_tcp_close_wr,
		.on_tcp_rst = mock_tcp_rst,
		.on_tcp_window = mock_tcp_window,
		.on_udp_bind_ack = mock_udp_bind_ack,
		.on_udp_packet = mock_udp_packet,
		.on_udp_close = mock_udp_close,
		.on_protocol_error = mock_protocol_error,
	};
	return mux_create(&cb);
}

/* ====== send-side: bytes match PROTOCOL.md ====== */

static void
test_send_hello_matches_protocol(void)
{
	static const uint8_t want[12] = {
		0x00, 0x01, 0x00, 0x00, 0x00, 0x06,
		0x00, 0x00, 0x01, 0x00, 0x00, 0x40,
	};
	struct mock m = { 0 };
	struct mux_session *s = make_session(&m);
	struct hello_payload h = {
		.proto_ver = 0, .flags = 0,
		.max_streams = 256, .initial_win_kib = 64,
	};
	EXPECT(mux_send_hello(s, &h) == 0, "send_hello rc");
	EXPECT_BYTES(m.outbuf, m.outlen, want, sizeof want, "HELLO wire");
	mux_destroy(s);
	printf("PASS: mux_send_hello matches PROTOCOL.md §9.1\n");
}

static void
test_send_tcp_open_matches_protocol(void)
{
	static const uint8_t want[14] = {
		0x00, 0x10, 0x00, 0x01, 0x00, 0x08,
		0x01, 0x01, 0xBB, 0x04, 0x08, 0x08, 0x08, 0x08,
	};
	struct mock m = { 0 };
	struct mux_session *s = make_session(&m);
	struct endpoint ep = {
		.addr_type = ADDR_TYPE_IPV4, .port = 443, .addr_len = 4,
		.addr = { 8, 8, 8, 8 },
	};
	EXPECT(mux_send_tcp_open(s, 1, &ep) == 0, "send_tcp_open rc");
	EXPECT_BYTES(m.outbuf, m.outlen, want, sizeof want, "TCP_OPEN wire");
	mux_destroy(s);
	printf("PASS: mux_send_tcp_open matches PROTOCOL.md §9.2\n");
}

static void
test_send_tcp_data_matches_protocol(void)
{
	static const uint8_t want[11] = {
		0x00, 0x12, 0x00, 0x01, 0x00, 0x05,
		0x47, 0x45, 0x54, 0x20, 0x2F,
	};
	struct mock m = { 0 };
	struct mux_session *s = make_session(&m);
	EXPECT(mux_send_tcp_data(s, 1, (const uint8_t *)"GET /", 5) == 0, "rc");
	EXPECT_BYTES(m.outbuf, m.outlen, want, sizeof want, "TCP_DATA wire");
	mux_destroy(s);
	printf("PASS: mux_send_tcp_data matches PROTOCOL.md §9.3\n");
}

static void
test_send_udp_packet_composite(void)
{
	/* stream_id=1, remote=1.1.1.1:53, datagram="HI" */
	static const uint8_t want[16] = {
		0x00, 0x22, 0x00, 0x01, 0x00, 0x0A,
		0x01, 0x00, 0x35, 0x04,
		0x01, 0x01, 0x01, 0x01,
		'H', 'I',
	};
	struct mock m = { 0 };
	struct mux_session *s = make_session(&m);
	struct endpoint ep = {
		.addr_type = ADDR_TYPE_IPV4, .port = 53, .addr_len = 4,
		.addr = { 1, 1, 1, 1 },
	};
	EXPECT(mux_send_udp_packet(s, 1, &ep, (const uint8_t *)"HI", 2) == 0, "rc");
	EXPECT_BYTES(m.outbuf, m.outlen, want, sizeof want, "UDP_PACKET wire");
	mux_destroy(s);
	printf("PASS: mux_send_udp_packet wire layout\n");
}

static void
test_send_short_write_returns_error(void)
{
	struct mock m = { 0 };
	m.force_short_write = 1;
	struct mux_session *s = make_session(&m);
	struct hello_payload h = { .proto_ver = 0, .flags = 0, .max_streams = 0, .initial_win_kib = 0 };
	int rc = mux_send_hello(s, &h);
	EXPECT(rc == -1, "short write should produce -1");
	mux_destroy(s);
	printf("PASS: short write returns -1\n");
}

/* ====== receive-side: reassembly + dispatch ====== */

static void
test_feed_single_frame_dispatch(void)
{
	/* HELLO from peer */
	static const uint8_t frame[12] = {
		0x00, 0x01, 0x00, 0x00, 0x00, 0x06,
		0x00, 0x00, 0x01, 0x00, 0x00, 0x40,
	};
	struct mock m = { 0 };
	struct mux_session *s = make_session(&m);
	EXPECT(mux_feed(s, frame, sizeof frame) == 0, "feed rc");
	EXPECT(m.event_count == 1, "one event");
	EXPECT(strcmp(m.events[0].tag, "HELLO") == 0, "HELLO tag");
	EXPECT(m.events[0].hello.proto_ver == 0, "proto_ver");
	EXPECT(m.events[0].hello.max_streams == 256, "max_streams");
	EXPECT(m.events[0].hello.initial_win_kib == 64, "initial_win_kib");
	mux_destroy(s);
	printf("PASS: feed single frame dispatch\n");
}

static void
test_feed_byte_by_byte(void)
{
	/* Same HELLO, fed one byte at a time. */
	static const uint8_t frame[12] = {
		0x00, 0x01, 0x00, 0x00, 0x00, 0x06,
		0x00, 0x00, 0x01, 0x00, 0x00, 0x40,
	};
	struct mock m = { 0 };
	struct mux_session *s = make_session(&m);
	for (size_t i = 0; i < sizeof frame; i++)
		EXPECT(mux_feed(s, frame + i, 1) == 0, "feed 1B rc");
	EXPECT(m.event_count == 1, "one event after dribble");
	EXPECT(m.events[0].hello.max_streams == 256, "max_streams");
	mux_destroy(s);
	printf("PASS: feed byte-by-byte reassembly\n");
}

static void
test_feed_multiple_frames_one_chunk(void)
{
	/* HELLO followed by PING (nonce="ABCD") in a single feed call. */
	uint8_t buf[12 + 6 + 4];
	memcpy(buf, (uint8_t[]){0x00,0x01,0x00,0x00,0x00,0x06,
				0x00,0x00,0x01,0x00,0x00,0x40}, 12);
	memcpy(buf + 12, (uint8_t[]){0x00,0x03,0x00,0x00,0x00,0x04}, 6);
	memcpy(buf + 18, "ABCD", 4);

	struct mock m = { 0 };
	struct mux_session *s = make_session(&m);
	EXPECT(mux_feed(s, buf, sizeof buf) == 0, "feed rc");
	EXPECT(m.event_count == 2, "two events");
	EXPECT(strcmp(m.events[0].tag, "HELLO") == 0, "first HELLO");
	EXPECT(strcmp(m.events[1].tag, "PING") == 0, "second PING");
	EXPECT(m.events[1].body_len == 4 && memcmp(m.events[1].body, "ABCD", 4) == 0, "ping body");
	mux_destroy(s);
	printf("PASS: feed two frames in one chunk\n");
}

static void
test_feed_split_across_frames(void)
{
	/* Same two frames, split mid-payload of HELLO and mid-header of PING. */
	uint8_t buf[12 + 6 + 4];
	memcpy(buf, (uint8_t[]){0x00,0x01,0x00,0x00,0x00,0x06,
				0x00,0x00,0x01,0x00,0x00,0x40}, 12);
	memcpy(buf + 12, (uint8_t[]){0x00,0x03,0x00,0x00,0x00,0x04}, 6);
	memcpy(buf + 18, "WXYZ", 4);

	struct mock m = { 0 };
	struct mux_session *s = make_session(&m);
	/* split points: 4 (mid-header), 9 (mid-HELLO payload), 14 (mid-PING header), end */
	const size_t splits[] = { 4, 9, 14, sizeof buf };
	size_t prev = 0;
	for (size_t i = 0; i < sizeof splits / sizeof splits[0]; i++) {
		size_t n = splits[i] - prev;
		EXPECT(mux_feed(s, buf + prev, n) == 0, "feed chunk rc");
		prev = splits[i];
	}
	EXPECT(m.event_count == 2, "two events");
	EXPECT(m.events[1].body_len == 4 && memcmp(m.events[1].body, "WXYZ", 4) == 0, "ping body");
	mux_destroy(s);
	printf("PASS: feed split across frame boundaries\n");
}

static void
test_feed_tcp_data_dispatch(void)
{
	/* TCP_DATA stream_id=7, payload "abc". */
	static const uint8_t frame[9] = {
		0x00, 0x12, 0x00, 0x07, 0x00, 0x03, 'a', 'b', 'c',
	};
	struct mock m = { 0 };
	struct mux_session *s = make_session(&m);
	EXPECT(mux_feed(s, frame, sizeof frame) == 0, "feed rc");
	EXPECT(m.event_count == 1, "one event");
	EXPECT(strcmp(m.events[0].tag, "TCP_DATA") == 0, "tag");
	EXPECT(m.events[0].stream_id == 7, "stream_id");
	EXPECT(m.events[0].body_len == 3 && memcmp(m.events[0].body, "abc", 3) == 0, "body");
	mux_destroy(s);
	printf("PASS: TCP_DATA dispatch\n");
}

static void
test_feed_udp_packet_dispatch(void)
{
	/* UDP_PACKET stream_id=1, src=1.1.1.1:53, datagram="HI". */
	static const uint8_t frame[16] = {
		0x00, 0x22, 0x00, 0x01, 0x00, 0x0A,
		0x01, 0x00, 0x35, 0x04, 0x01, 0x01, 0x01, 0x01,
		'H', 'I',
	};
	struct mock m = { 0 };
	struct mux_session *s = make_session(&m);
	EXPECT(mux_feed(s, frame, sizeof frame) == 0, "feed rc");
	EXPECT(m.event_count == 1, "one event");
	EXPECT(strcmp(m.events[0].tag, "UDP_PACKET") == 0, "tag");
	EXPECT(m.events[0].stream_id == 1, "stream_id");
	EXPECT(m.events[0].endpoint.port == 53, "port");
	EXPECT(m.events[0].endpoint.addr[0] == 1 && m.events[0].endpoint.addr[3] == 1, "addr");
	EXPECT(m.events[0].body_len == 2 && memcmp(m.events[0].body, "HI", 2) == 0, "datagram");
	mux_destroy(s);
	printf("PASS: UDP_PACKET dispatch\n");
}

static void
test_feed_tcp_window_dispatch(void)
{
	static const uint8_t frame[10] = {
		0x00, 0x15, 0x00, 0x01, 0x00, 0x04,
		0x00, 0x00, 0x40, 0x00,  /* credit = 16384 */
	};
	struct mock m = { 0 };
	struct mux_session *s = make_session(&m);
	EXPECT(mux_feed(s, frame, sizeof frame) == 0, "feed rc");
	EXPECT(m.event_count == 1, "one event");
	EXPECT(strcmp(m.events[0].tag, "TCP_WINDOW") == 0, "tag");
	EXPECT(m.events[0].scalar == 16384, "credit");
	mux_destroy(s);
	printf("PASS: TCP_WINDOW dispatch\n");
}

/* ====== protocol error detection ====== */

static void
test_version_mismatch(void)
{
	/* HELLO but ver byte = 0x99 */
	static const uint8_t frame[12] = {
		0x99, 0x01, 0x00, 0x00, 0x00, 0x06,
		0x00, 0x00, 0x01, 0x00, 0x00, 0x40,
	};
	struct mock m = { 0 };
	struct mux_session *s = make_session(&m);
	EXPECT(mux_feed(s, frame, sizeof frame) == -1, "feed should fail");
	EXPECT(m.protocol_errors == 1, "one protocol error");
	EXPECT(m.last_error != NULL, "error string set");
	mux_destroy(s);
	printf("PASS: version mismatch flagged\n");
}

static void
test_unknown_frame_type(void)
{
	/* unknown type 0xAB, length=0 */
	static const uint8_t frame[6] = { 0x00, 0xAB, 0x00, 0x00, 0x00, 0x00 };
	struct mock m = { 0 };
	struct mux_session *s = make_session(&m);
	EXPECT(mux_feed(s, frame, sizeof frame) == -1, "feed should fail");
	EXPECT(m.protocol_errors == 1, "one protocol error");
	mux_destroy(s);
	printf("PASS: unknown type flagged\n");
}

static void
test_server_sends_tcp_open_rejected(void)
{
	/* Server (Android) MUST NOT send TCP_OPEN to client (pomera). */
	static const uint8_t frame[14] = {
		0x00, 0x10, 0x00, 0x01, 0x00, 0x08,
		0x01, 0x01, 0xBB, 0x04, 0x08, 0x08, 0x08, 0x08,
	};
	struct mock m = { 0 };
	struct mux_session *s = make_session(&m);
	EXPECT(mux_feed(s, frame, sizeof frame) == -1, "should fail");
	EXPECT(m.protocol_errors == 1, "one error");
	mux_destroy(s);
	printf("PASS: server-side TCP_OPEN rejected\n");
}

static void
test_malformed_hello(void)
{
	/* HELLO with payload length 5 (must be 6). */
	static const uint8_t frame[11] = {
		0x00, 0x01, 0x00, 0x00, 0x00, 0x05,
		0x00, 0x00, 0x01, 0x00, 0x00,
	};
	struct mock m = { 0 };
	struct mux_session *s = make_session(&m);
	EXPECT(mux_feed(s, frame, sizeof frame) == -1, "should fail");
	EXPECT(m.protocol_errors == 1, "one error");
	mux_destroy(s);
	printf("PASS: malformed HELLO rejected\n");
}

static void
test_tcp_data_empty_rejected(void)
{
	/* TCP_DATA with length=0 is illegal per PROTOCOL.md §3.6. */
	static const uint8_t frame[6] = { 0x00, 0x12, 0x00, 0x01, 0x00, 0x00 };
	struct mock m = { 0 };
	struct mux_session *s = make_session(&m);
	EXPECT(mux_feed(s, frame, sizeof frame) == -1, "should fail");
	EXPECT(m.protocol_errors == 1, "one error");
	mux_destroy(s);
	printf("PASS: empty TCP_DATA rejected\n");
}

int
main(void)
{
	g_failed = 0;

	test_send_hello_matches_protocol();
	test_send_tcp_open_matches_protocol();
	test_send_tcp_data_matches_protocol();
	test_send_udp_packet_composite();
	test_send_short_write_returns_error();

	test_feed_single_frame_dispatch();
	test_feed_byte_by_byte();
	test_feed_multiple_frames_one_chunk();
	test_feed_split_across_frames();
	test_feed_tcp_data_dispatch();
	test_feed_udp_packet_dispatch();
	test_feed_tcp_window_dispatch();

	test_version_mismatch();
	test_unknown_frame_type();
	test_server_sends_tcp_open_rejected();
	test_malformed_hello();
	test_tcp_data_empty_rejected();

	if (g_failed) {
		fprintf(stderr, "MUX TESTS FAILED\n");
		return 1;
	}
	printf("All mux tests passed.\n");
	return 0;
}
