/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 4noha */
/*
 * panctl/test_frame.c — unit tests for the mux wire format.
 *
 * Includes PROTOCOL.md §9 test vectors verbatim so that any encoding drift
 * vs. android_app's mux/Frame.kt is caught at build time.
 */
#include "frame.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failed;

static void
hex_print(FILE *f, const char *label, const uint8_t *buf, size_t len)
{
	fprintf(f, "%s", label);
	for (size_t i = 0; i < len; i++)
		fprintf(f, "%02X%s", buf[i], (i + 1 == len) ? "" : " ");
	fprintf(f, "\n");
}

#define EXPECT(cond, msg) do {                                          \
	if (!(cond)) {                                                  \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); \
		g_failed = 1;                                           \
		return;                                                 \
	}                                                               \
} while (0)

#define EXPECT_BYTES(got, want, len, label) do {                        \
	if (memcmp((got), (want), (len)) != 0) {                        \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, label); \
		hex_print(stderr, "  got:  ", (got), (len));            \
		hex_print(stderr, "  want: ", (want), (len));           \
		g_failed = 1;                                           \
		return;                                                 \
	}                                                               \
} while (0)

/* PROTOCOL.md §9.1 — HELLO (proto_ver=0, max_streams=256, initial_win=64). */
static void
test_hello_vector(void)
{
	static const uint8_t want[12] = {
		0x00, 0x01, 0x00, 0x00, 0x00, 0x06,
		0x00, 0x00, 0x01, 0x00, 0x00, 0x40,
	};
	uint8_t got[12];

	frame_hdr_encode(got, FRAME_TYPE_HELLO, 0, 6);

	struct hello_payload h = {
		.proto_ver = 0, .flags = 0,
		.max_streams = 256, .initial_win_kib = 64,
	};
	hello_encode(got + FRAME_HEADER_SIZE, &h);

	EXPECT_BYTES(got, want, sizeof want, "HELLO vector (PROTOCOL.md §9.1)");
	printf("PASS: HELLO vector (§9.1)\n");
}

/* PROTOCOL.md §9.2 — TCP_OPEN (stream_id=1, dst=8.8.8.8:443). */
static void
test_tcp_open_vector(void)
{
	static const uint8_t want[14] = {
		0x00, 0x10, 0x00, 0x01, 0x00, 0x08,
		0x01, 0x01, 0xBB, 0x04, 0x08, 0x08, 0x08, 0x08,
	};
	uint8_t got[14];

	frame_hdr_encode(got, FRAME_TYPE_TCP_OPEN, 1, 8);

	struct endpoint ep = {
		.addr_type = ADDR_TYPE_IPV4,
		.port = 443,
		.addr_len = 4,
		.addr = { 8, 8, 8, 8 },
	};
	size_t n = 0;
	int rc = endpoint_encode(got + FRAME_HEADER_SIZE,
				 sizeof got - FRAME_HEADER_SIZE, &ep, &n);
	EXPECT(rc == 0, "endpoint_encode rc != 0");
	EXPECT(n == 8, "endpoint encoded len != 8");

	EXPECT_BYTES(got, want, sizeof want, "TCP_OPEN vector (PROTOCOL.md §9.2)");
	printf("PASS: TCP_OPEN vector (§9.2)\n");
}

/* PROTOCOL.md §9.3 — TCP_DATA (stream_id=1, payload "GET /"). */
static void
test_tcp_data_vector(void)
{
	static const uint8_t want[11] = {
		0x00, 0x12, 0x00, 0x01, 0x00, 0x05,
		0x47, 0x45, 0x54, 0x20, 0x2F,
	};
	uint8_t got[11];

	frame_hdr_encode(got, FRAME_TYPE_TCP_DATA, 1, 5);
	memcpy(got + FRAME_HEADER_SIZE, "GET /", 5);

	EXPECT_BYTES(got, want, sizeof want, "TCP_DATA vector (PROTOCOL.md §9.3)");
	printf("PASS: TCP_DATA vector (§9.3)\n");
}

static void
test_scalar_helpers(void)
{
	uint8_t buf[4];

	write_u16_be(buf, 0xBEEF);
	EXPECT(buf[0] == 0xBE && buf[1] == 0xEF, "u16 be");
	EXPECT(read_u16_be(buf) == 0xBEEF, "u16 roundtrip");

	write_u32_be(buf, 0xDEADBEEFu);
	EXPECT(buf[0] == 0xDE && buf[1] == 0xAD && buf[2] == 0xBE && buf[3] == 0xEF, "u32 be");
	EXPECT(read_u32_be(buf) == 0xDEADBEEFu, "u32 roundtrip");

	printf("PASS: scalar helpers\n");
}

static void
test_header_roundtrip(void)
{
	uint8_t buf[FRAME_HEADER_SIZE];
	frame_hdr_encode(buf, FRAME_TYPE_TCP_DATA, 0x1234, 0xABCD);

	struct frame_hdr h;
	frame_hdr_decode(buf, &h);
	EXPECT(h.ver == PROTO_VERSION, "ver");
	EXPECT(h.type == FRAME_TYPE_TCP_DATA, "type");
	EXPECT(h.stream_id == 0x1234, "stream_id");
	EXPECT(h.length == 0xABCD, "length");

	printf("PASS: header roundtrip\n");
}

static void
test_hello_roundtrip(void)
{
	struct hello_payload in = {
		.proto_ver = 0, .flags = 0,
		.max_streams = 256, .initial_win_kib = 64,
	};
	uint8_t buf[6];
	hello_encode(buf, &in);

	struct hello_payload out;
	int rc = hello_decode(buf, sizeof buf, &out);
	EXPECT(rc == 0, "hello_decode rc");
	EXPECT(out.proto_ver == in.proto_ver, "proto_ver");
	EXPECT(out.flags == in.flags, "flags");
	EXPECT(out.max_streams == in.max_streams, "max_streams");
	EXPECT(out.initial_win_kib == in.initial_win_kib, "initial_win_kib");

	/* wrong length must fail */
	uint8_t short_buf[5] = { 0 };
	EXPECT(hello_decode(short_buf, sizeof short_buf, &out) == -1, "hello short");

	printf("PASS: HELLO roundtrip\n");
}

static void
test_endpoint_roundtrip_ipv4(void)
{
	struct endpoint in = {
		.addr_type = ADDR_TYPE_IPV4, .port = 443, .addr_len = 4,
		.addr = { 8, 8, 8, 8 },
	};
	uint8_t buf[16];
	size_t n = 0;
	EXPECT(endpoint_encode(buf, sizeof buf, &in, &n) == 0, "encode");
	EXPECT(n == 8, "encoded len");

	struct endpoint out;
	size_t consumed = 0;
	EXPECT(endpoint_decode(buf, n, 0, &out, &consumed) == 0, "decode");
	EXPECT(consumed == 8, "consumed");
	EXPECT(out.addr_type == in.addr_type, "addr_type");
	EXPECT(out.port == in.port, "port");
	EXPECT(out.addr_len == in.addr_len, "addr_len");
	EXPECT(memcmp(out.addr, in.addr, in.addr_len) == 0, "addr bytes");

	printf("PASS: endpoint roundtrip (IPv4)\n");
}

static void
test_endpoint_roundtrip_ipv6(void)
{
	struct endpoint in = {
		.addr_type = ADDR_TYPE_IPV6, .port = 53, .addr_len = 16,
		.addr = {
			0x20,0x01,0x4b,0x10,0x00,0x00,0x00,0x00,
			0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,
		},
	};
	uint8_t buf[32];
	size_t n = 0;
	EXPECT(endpoint_encode(buf, sizeof buf, &in, &n) == 0, "encode");
	EXPECT(n == 20, "encoded len");

	struct endpoint out;
	size_t consumed = 0;
	EXPECT(endpoint_decode(buf, n, 0, &out, &consumed) == 0, "decode");
	EXPECT(consumed == 20, "consumed");
	EXPECT(out.addr_type == in.addr_type, "addr_type");
	EXPECT(out.port == in.port, "port");
	EXPECT(out.addr_len == 16, "addr_len");
	EXPECT(memcmp(out.addr, in.addr, 16) == 0, "addr bytes");

	printf("PASS: endpoint roundtrip (IPv6)\n");
}

static void
test_endpoint_truncated_header(void)
{
	uint8_t buf[3] = { ADDR_TYPE_IPV4, 0x00, 0x00 };
	struct endpoint ep;
	size_t consumed = 0;
	EXPECT(endpoint_decode(buf, sizeof buf, 0, &ep, &consumed) == -1,
	       "truncated header should fail");
	printf("PASS: endpoint truncated header rejected\n");
}

static void
test_endpoint_truncated_addr(void)
{
	/* Declares addr_len=4 but only 1 addr byte is present. */
	uint8_t buf[5] = { ADDR_TYPE_IPV4, 0x01, 0xBB, 4, 0x08 };
	struct endpoint ep;
	size_t consumed = 0;
	EXPECT(endpoint_decode(buf, sizeof buf, 0, &ep, &consumed) == -1,
	       "truncated addr should fail");
	printf("PASS: endpoint truncated addr rejected\n");
}

static void
test_endpoint_overflow_addr_len(void)
{
	/* addr_len=255 declared but no bytes after; also exceeds struct capacity. */
	uint8_t buf[4] = { ADDR_TYPE_DOMAIN, 0x00, 0x35, 0xFF };
	struct endpoint ep;
	size_t consumed = 0;
	EXPECT(endpoint_decode(buf, sizeof buf, 0, &ep, &consumed) == -1,
	       "addr_len overflow should fail");
	printf("PASS: endpoint addr_len overflow rejected\n");
}

/*
 * Composite frame checks. PROTOCOL.md §9 only covers HELLO / TCP_OPEN /
 * TCP_DATA encode. These check the other frame types the Android side
 * actually sends/receives in MuxServer.kt / TcpStream.kt / UdpStream.kt so
 * that we don't silently drift from those implementations.
 */

/* TCP_WINDOW (PROTOCOL.md §3.9): u32 credit payload. */
static void
test_tcp_window_frame(void)
{
	/* stream_id=1, credit=16384 (= CREDIT_CHUNK in TcpStream.kt) */
	static const uint8_t want[10] = {
		0x00, 0x15, 0x00, 0x01, 0x00, 0x04,
		0x00, 0x00, 0x40, 0x00,
	};
	uint8_t got[10];

	frame_hdr_encode(got, FRAME_TYPE_TCP_WINDOW, 1, 4);
	write_u32_be(got + FRAME_HEADER_SIZE, 16384u);

	EXPECT_BYTES(got, want, sizeof want, "TCP_WINDOW frame");
	printf("PASS: TCP_WINDOW frame (credit u32)\n");
}

/* BYE (PROTOCOL.md §3.2): u16 reason payload, stream_id=0. */
static void
test_bye_frame(void)
{
	/* reason=0x0001 (REASON_VERSION_MISMATCH in MuxServer.kt) */
	static const uint8_t want[8] = {
		0x00, 0x02, 0x00, 0x00, 0x00, 0x02,
		0x00, 0x01,
	};
	uint8_t got[8];

	frame_hdr_encode(got, FRAME_TYPE_BYE, 0, 2);
	write_u16_be(got + FRAME_HEADER_SIZE, 0x0001);

	EXPECT_BYTES(got, want, sizeof want, "BYE frame");
	printf("PASS: BYE frame (reason u16)\n");
}

/* TCP_OPEN_ACK (PROTOCOL.md §3.5): u16 status payload. */
static void
test_tcp_open_ack_frame(void)
{
	/* stream_id=42, status=ACK_STATUS_ECONNREFUSED */
	static const uint8_t want[8] = {
		0x00, 0x11, 0x00, 0x2A, 0x00, 0x02,
		0x00, 0x01,
	};
	uint8_t got[8];

	frame_hdr_encode(got, FRAME_TYPE_TCP_OPEN_ACK, 42, 2);
	write_u16_be(got + FRAME_HEADER_SIZE, ACK_STATUS_ECONNREFUSED);

	EXPECT_BYTES(got, want, sizeof want, "TCP_OPEN_ACK frame");
	printf("PASS: TCP_OPEN_ACK frame (status u16)\n");
}

/*
 * UDP_PACKET (PROTOCOL.md §3.12): endpoint + datagram, no separator.
 * Mirrors UdpStream.kt:88-90 which builds the header in two halves
 * (encoded endpoint, then datagram) and sends them as a single frame.
 */
static void
test_udp_packet_frame(void)
{
	/* stream_id=1, remote=1.1.1.1:53, datagram="HI"
	 * Frame size = 6 (header) + 8 (endpoint) + 2 (datagram) = 16 bytes
	 * Payload length field = 8 + 2 = 10 = 0x0A
	 */
	static const uint8_t want[16] = {
		0x00, 0x22,                          /* ver, type */
		0x00, 0x01,                          /* stream_id */
		0x00, 0x0A,                          /* length = endpoint 8 + datagram 2 */
		0x01,                                /* addr_type = IPv4 */
		0x00, 0x35,                          /* port = 53 */
		0x04,                                /* addr_len */
		0x01, 0x01, 0x01, 0x01,              /* 1.1.1.1 */
		'H', 'I',                            /* datagram */
	};
	uint8_t got[16];
	struct endpoint ep = {
		.addr_type = ADDR_TYPE_IPV4, .port = 53, .addr_len = 4,
		.addr = { 1, 1, 1, 1 },
	};
	size_t ep_len = 0;
	int rc = endpoint_encode(got + FRAME_HEADER_SIZE,
				 sizeof got - FRAME_HEADER_SIZE, &ep, &ep_len);
	EXPECT(rc == 0, "endpoint_encode rc");
	EXPECT(ep_len == 8, "endpoint encoded len");

	memcpy(got + FRAME_HEADER_SIZE + ep_len, "HI", 2);
	frame_hdr_encode(got, FRAME_TYPE_UDP_PACKET, 1, (uint16_t)(ep_len + 2));

	EXPECT_BYTES(got, want, sizeof want, "UDP_PACKET frame");
	printf("PASS: UDP_PACKET frame (endpoint + datagram)\n");
}

/*
 * Inbound UDP_PACKET parsing: receive a frame, decode endpoint, slice off
 * the datagram. Mirrors MuxServer.kt:203-210.
 */
static void
test_udp_packet_inbound_parse(void)
{
	/* Same bytes as test_udp_packet_frame but treated as inbound. */
	static const uint8_t frame[16] = {
		0x00, 0x22, 0x00, 0x01, 0x00, 0x0A,
		0x01, 0x00, 0x35, 0x04, 0x01, 0x01, 0x01, 0x01,
		'H', 'I',
	};
	struct frame_hdr h;
	frame_hdr_decode(frame, &h);
	EXPECT(h.type == FRAME_TYPE_UDP_PACKET, "type");
	EXPECT(h.stream_id == 1, "stream_id");
	EXPECT(h.length == 10, "length");

	const uint8_t *payload = frame + FRAME_HEADER_SIZE;
	struct endpoint ep;
	size_t consumed = 0;
	int rc = endpoint_decode(payload, h.length, 0, &ep, &consumed);
	EXPECT(rc == 0, "endpoint_decode");
	EXPECT(consumed == 8, "consumed");
	EXPECT(ep.port == 53, "port");
	EXPECT(ep.addr[0] == 1 && ep.addr[3] == 1, "addr");

	/* Datagram is payload[consumed..length] */
	size_t dg_len = (size_t)h.length - consumed;
	EXPECT(dg_len == 2, "dg_len");
	EXPECT(payload[consumed] == 'H' && payload[consumed + 1] == 'I', "datagram");

	printf("PASS: inbound UDP_PACKET parse\n");
}

static void
test_endpoint_offset_decode(void)
{
	/* Endpoint embedded inside a UDP_PACKET payload, after some prefix. */
	uint8_t buf[16] = {
		0xAA, 0xBB,                          /* prefix scratch */
		ADDR_TYPE_IPV4, 0x00, 0x35, 0x04,    /* type, port=53, addr_len=4 */
		0x01, 0x01, 0x01, 0x01,              /* 1.1.1.1 */
		0xCC, 0xDD,                          /* trailing scratch */
	};
	struct endpoint ep;
	size_t consumed = 0;
	EXPECT(endpoint_decode(buf, sizeof buf, 2, &ep, &consumed) == 0, "decode");
	EXPECT(consumed == 8, "consumed");
	EXPECT(ep.port == 53, "port");
	EXPECT(ep.addr[0] == 1 && ep.addr[3] == 1, "addr");
	printf("PASS: endpoint offset decode\n");
}

int
main(void)
{
	g_failed = 0;

	test_scalar_helpers();
	test_header_roundtrip();
	test_hello_vector();
	test_hello_roundtrip();
	test_tcp_open_vector();
	test_tcp_data_vector();
	test_endpoint_roundtrip_ipv4();
	test_endpoint_roundtrip_ipv6();
	test_endpoint_truncated_header();
	test_endpoint_truncated_addr();
	test_endpoint_overflow_addr_len();
	test_endpoint_offset_decode();
	test_tcp_window_frame();
	test_bye_frame();
	test_tcp_open_ack_frame();
	test_udp_packet_frame();
	test_udp_packet_inbound_parse();

	if (g_failed) {
		fprintf(stderr, "TESTS FAILED\n");
		return 1;
	}
	printf("All tests passed.\n");
	return 0;
}
