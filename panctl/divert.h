/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 4noha */
/*
 * panctl/divert.h — pomera 側 outbound intercept (OpenBSD only at runtime).
 *
 * Two modes for UDP:
 *   DIVERT_UDP_MODE_PF   — pf divert-packet (SOCK_RAW IPPROTO_DIVERT).
 *                         Reply path re-injects via write() to the same socket.
 *   DIVERT_UDP_MODE_TUN  — read/write /dev/tunN (L3). Default route points at it.
 *
 * TCP always uses pf divert-to (a stream listen socket). The tun fallback for
 * TCP would require a userland TCP state machine, which is the 1-H escape
 * hatch — out of scope for this layer (see btstack/04-outbound-intercept.md).
 *
 * The layer is passive with respect to the event loop: it returns its
 * pollable fds and processes a single fd at a time. Caller (panctl/main.c)
 * owns the poll/select loop and the BTstack run loop.
 *
 * Header is platform-independent so panctl/main.c can include it on either
 * macOS (planning) or OpenBSD (real build). The implementation in divert.c
 * is OpenBSD-gated.
 */
#ifndef PANCTL_DIVERT_H
#define PANCTL_DIVERT_H

#include "mux.h"

#include <stddef.h>
#include <stdint.h>

enum divert_udp_mode {
	DIVERT_UDP_MODE_PF,
	DIVERT_UDP_MODE_TUN,
};

struct divert_config {
	uint16_t  pf_tcp_port;        /* divert-to port for TCP (e.g. 9999) */
	enum divert_udp_mode udp_mode;
	uint16_t  pf_udp_port;        /* divert-packet port (PF mode) */
	const char *tun_dev;          /* "/dev/tun0" (TUN mode) */

	/* Owned by caller. divert installs its callbacks into this session. */
	struct mux_session *mux;
};

struct divert; /* opaque */

/*
 * Allocate + open underlying sockets / tun device per cfg. Installs
 * stream-related callbacks (on_tcp_*, on_udp_*) onto cfg->mux so inbound
 * frames are routed back through divert. Control frames (HELLO/BYE/PING/
 * PONG/protocol_error) are NOT touched — the caller should set those
 * before passing the mux in.
 *
 * Returns NULL on failure. On OpenBSD the kernel may complain about
 * privileges (raw socket / divert socket needs root or the operator group).
 *
 * On non-OpenBSD platforms returns NULL with errno=ENOSYS.
 */
struct divert *divert_create(const struct divert_config *cfg);
void divert_destroy(struct divert *d);

/*
 * Fill out[0..*n) with the divert layer's pollable fds. *n is in/out:
 * on entry it's the capacity of out, on success it's set to the number
 * of fds actually written. Returns 0 on success, -1 if out was too small.
 *
 * The set of fds is fixed for the lifetime of the divert object, except
 * that accepted TCP connections add new fds dynamically. Callers must
 * re-poll after divert_handle_fd to pick up freshly-accepted TCP sockets:
 * use divert_get_fds() again, or query via divert_each_fd() (TBD).
 *
 * For v0 the implementation may only expose the listen/divert/tun fds and
 * spawn an internal thread / reader loop for accepted TCP fds. Simpler.
 */
int divert_get_fds(struct divert *d, int *out, size_t cap, size_t *n);

/*
 * Process readiness on one fd. Caller has just observed POLLIN on this fd.
 * May invoke mux_send_* multiple times. Returns 0 on success, -1 on fatal
 * error (e.g. divert socket closed).
 */
int divert_handle_fd(struct divert *d, int fd);

/*
 * Return the /dev/tunN file descriptor when DIVERT_UDP_MODE_TUN is in use,
 * so a separate userland TCP shim (panctl/tun_tcp.c) can write packets
 * back into it. Returns -1 in PF mode.
 */
int divert_get_tun_fd(const struct divert *d);

/*
 * Hook for forwarding non-UDP IPv4 frames pulled off tun. divert.c reads
 * the tun fd and demuxes by IP protocol; if cb is set and the packet is
 * TCP (proto 6), the callback is invoked with the stripped raw IPv4 frame.
 * Set to NULL to silently drop non-UDP (default).
 */
typedef void (*divert_tun_other_cb)(void *ctx, const uint8_t *ip, size_t len);
void divert_set_tun_other_cb(struct divert *d, divert_tun_other_cb cb, void *ctx);

#endif /* PANCTL_DIVERT_H */
