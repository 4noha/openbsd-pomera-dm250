/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 4noha */
/*
 * panctl/test_ipv4.c — IPv4 + UDP packing/unpacking tests.
 */
#include "ipv4.h"

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

/*
 * RFC 1071 worked example: checksum of {0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5,
 * 0xf6, 0xf7} is 0x220d. (one's complement sum of 0x0001+0xf203+0xf4f5+0xf6f7
 * = 0x2ddf0; folded = 0x2df1; complement = 0xd20e -- wait, the standard
 * example yields 0x220d; we just verify our impl is internally consistent
 * via known IPv4 header below.)
 */
static void
test_checksum_known_header(void)
{
	/* Known IPv4 header (Wikipedia / Wireshark examples).
	 * 4500 003c 1c46 4000 4006 b1e6 ac10 0a63 ac10 0a0c
	 * checksum field is at offset 10..11 = 0xb1e6.
	 * If we zero it out and compute, we should get back 0xb1e6.
	 */
	uint8_t hdr[20] = {
		0x45, 0x00, 0x00, 0x3c,
		0x1c, 0x46, 0x40, 0x00,
		0x40, 0x06, 0x00, 0x00, /* checksum cleared */
		0xac, 0x10, 0x0a, 0x63,
		0xac, 0x10, 0x0a, 0x0c,
	};
	uint16_t sum = ipv4_checksum(hdr, sizeof hdr);
	EXPECT(sum == 0xb1e6, "ipv4 checksum");
	printf("PASS: ipv4_checksum known header\n");
}

static void
test_build_parse_roundtrip(void)
{
	uint8_t pkt[100];
	uint8_t src[4] = { 10, 0, 0, 1 };
	uint8_t dst[4] = { 1, 1, 1, 1 };
	const uint8_t payload[] = "Hello UDP world";
	size_t payload_len = sizeof payload - 1;

	int n = ipv4_udp_build(pkt, sizeof pkt, src, 54321, dst, 53,
			       0x1234, 0, payload, payload_len);
	EXPECT(n > 0, "build returns positive");
	EXPECT((size_t)n == 20 + 8 + payload_len, "total length");

	struct ipv4_udp_view v;
	int rc = ipv4_udp_parse(pkt, (size_t)n, &v);
	EXPECT(rc == 0, "parse succeeds");
	EXPECT(memcmp(v.src_addr, src, 4) == 0, "src_addr");
	EXPECT(memcmp(v.dst_addr, dst, 4) == 0, "dst_addr");
	EXPECT(v.src_port == 54321, "src_port");
	EXPECT(v.dst_port == 53, "dst_port");
	EXPECT(v.datagram_len == payload_len, "datagram_len");
	EXPECT(memcmp(v.datagram, payload, payload_len) == 0, "datagram");

	printf("PASS: ipv4+udp build/parse roundtrip\n");
}

static void
test_checksums_self_validate(void)
{
	/*
	 * For a correctly-built packet, recomputing the IP header checksum
	 * over the header (including its checksum field) should yield 0.
	 */
	uint8_t pkt[100];
	uint8_t src[4] = { 192, 168, 1, 100 };
	uint8_t dst[4] = { 8, 8, 8, 8 };
	const uint8_t payload[] = { 0x01, 0x02, 0x03, 0x04 };

	int n = ipv4_udp_build(pkt, sizeof pkt, src, 12345, dst, 53,
			       0xABCD, 64, payload, sizeof payload);
	EXPECT(n > 0, "build");

	/* IP checksum self-validation. */
	uint16_t ip_sum = ipv4_checksum(pkt, 20);
	EXPECT(ip_sum == 0, "IP checksum should self-validate to 0");

	/* UDP checksum self-validation via pseudo-header. */
	size_t udp_total = (size_t)8 + sizeof payload;
	uint8_t pseudo[12 + 8 + sizeof payload];
	memcpy(pseudo, src, 4);
	memcpy(pseudo + 4, dst, 4);
	pseudo[8] = 0;
	pseudo[9] = 17;
	pseudo[10] = (uint8_t)(udp_total >> 8);
	pseudo[11] = (uint8_t)(udp_total & 0xFF);
	memcpy(pseudo + 12, pkt + 20, udp_total);
	uint16_t udp_sum = ipv4_checksum(pseudo, sizeof pseudo);
	EXPECT(udp_sum == 0, "UDP checksum should self-validate to 0");

	printf("PASS: build self-checksums validate to 0\n");
}

static void
test_parse_rejects_non_udp(void)
{
	/* Valid IPv4 header but protocol = TCP (6), not UDP. */
	uint8_t pkt[20] = {
		0x45, 0x00, 0x00, 0x14,
		0x00, 0x00, 0x40, 0x00,
		0x40, 0x06, 0x00, 0x00,
		0x0a, 0x00, 0x00, 0x01,
		0x0a, 0x00, 0x00, 0x02,
	};
	struct ipv4_udp_view v;
	EXPECT(ipv4_udp_parse(pkt, sizeof pkt, &v) == -1, "TCP must be rejected");
	printf("PASS: parse rejects non-UDP protocol\n");
}

static void
test_parse_rejects_short_buffer(void)
{
	uint8_t pkt[10] = { 0x45 };
	struct ipv4_udp_view v;
	EXPECT(ipv4_udp_parse(pkt, sizeof pkt, &v) == -1, "short buffer rejected");
	printf("PASS: parse rejects short buffer\n");
}

static void
test_parse_rejects_wrong_version(void)
{
	uint8_t pkt[20] = { 0x65 /* version=6 */ };
	struct ipv4_udp_view v;
	EXPECT(ipv4_udp_parse(pkt, sizeof pkt, &v) == -1, "non-IPv4 rejected");
	printf("PASS: parse rejects non-IPv4 version\n");
}

static void
test_parse_handles_ihl_greater_than_5(void)
{
	/* IHL = 6 means 24-byte IP header (4 bytes of options). */
	uint8_t pkt[40] = {
		0x46,                          /* version=4, ihl=6 */
		0x00,
		0x00, 0x20,                    /* total_len=32 */
		0x00, 0x00, 0x40, 0x00,
		0x40, 0x11, 0x00, 0x00,        /* proto=UDP */
		0xc0, 0xa8, 0x01, 0x01,        /* src */
		0xc0, 0xa8, 0x01, 0x02,        /* dst */
		0xaa, 0xaa, 0xaa, 0xaa,        /* 4 bytes IP options */
		/* UDP header at offset 24 */
		0x00, 0x35, 0x00, 0x35,
		0x00, 0x08, 0x00, 0x00,
		/* no payload */
	};
	struct ipv4_udp_view v;
	int rc = ipv4_udp_parse(pkt, sizeof pkt, &v);
	EXPECT(rc == 0, "parse with IHL=6");
	EXPECT(v.src_port == 53 && v.dst_port == 53, "ports");
	EXPECT(v.datagram_len == 0, "empty payload");
	printf("PASS: parse handles ihl > 5 (options)\n");
}

int
main(void)
{
	g_failed = 0;

	test_checksum_known_header();
	test_build_parse_roundtrip();
	test_checksums_self_validate();
	test_parse_rejects_non_udp();
	test_parse_rejects_short_buffer();
	test_parse_rejects_wrong_version();
	test_parse_handles_ihl_greater_than_5();

	if (g_failed) {
		fprintf(stderr, "IPv4 TESTS FAILED\n");
		return 1;
	}
	printf("All ipv4 tests passed.\n");
	return 0;
}
