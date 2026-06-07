/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 4noha */
/*
 * panctl/ipv4.h — IPv4 + UDP header packing/unpacking + checksum.
 *
 * Used by divert.c on OpenBSD (for tun(4) re-injection of UDP reply paths,
 * and for parsing divert-packet payloads). Kept platform-independent so it
 * builds and tests on macOS as well.
 *
 * No malloc, no syscalls. The caller provides all buffers.
 */
#ifndef PANCTL_IPV4_H
#define PANCTL_IPV4_H

#include <stddef.h>
#include <stdint.h>

#define IPV4_HEADER_MIN 20
#define UDP_HEADER_LEN  8

/* Minimal parsed view of an IPv4 + UDP packet. Returned by ipv4_udp_parse. */
struct ipv4_udp_view {
	uint8_t  src_addr[4];
	uint8_t  dst_addr[4];
	uint16_t src_port;
	uint16_t dst_port;
	uint16_t total_len;       /* IPv4 total_length */
	uint16_t udp_len;         /* UDP header + datagram */
	const uint8_t *datagram;  /* points into the caller's buffer */
	uint16_t datagram_len;
};

/*
 * Parse an IPv4 + UDP packet sitting at buf[0..len). Performs minimal
 * validation (version, IHL, protocol, length consistency). Does NOT
 * verify checksums (the caller may chose to do so).
 *
 * Returns 0 on success; -1 if the packet isn't a parseable IPv4+UDP frame.
 */
int ipv4_udp_parse(const uint8_t *buf, size_t len, struct ipv4_udp_view *out);

/*
 * Build an IPv4 + UDP packet from scratch. Returns total bytes written into
 * buf, or -1 if cap is too small. Fills the IP and UDP checksums.
 *
 * IP identification is taken from id; caller picks any reasonable value
 * (monotonic counter or random). TTL defaults to 64 if 0 is passed.
 */
int ipv4_udp_build(uint8_t *buf, size_t cap,
		   const uint8_t src_addr[4], uint16_t src_port,
		   const uint8_t dst_addr[4], uint16_t dst_port,
		   uint16_t id, uint8_t ttl,
		   const uint8_t *datagram, size_t datagram_len);

/*
 * Internet checksum (RFC 1071). Exposed because UDP pseudo-header checksum
 * computation reuses it and tests verify it independently.
 */
uint16_t ipv4_checksum(const uint8_t *buf, size_t len);

#endif /* PANCTL_IPV4_H */
