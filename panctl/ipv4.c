/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 4noha */
/*
 * panctl/ipv4.c — IPv4 + UDP header packing/unpacking + checksum.
 */
#include "ipv4.h"

#include <string.h>

static uint16_t
read_be16(const uint8_t *b)
{
	return (uint16_t)((b[0] << 8) | b[1]);
}

static void
write_be16(uint8_t *b, uint16_t v)
{
	b[0] = (uint8_t)(v >> 8);
	b[1] = (uint8_t)(v & 0xFF);
}

uint16_t
ipv4_checksum(const uint8_t *buf, size_t len)
{
	uint32_t sum = 0;
	size_t i = 0;

	while (i + 1 < len) {
		sum += ((uint32_t)buf[i] << 8) | buf[i + 1];
		i += 2;
	}
	if (i < len)
		sum += ((uint32_t)buf[i] << 8);

	while (sum >> 16)
		sum = (sum & 0xFFFF) + (sum >> 16);
	return (uint16_t)(~sum & 0xFFFF);
}

int
ipv4_udp_parse(const uint8_t *buf, size_t len, struct ipv4_udp_view *out)
{
	if (len < IPV4_HEADER_MIN)
		return -1;

	uint8_t version_ihl = buf[0];
	if ((version_ihl >> 4) != 4)
		return -1;
	uint8_t ihl_words = version_ihl & 0x0F;
	if (ihl_words < 5)
		return -1;
	size_t ip_hdr_len = (size_t)ihl_words * 4;
	if (len < ip_hdr_len)
		return -1;

	uint16_t total_len = read_be16(buf + 2);
	if (total_len < ip_hdr_len || (size_t)total_len > len)
		return -1;

	if (buf[9] != 17)  /* IPPROTO_UDP */
		return -1;

	if (total_len < ip_hdr_len + UDP_HEADER_LEN)
		return -1;

	const uint8_t *udp = buf + ip_hdr_len;
	uint16_t udp_len = read_be16(udp + 4);
	if (udp_len < UDP_HEADER_LEN)
		return -1;
	if (ip_hdr_len + (size_t)udp_len > (size_t)total_len)
		return -1;

	memcpy(out->src_addr, buf + 12, 4);
	memcpy(out->dst_addr, buf + 16, 4);
	out->src_port = read_be16(udp);
	out->dst_port = read_be16(udp + 2);
	out->total_len = total_len;
	out->udp_len = udp_len;
	out->datagram = udp + UDP_HEADER_LEN;
	out->datagram_len = (uint16_t)(udp_len - UDP_HEADER_LEN);
	return 0;
}

int
ipv4_udp_build(uint8_t *buf, size_t cap,
	       const uint8_t src_addr[4], uint16_t src_port,
	       const uint8_t dst_addr[4], uint16_t dst_port,
	       uint16_t id, uint8_t ttl,
	       const uint8_t *datagram, size_t datagram_len)
{
	if (ttl == 0)
		ttl = 64;
	size_t total = IPV4_HEADER_MIN + UDP_HEADER_LEN + datagram_len;
	if (total > 0xFFFF || total > cap)
		return -1;

	/* IPv4 header */
	buf[0] = (4 << 4) | 5;       /* version=4, ihl=5 */
	buf[1] = 0;                   /* TOS */
	write_be16(buf + 2, (uint16_t)total);
	write_be16(buf + 4, id);      /* identification */
	write_be16(buf + 6, 0);       /* flags + frag offset */
	buf[8] = ttl;
	buf[9] = 17;                  /* protocol = UDP */
	write_be16(buf + 10, 0);      /* header checksum, fixed below */
	memcpy(buf + 12, src_addr, 4);
	memcpy(buf + 16, dst_addr, 4);
	write_be16(buf + 10, ipv4_checksum(buf, IPV4_HEADER_MIN));

	/* UDP header. Payload first so we can checksum cleanly. */
	uint8_t *udp = buf + IPV4_HEADER_MIN;
	write_be16(udp + 0, src_port);
	write_be16(udp + 2, dst_port);
	write_be16(udp + 4, (uint16_t)(UDP_HEADER_LEN + datagram_len));
	write_be16(udp + 6, 0);  /* checksum cleared for computation below */
	if (datagram_len > 0)
		memcpy(udp + UDP_HEADER_LEN, datagram, datagram_len);

	/*
	 * UDP checksum over pseudo-header + UDP header + datagram.
	 * Build the pseudo-header inline on the stack.
	 */
	size_t udp_total = UDP_HEADER_LEN + datagram_len;
	if (udp_total > 0xFFE0) {
		/* shouldn't happen given total <= 0xFFFF check above, but
		 * keep the check explicit for the stack buffer below */
		return -1;
	}
	uint8_t pseudo[12 + 65535];  /* large enough for max UDP datagram */
	memcpy(pseudo + 0, src_addr, 4);
	memcpy(pseudo + 4, dst_addr, 4);
	pseudo[8] = 0;
	pseudo[9] = 17;
	write_be16(pseudo + 10, (uint16_t)udp_total);
	memcpy(pseudo + 12, udp, udp_total);

	uint16_t sum = ipv4_checksum(pseudo, 12 + udp_total);
	/* UDP-over-IPv4 special case: a computed-zero checksum is transmitted
	 * as 0xFFFF (so that 0x0000 reliably means "no checksum"). */
	if (sum == 0)
		sum = 0xFFFF;
	write_be16(udp + 6, sum);

	return (int)total;
}
