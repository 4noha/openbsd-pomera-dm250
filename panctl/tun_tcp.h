/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 4noha */
/*
 * panctl/tun_tcp.h — minimal userland TCP-over-tun shim.
 *
 * Sits between OpenBSD's /dev/tunN device (raw IPv4 frames) and panctl's
 * mux protocol. For each TCP 5-tuple (src_ip, src_port, dst_ip, dst_port)
 * we keep a tiny state machine and a mux stream id. Outbound bytes from
 * the local TCP stack get forwarded via mux_send_tcp_data; inbound bytes
 * from the mux are wrapped in IP+TCP segments and re-injected on tun.
 *
 * Scope of this v0 implementation:
 *   * Single connection at a time (PoC; multi-connection comes next).
 *   * No retransmission timer — we rely on the local kernel to retransmit.
 *   * No TCP options beyond MSS (advertised in our SYN-ACK).
 *   * No window scaling, no SACK, no PAWS. Fixed window of 32 KiB.
 *   * IPv4 only.
 *
 * Caller integrates with the BTstack run loop: divert.c demuxes packets
 * off /dev/tunN by IP protocol — UDP goes to the existing handler, TCP
 * comes here via tun_tcp_handle_packet().
 */
#ifndef PANCTL_TUN_TCP_H
#define PANCTL_TUN_TCP_H

#include "mux.h"

#include <stddef.h>
#include <stdint.h>

struct tun_tcp; /* opaque */

/*
 * tun_fd is the file descriptor for /dev/tunN. mux is the mux session
 * tun_tcp will dispatch outbound TCP onto. tun_tcp does not take ownership
 * of either; caller is responsible for close/destroy in correct order.
 */
struct tun_tcp *tun_tcp_create(int tun_fd, struct mux_session *mux);
void            tun_tcp_destroy(struct tun_tcp *);

/*
 * Feed one raw IPv4 packet (no AF prefix). divert.c is expected to have
 * stripped the OpenBSD tun(4) 4-byte AF header before calling us. We
 * verify it's IPv4 + TCP ourselves and ignore otherwise.
 */
void tun_tcp_handle_packet(struct tun_tcp *, const uint8_t *ip, size_t len);

/*
 * Mux callbacks. Each returns 1 if the stream id matched a tun_tcp
 * connection (and was handled), 0 otherwise — main.c uses this to chain
 * to other handlers (test mode, divert fallback).
 */
int tun_tcp_on_tcp_open_ack(struct tun_tcp *, uint16_t sid, uint16_t status);
int tun_tcp_on_tcp_data    (struct tun_tcp *, uint16_t sid,
                            const uint8_t *data, size_t len);
int tun_tcp_on_tcp_close_wr(struct tun_tcp *, uint16_t sid);
int tun_tcp_on_tcp_rst     (struct tun_tcp *, uint16_t sid, uint16_t reason);

/*
 * Periodic sweep — caller invokes ~once per second. Resets any slot stuck
 * in TS_SYN_RCVD beyond a few seconds (mux open never acked: Android
 * couldn't reach the destination, or the OPEN frame was dropped under
 * queue overflow). Without this, Tailscale's DERP-discovery burst exhausts
 * the slot table and locks new connections out.
 */
void tun_tcp_sweep(struct tun_tcp *);

#endif /* PANCTL_TUN_TCP_H */
