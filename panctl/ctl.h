/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 4noha */
/*
 * panctl/ctl.h — UNIX-domain control socket.
 *
 * panctl listens on /var/run/panctl/ctl.sock so an operator can drive
 * out-of-band actions (currently: confirm/deny BT pair requests).
 *
 * Integration:
 *   ctl_create() returns an opaque handle.
 *   ctl_get_fds(c, fds, n_out) returns the listen fd plus any accepted
 *     client fds — main.c registers these with BTstack's run loop as
 *     data sources.
 *   ctl_handle_fd(c, fd) is called when one of those fds is readable.
 *
 * Commands (one per line, terminated by '\n'):
 *   confirm        accept the pending pair request
 *   deny           reject the pending pair request
 *   status         print advertise + pending pair state (or "idle")
 *   advertise on   become discoverable (scannable) until "advertise off"
 *   advertise off  stop being discoverable (connectable stays on)
 *   advertise      print current discoverable state
 *
 * The command is dispatched to ctl_on_command() (implemented in main.c)
 * with a reply fd the caller can dprintf() back to.
 */
#ifndef PANCTL_CTL_H
#define PANCTL_CTL_H

#include <stddef.h>

struct panctl_ctl;

struct panctl_ctl *ctl_create(const char *sock_path);
void               ctl_destroy(struct panctl_ctl *);

/* Return the listen fd. Caller registers it with its run loop and calls
 * ctl_handle_listen() when the fd becomes readable. */
int ctl_listen_fd(const struct panctl_ctl *);

/* Called when the listen fd is readable — accept(s) all pending clients,
 * eagerly reads any commands already buffered, and dispatches them. */
void ctl_handle_listen(struct panctl_ctl *);

/* Called periodically (e.g. from a 50ms sweep timer) to read any newly
 * arrived bytes on accepted client fds. Non-blocking. */
void ctl_drain(struct panctl_ctl *);

/* Implemented by main.c — invoked when a single newline-terminated
 * command has been read off an accepted client fd. */
extern void ctl_on_command(const char *cmd, int reply_fd);

#endif /* PANCTL_CTL_H */
