/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 4noha */
/*
 * panctl/frame.c — mux protocol wire format implementation.
 */
#include "frame.h"

#include <string.h>

uint16_t
read_u16_be(const uint8_t *buf)
{
	return (uint16_t)((buf[0] << 8) | buf[1]);
}

uint32_t
read_u32_be(const uint8_t *buf)
{
	return ((uint32_t)buf[0] << 24)
	     | ((uint32_t)buf[1] << 16)
	     | ((uint32_t)buf[2] << 8)
	     |  (uint32_t)buf[3];
}

void
write_u16_be(uint8_t *buf, uint16_t v)
{
	buf[0] = (uint8_t)(v >> 8);
	buf[1] = (uint8_t)(v & 0xFF);
}

void
write_u32_be(uint8_t *buf, uint32_t v)
{
	buf[0] = (uint8_t)(v >> 24);
	buf[1] = (uint8_t)(v >> 16);
	buf[2] = (uint8_t)(v >> 8);
	buf[3] = (uint8_t)(v & 0xFF);
}

int
frame_hdr_encode(uint8_t *buf, uint8_t type, uint16_t stream_id, uint16_t length)
{
	buf[0] = PROTO_VERSION;
	buf[1] = type;
	write_u16_be(buf + 2, stream_id);
	write_u16_be(buf + 4, length);
	return 0;
}

void
frame_hdr_decode(const uint8_t *buf, struct frame_hdr *out)
{
	out->ver = buf[0];
	out->type = buf[1];
	out->stream_id = read_u16_be(buf + 2);
	out->length = read_u16_be(buf + 4);
}

void
hello_encode(uint8_t *buf, const struct hello_payload *h)
{
	buf[0] = h->proto_ver;
	buf[1] = h->flags;
	write_u16_be(buf + 2, h->max_streams);
	write_u16_be(buf + 4, h->initial_win_kib);
}

int
hello_decode(const uint8_t *buf, size_t len, struct hello_payload *out)
{
	if (len != 6)
		return -1;
	out->proto_ver = buf[0];
	out->flags = buf[1];
	out->max_streams = read_u16_be(buf + 2);
	out->initial_win_kib = read_u16_be(buf + 4);
	return 0;
}

int
endpoint_encode(uint8_t *buf, size_t cap, const struct endpoint *ep,
		size_t *out_len)
{
	size_t need;

	if (ep->addr_len > sizeof ep->addr)
		return -1;
	need = (size_t)1 + 2 + 1 + ep->addr_len;
	if (cap < need)
		return -1;

	buf[0] = ep->addr_type;
	write_u16_be(buf + 1, ep->port);
	buf[3] = ep->addr_len;
	if (ep->addr_len > 0)
		memcpy(buf + 4, ep->addr, ep->addr_len);
	if (out_len != NULL)
		*out_len = need;
	return 0;
}

int
endpoint_decode(const uint8_t *buf, size_t len, size_t offset,
		struct endpoint *out, size_t *consumed)
{
	if (offset + 4 > len)
		return -1;

	out->addr_type = buf[offset];
	out->port = read_u16_be(buf + offset + 1);
	out->addr_len = buf[offset + 3];

	if (out->addr_len > sizeof out->addr)
		return -1;
	if (offset + 4 + (size_t)out->addr_len > len)
		return -1;

	if (out->addr_len > 0)
		memcpy(out->addr, buf + offset + 4, out->addr_len);
	if (consumed != NULL)
		*consumed = (size_t)4 + out->addr_len;
	return 0;
}
