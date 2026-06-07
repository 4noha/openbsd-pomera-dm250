/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 4noha */
/*
 * panctl/ctl.c — UNIX-domain control socket implementation.
 * See ctl.h for the contract.
 */
#include "ctl.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#define CTL_MAX_CLIENTS 4
#define CTL_LINE_MAX    128

struct ctl_client {
	int  fd;                    /* -1 if slot free */
	char buf[CTL_LINE_MAX];
	size_t filled;
};

struct panctl_ctl {
	int listen_fd;
	struct ctl_client clients[CTL_MAX_CLIENTS];
};

static int
open_listen(const char *path)
{
	(void)unlink(path);

	int s = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
	if (s < 0)
		return -1;

	struct sockaddr_un sun = { 0 };
	sun.sun_family = AF_UNIX;
	if (strlen(path) >= sizeof sun.sun_path) {
		close(s);
		errno = ENAMETOOLONG;
		return -1;
	}
	strcpy(sun.sun_path, path);

	if (bind(s, (struct sockaddr *)&sun, sizeof sun) < 0) {
		close(s);
		return -1;
	}
	/* root:wheel 0660 — only root and panctl operators (via doas) can
	 * speak to the socket. <your-pomera-user> runs panctlctl via doas. */
	(void)chmod(path, 0660);

	if (listen(s, 4) < 0) {
		close(s);
		(void)unlink(path);
		return -1;
	}
	return s;
}

struct panctl_ctl *
ctl_create(const char *sock_path)
{
	struct panctl_ctl *c = (struct panctl_ctl *)calloc(1, sizeof *c);
	if (c == NULL)
		return NULL;
	for (size_t i = 0; i < CTL_MAX_CLIENTS; i++)
		c->clients[i].fd = -1;
	c->listen_fd = open_listen(sock_path);
	if (c->listen_fd < 0) {
		free(c);
		return NULL;
	}
	return c;
}

void
ctl_destroy(struct panctl_ctl *c)
{
	if (c == NULL)
		return;
	for (size_t i = 0; i < CTL_MAX_CLIENTS; i++)
		if (c->clients[i].fd >= 0)
			close(c->clients[i].fd);
	if (c->listen_fd >= 0)
		close(c->listen_fd);
	free(c);
}

int
ctl_listen_fd(const struct panctl_ctl *c)
{
	return c ? c->listen_fd : -1;
}

static void read_client(struct panctl_ctl *c, struct ctl_client *cl);

static void
do_accept(struct panctl_ctl *c)
{
	for (;;) {
		int afd = accept4(c->listen_fd, NULL, NULL, SOCK_NONBLOCK);
		if (afd < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return;
			if (errno == EINTR)
				continue;
			return;
		}
		int slot = -1;
		for (size_t i = 0; i < CTL_MAX_CLIENTS; i++)
			if (c->clients[i].fd < 0) { slot = (int)i; break; }
		if (slot < 0) {
			(void)dprintf(afd, "busy\n");
			close(afd);
			continue;
		}
		c->clients[slot].fd = afd;
		c->clients[slot].filled = 0;
		/* Most panctlctl invocations write the whole command before
		 * the accept returns; try to read+dispatch eagerly so the
		 * reply lands within the same select cycle. */
		read_client(c, &c->clients[slot]);
	}
}

static void
drop_client(struct panctl_ctl *c, struct ctl_client *cl)
{
	close(cl->fd);
	cl->fd = -1;
	cl->filled = 0;
	(void)c;
}

static void
read_client(struct panctl_ctl *c, struct ctl_client *cl)
{
	for (;;) {
		ssize_t n = read(cl->fd, cl->buf + cl->filled,
		                 sizeof cl->buf - 1 - cl->filled);
		if (n == 0) {
			drop_client(c, cl);
			return;
		}
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return;
			if (errno == EINTR)
				continue;
			drop_client(c, cl);
			return;
		}
		cl->filled += (size_t)n;

		/* Look for a newline; dispatch one command per request. */
		char *nl = (char *)memchr(cl->buf, '\n', cl->filled);
		if (nl == NULL) {
			if (cl->filled >= sizeof cl->buf - 1) {
				(void)dprintf(cl->fd, "line too long\n");
				drop_client(c, cl);
				return;
			}
			continue;
		}
		*nl = '\0';
		/* Strip trailing \r if present. */
		size_t len = (size_t)(nl - cl->buf);
		if (len > 0 && cl->buf[len - 1] == '\r')
			cl->buf[--len] = '\0';

		ctl_on_command(cl->buf, cl->fd);
		/* One-shot: close after handling. Keeps state simple and the
		 * client-side panctlctl exits right after the reply. */
		drop_client(c, cl);
		return;
	}
}

void
ctl_handle_listen(struct panctl_ctl *c)
{
	if (c)
		do_accept(c);
}

void
ctl_drain(struct panctl_ctl *c)
{
	if (c == NULL)
		return;
	for (size_t i = 0; i < CTL_MAX_CLIENTS; i++)
		if (c->clients[i].fd >= 0)
			read_client(c, &c->clients[i]);
}
