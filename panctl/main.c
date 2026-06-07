/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 4noha */
/*
 * panctl/main.c — BTstack-based RFCOMM client + mux + divert glue.
 *
 * Run loop architecture:
 *
 *   [BTstack POSIX run loop (select-based)]
 *     ├─ HCI transport (libusb fd via libusb_get_pollfds, or UART tty fd)
 *     ├─ Divert TCP listen fd (registered as data_source)
 *     ├─ Divert UDP fd (PF divert-packet or /dev/tunN, registered as data_source)
 *     └─ One-shot timer that periodically calls divert_drain_active_tcp()
 *        to sweep accepted TCP fds for new outbound bytes.
 *
 * Inbound:
 *   RFCOMM_DATA_PACKET → mux_feed → mux dispatch → divert trampolines →
 *   write to corresponding socket / synthesize UDP reply.
 *
 * Outbound:
 *   divert fd ready → divert_handle_fd → mux_send_* → mux_write_rfcomm →
 *   rfcomm_send.
 *
 * BUILD STATUS:
 *   Requires third_party/btstack/ (run tools/fetch-btstack.sh first).
 *   The Makefile's `panctl-libusb` target picks up BTstack sources via
 *   port/libusb/Makefile.inc; `panctl-h4` picks up port/posix-h4 +
 *   chipset/bcm for the DM250 path. Not built on macOS by default.
 *
 * STATUS: skeleton. The BTstack API calls below are written against the
 *   v1.6.x API and may need touch-up after first compile. Annotated
 *   carefully where BTstack version drift would bite.
 */

#include "frame.h"
#include "mux.h"
#include "divert.h"
#include "ctl.h"
#include "tun_tcp.h"

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>   /* close, write, dup */

/*
 * BTstack headers. Conditionally included so this file at least
 * passes a syntax check on macOS without third_party/btstack/.
 */
#if defined(HAVE_BTSTACK)
#  include "btstack.h"
#  include "btstack_run_loop_posix.h"
#  include "btstack_tlv_posix.h"
#  include "hci_transport_h4.h"
#  include "classic/rfcomm.h"
#  include "classic/btstack_link_key_db_tlv.h"
#  include "ble/le_device_db_tlv.h"
#  if defined(USE_BCM_CHIPSET)
#    include "btstack_chipset_bcm.h"
#  endif
#endif

#if defined(HAVE_BTSTACK)
static void on_btstack_packet(uint8_t, uint16_t, uint8_t *, uint16_t);
#endif

/* ============================ globals ============================ */

static struct mux_session *g_mux;
static struct divert *g_divert;
static struct panctl_ctl *g_ctl;
static struct tun_tcp *g_tun_tcp;

#if defined(HAVE_BTSTACK)
static bd_addr_t g_android_addr;
static uint16_t g_rfcomm_cid;          /* set by RFCOMM_EVENT_CHANNEL_OPENED */
static uint8_t g_rfcomm_server_channel;
static btstack_packet_callback_registration_t g_hci_event_cb;
static btstack_timer_source_t g_tcp_sweep_timer;
static btstack_timer_source_t g_reconnect_timer;

#define RECONNECT_DELAY_MS 3000

/*
 * Pending Numeric Comparison pair request. Set by HCI_EVENT_USER_-
 * CONFIRMATION_REQUEST handler, cleared by ctl_on_command("confirm"|"deny")
 * or by the pair timeout.
 */
static int g_pending_pair_set;
static bd_addr_t g_pending_pair_addr;
static uint32_t  g_pending_pair_pin;
static btstack_timer_source_t g_pair_timeout;

#define PAIR_CONFIRM_TIMEOUT_MS 120000

/*
 * Test mode (panctlctl test-tcp IP PORT): opens one mux TCP stream from
 * panctl directly, sends a canned HTTP request, streams response bytes
 * back to the ctl client. Bypasses pf divert entirely so we can prove
 * the mux ↔ Android ↔ internet path works before wiring up routing.
 */
static int      g_test_fd = -1;          /* dup'd ctl client fd; -1 if idle */
static uint16_t g_test_sid;
static char     g_test_host_str[64];     /* for the Host: header */
#endif

/* SDP UUID: 1f2f8a3e-7c4f-4f3a-9d2b-c0ffeec0ffee (PROTOCOL.md §5). */
static const uint8_t kServiceUuid128[16] = {
	0x1f, 0x2f, 0x8a, 0x3e, 0x7c, 0x4f, 0x4f, 0x3a,
	0x9d, 0x2b, 0xc0, 0xff, 0xee, 0xc0, 0xff, 0xee,
};

/* Trampolines exported by divert.c (for mux callback wiring). */
extern void divert_dispatch_tcp_open_ack(void *, uint16_t, uint16_t);
extern void divert_dispatch_tcp_data(void *, uint16_t, const uint8_t *, size_t);
extern void divert_dispatch_tcp_close_wr(void *, uint16_t);
extern void divert_dispatch_tcp_rst(void *, uint16_t, uint16_t);
extern void divert_dispatch_tcp_window(void *, uint16_t, uint32_t);
extern void divert_dispatch_udp_bind_ack(void *, uint16_t, uint16_t);
extern void divert_dispatch_udp_packet(void *, uint16_t, const struct endpoint *,
				      const uint8_t *, size_t);
extern void divert_dispatch_udp_close(void *, uint16_t);

/* In divert.c (OpenBSD path). On macOS panctl can't run anyway. */
extern void divert_drain_active_tcp(struct divert *);

/* ============================ mux integration ============================ */

#if defined(HAVE_BTSTACK)
/*
 * Writer callback: send raw bytes onto the open RFCOMM channel. BTstack's
 * rfcomm_send takes a credit-managed buffer; in v0 we trust BTstack's
 * internal flow control plus the Android side's HELLO initial_win=64KiB.
 */
/*
 * RFCOMM credit-aware outbound queue.
 *
 * BTstack v1.6.2 on the DM250 keeps a single-slot ACL TX buffer, so any two
 * back-to-back sends will hit BTSTACK_ACL_BUFFERS_FULL. v0 simply dropped
 * on backpressure — fine for HTTP/UDP that retries, fatal for TCP burst
 * loads (Tailscale, file transfer). v1 queues full mux frames and asks
 * BTstack to signal RFCOMM_EVENT_CAN_SEND_NOW; we drain on the event.
 */
/*
 * Queue sizing tuned for the DM250's 1 GB RAM. 256 × 1100 byte slots ate
 * ~280 KB which on top of tailscaled (50 MB+) and tun_tcp's 128 slot
 * tracking pushed the system into OOM panics during long-running tests.
 * 64 × 1100 = ~70 KB is plenty for the back-to-back mux frame bursts the
 * single-slot BCM43438 ACL TX buffer demands.
 */
#define SEND_Q_DEPTH 64
#define SEND_Q_BYTES 1100   /* largest single mux frame (~RFCOMM mtu 990 + header) */

struct send_q_entry {
	size_t  len;
	uint8_t data[SEND_Q_BYTES];
};

static struct send_q_entry g_send_q[SEND_Q_DEPTH];
static int g_send_q_head, g_send_q_tail;   /* head = oldest, tail = next free */
static int g_can_send_requested;

static int
send_q_empty(void) { return g_send_q_head == g_send_q_tail; }

static int
send_q_full(void)
{
	return ((g_send_q_tail + 1) % SEND_Q_DEPTH) == g_send_q_head;
}

static int
send_q_enqueue(const uint8_t *buf, size_t len)
{
	if (send_q_full()) return -1;
	if (len > SEND_Q_BYTES) return -1;
	memcpy(g_send_q[g_send_q_tail].data, buf, len);
	g_send_q[g_send_q_tail].len = len;
	g_send_q_tail = (g_send_q_tail + 1) % SEND_Q_DEPTH;
	return 0;
}

static void
send_q_request_can_send(void)
{
	if (!g_can_send_requested && g_rfcomm_cid != 0) {
		rfcomm_request_can_send_now_event(g_rfcomm_cid);
		g_can_send_requested = 1;
	}
}

static long
mux_write_rfcomm(void *ctx, const uint8_t *buf, size_t len)
{
	(void)ctx;
	if (g_rfcomm_cid == 0)
		return -1;
	/*
	 * If the queue already has pending data, preserve FIFO order: enqueue
	 * this and let the drain handler push it. Otherwise try a direct send.
	 */
	if (send_q_empty()) {
		uint8_t rc = rfcomm_send(g_rfcomm_cid, (uint8_t *)buf, (uint16_t)len);
		if (rc == 0)
			return (long)len;
		if (rc != BTSTACK_ACL_BUFFERS_FULL && rc != RFCOMM_NO_OUTGOING_CREDITS) {
			fprintf(stderr,
			    "[rfcomm_send] non-recoverable rc=0x%02x len=%zu\n",
			    rc, len);
			return -1;
		}
	}
	if (send_q_enqueue(buf, len) != 0) {
		fprintf(stderr, "[rfcomm_send] queue overflow, dropping %zu bytes\n",
		    len);
		return -1;
	}
	send_q_request_can_send();
	return (long)len;
}

static void
send_q_drain(void)
{
	while (!send_q_empty()) {
		struct send_q_entry *e = &g_send_q[g_send_q_head];
		uint8_t rc = rfcomm_send(g_rfcomm_cid, e->data, (uint16_t)e->len);
		if (rc == 0) {
			g_send_q_head = (g_send_q_head + 1) % SEND_Q_DEPTH;
			continue;
		}
		if (rc == BTSTACK_ACL_BUFFERS_FULL || rc == RFCOMM_NO_OUTGOING_CREDITS) {
			send_q_request_can_send();
			return;
		}
		/* Non-recoverable: drop and continue so we don't wedge. */
		fprintf(stderr,
		    "[rfcomm_send] drain rc=0x%02x dropping %zu bytes\n",
		    rc, e->len);
		g_send_q_head = (g_send_q_head + 1) % SEND_Q_DEPTH;
	}
}
#endif /* HAVE_BTSTACK */

static void
on_hello(void *ctx, const struct hello_payload *h)
{
	(void)ctx;
	fprintf(stderr, "peer HELLO ver=%u maxStreams=%u initWin=%uKiB\n",
	    h->proto_ver, h->max_streams, h->initial_win_kib);
}

static void
on_bye(void *ctx, uint16_t reason)
{
	(void)ctx;
	fprintf(stderr, "peer BYE reason=0x%04x\n", reason);
}

static void
on_ping(void *ctx, const uint8_t *nonce, size_t len)
{
	(void)ctx;
	/* PROTOCOL.md §3.3: respond with PONG echoing the nonce. */
	(void)mux_send_pong(g_mux, nonce, len);
}

static void
on_pong(void *ctx, const uint8_t *nonce, size_t len)
{
	(void)ctx; (void)nonce; (void)len;
	/* v0: no keepalive state tracking on this side. */
}

static void
on_protocol_error(void *ctx, const char *what)
{
	(void)ctx;
	fprintf(stderr, "mux protocol error: %s\n", what);
	/* Caller side will tear down RFCOMM on the next opportunity. */
}

#if defined(HAVE_BTSTACK)
/*
 * Test mode dispatchers. For the test stream id, write/close the dup'd
 * ctl client fd. For everything else, fall through to divert.c.
 */
static void
test_finish(void)
{
	if (g_test_fd >= 0)
		close(g_test_fd);
	g_test_fd = -1;
	g_test_sid = 0;
}

static void
main_on_tcp_open_ack(void *ctx, uint16_t sid, uint16_t status)
{
	if (g_tun_tcp && tun_tcp_on_tcp_open_ack(g_tun_tcp, sid, status))
		return;
	if (g_test_fd >= 0 && sid == g_test_sid) {
		if (status != 0) {
			dprintf(g_test_fd,
			    "(test) open failed, mux status=0x%04x\n", status);
			test_finish();
			return;
		}
		/* Auto-send a minimal HTTP/1.0 GET so we get bytes flowing. */
		char req[128];
		int n = snprintf(req, sizeof req,
		    "GET / HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",
		    g_test_host_str);
		(void)mux_send_tcp_data(g_mux, sid, (uint8_t *)req, (size_t)n);
		dprintf(g_test_fd,
		    "(test) stream %u open. Sent GET / HTTP/1.0 ... response:\n"
		    "----\n", sid);
		return;
	}
	divert_dispatch_tcp_open_ack(ctx, sid, status);
}

static void
main_on_tcp_data(void *ctx, uint16_t sid, const uint8_t *data, size_t len)
{
	if (g_tun_tcp && tun_tcp_on_tcp_data(g_tun_tcp, sid, data, len))
		return;
	if (g_test_fd >= 0 && sid == g_test_sid) {
		(void)write(g_test_fd, data, len);
		return;
	}
	divert_dispatch_tcp_data(ctx, sid, data, len);
}

static void
main_on_tcp_close_wr(void *ctx, uint16_t sid)
{
	if (g_tun_tcp && tun_tcp_on_tcp_close_wr(g_tun_tcp, sid))
		return;
	if (g_test_fd >= 0 && sid == g_test_sid) {
		dprintf(g_test_fd, "\n----\n(test) peer closed write side\n");
		test_finish();
		return;
	}
	divert_dispatch_tcp_close_wr(ctx, sid);
}

static void
main_on_tcp_rst(void *ctx, uint16_t sid, uint16_t reason)
{
	if (g_tun_tcp && tun_tcp_on_tcp_rst(g_tun_tcp, sid, reason))
		return;
	if (g_test_fd >= 0 && sid == g_test_sid) {
		dprintf(g_test_fd, "\n(test) RST reason=0x%04x\n", reason);
		test_finish();
		return;
	}
	divert_dispatch_tcp_rst(ctx, sid, reason);
}
#endif /* HAVE_BTSTACK */

/*
 * Build the callbacks struct with divert trampolines wired in (test
 * interceptors take precedence for the test stream id). mux_create copies
 * the struct, so the runtime mux session has stable pointers regardless
 * of where this struct lives.
 */
static void
make_mux_callbacks(struct mux_callbacks *cb)
{
	memset(cb, 0, sizeof *cb);
	cb->ctx = NULL;  /* trampolines use g_divert_for_mux singleton */
#if defined(HAVE_BTSTACK)
	cb->write = mux_write_rfcomm;

	cb->on_tcp_open_ack  = main_on_tcp_open_ack;
	cb->on_tcp_data      = main_on_tcp_data;
	cb->on_tcp_close_wr  = main_on_tcp_close_wr;
	cb->on_tcp_rst       = main_on_tcp_rst;
#else
	/* Sanity stub: never actually called when HAVE_BTSTACK is off. */
	cb->write = NULL;
	cb->on_tcp_open_ack  = divert_dispatch_tcp_open_ack;
	cb->on_tcp_data      = divert_dispatch_tcp_data;
	cb->on_tcp_close_wr  = divert_dispatch_tcp_close_wr;
	cb->on_tcp_rst       = divert_dispatch_tcp_rst;
#endif

	cb->on_hello = on_hello;
	cb->on_bye   = on_bye;
	cb->on_ping  = on_ping;
	cb->on_pong  = on_pong;

	cb->on_tcp_window    = divert_dispatch_tcp_window;
	cb->on_udp_bind_ack  = divert_dispatch_udp_bind_ack;
	cb->on_udp_packet    = divert_dispatch_udp_packet;
	cb->on_udp_close     = divert_dispatch_udp_close;

	cb->on_protocol_error = on_protocol_error;
}

/* ============================ BTstack handlers ============================ */

#if defined(HAVE_BTSTACK)

static void on_sdp_query(uint8_t, uint16_t, uint8_t *, uint16_t);

/*
 * Schedule a fresh SDP query. Called on RFCOMM close (Android app went away)
 * and on SDP failure (Android MuxServer not running yet). Idempotent: arming
 * the timer twice just resets it.
 */
static void
schedule_reconnect(void)
{
	btstack_run_loop_remove_timer(&g_reconnect_timer);
	btstack_run_loop_set_timer(&g_reconnect_timer, RECONNECT_DELAY_MS);
	btstack_run_loop_add_timer(&g_reconnect_timer);
}

static void
on_reconnect_timer(btstack_timer_source_t *ts)
{
	(void)ts;
	if (g_rfcomm_cid != 0)
		return;  /* already up */
	fprintf(stderr, "reconnect: re-issuing SDP query to Android\n");
	g_rfcomm_server_channel = 0;
	sdp_client_query_rfcomm_channel_and_name_for_uuid128(
	    &on_sdp_query, g_android_addr, kServiceUuid128);
}

/*
 * SDP query callback: BTstack delivers each found service. We expect one
 * (our service UUID). Record the RFCOMM server channel and then issue
 * rfcomm_create_channel from the SDP_EVENT_QUERY_COMPLETE handler.
 */
static void
on_sdp_query(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
	(void)packet_type;
	(void)channel;
	(void)size;
	uint8_t evt = hci_event_packet_get_type(packet);
	if (evt == SDP_EVENT_QUERY_RFCOMM_SERVICE) {
		g_rfcomm_server_channel = sdp_event_query_rfcomm_service_get_rfcomm_channel(packet);
		fprintf(stderr, "SDP found RFCOMM channel %u for our UUID\n",
		    g_rfcomm_server_channel);
	} else if (evt == SDP_EVENT_QUERY_COMPLETE) {
		uint8_t status = sdp_event_query_complete_get_status(packet);
		if (status != 0) {
			fprintf(stderr, "SDP query failed status=0x%02x; retry in %ums\n",
			    status, (unsigned)RECONNECT_DELAY_MS);
			schedule_reconnect();
			return;
		}
		if (g_rfcomm_server_channel == 0) {
			fprintf(stderr,
			    "SDP completed but no matching RFCOMM channel; retry in %ums\n",
			    (unsigned)RECONNECT_DELAY_MS);
			schedule_reconnect();
			return;
		}
		uint16_t cid_out;
		(void)cid_out;
		uint8_t rc = rfcomm_create_channel(&on_btstack_packet, g_android_addr,
		    g_rfcomm_server_channel, &cid_out);
		if (rc != 0) {
			fprintf(stderr,
			    "rfcomm_create_channel rc=0x%02x; retry in %ums\n",
			    rc, (unsigned)RECONNECT_DELAY_MS);
			schedule_reconnect();
		}
	}
}

static void
on_btstack_packet(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
	switch (packet_type) {
	case HCI_EVENT_PACKET:
		switch (hci_event_packet_get_type(packet)) {
		case BTSTACK_EVENT_STATE:
			if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
				fprintf(stderr, "HCI working; issuing SDP query to Android\n");
				sdp_client_query_rfcomm_channel_and_name_for_uuid128(
				    &on_sdp_query, g_android_addr, kServiceUuid128);
			}
			break;

		case RFCOMM_EVENT_CHANNEL_OPENED: {
			uint8_t status = rfcomm_event_channel_opened_get_status(packet);
			if (status != 0) {
				fprintf(stderr, "RFCOMM open status=0x%02x\n", status);
				return;
			}
			g_rfcomm_cid = rfcomm_event_channel_opened_get_rfcomm_cid(packet);
			fprintf(stderr, "RFCOMM open cid=%u mtu=%u\n", g_rfcomm_cid,
			    rfcomm_event_channel_opened_get_max_frame_size(packet));

			/* Send our HELLO. */
			struct hello_payload h = {
				.proto_ver = PROTO_VERSION, .flags = 0,
				.max_streams = 256, .initial_win_kib = 64,
			};
			(void)mux_send_hello(g_mux, &h);
			break;
		}

		case RFCOMM_EVENT_CHANNEL_CLOSED:
			fprintf(stderr, "RFCOMM closed; scheduling reconnect in %ums\n",
			    (unsigned)RECONNECT_DELAY_MS);
			g_rfcomm_cid = 0;
			/* Drop any queued bytes — peer is gone. */
			g_send_q_head = g_send_q_tail = 0;
			g_can_send_requested = 0;
			test_finish();
			schedule_reconnect();
			break;

		case RFCOMM_EVENT_CAN_SEND_NOW:
			g_can_send_requested = 0;
			send_q_drain();
			break;

		case GAP_EVENT_PAIRING_COMPLETE:
			fprintf(stderr, "pairing complete status=0x%02x\n",
			    gap_event_pairing_complete_get_status(packet));
			break;

		case HCI_EVENT_USER_CONFIRMATION_REQUEST: {
			/*
			 * BT 2.1+ SSP Numeric Comparison. Both sides display the
			 * same 6-digit code; operator verifies they match and runs
			 * `panctlctl confirm` (or `deny`).
			 */
			hci_event_user_confirmation_request_get_bd_addr(packet,
			    g_pending_pair_addr);
			g_pending_pair_pin =
			    hci_event_user_confirmation_request_get_numeric_value(packet)
			    % 1000000;
			g_pending_pair_set = 1;
			fprintf(stderr,
			    ">>> PAIR REQUEST from %02x:%02x:%02x:%02x:%02x:%02x"
			    " — passkey %06u\n"
			    "    Compare with the dialog on the peer (Android Settings)."
			    " Then run:\n"
			    "      panctlctl confirm     (codes match)\n"
			    "      panctlctl deny        (codes differ / unexpected)\n"
			    "    Auto-deny in %u seconds.\n",
			    g_pending_pair_addr[0], g_pending_pair_addr[1],
			    g_pending_pair_addr[2], g_pending_pair_addr[3],
			    g_pending_pair_addr[4], g_pending_pair_addr[5],
			    g_pending_pair_pin,
			    PAIR_CONFIRM_TIMEOUT_MS / 1000);
			/* Defensive remove first: if a second SSP request arrives
			 * (Android re-attempt while the previous one is still
			 * armed), add_timer asserts on double-insertion. */
			btstack_run_loop_remove_timer(&g_pair_timeout);
			btstack_run_loop_set_timer(&g_pair_timeout,
			    PAIR_CONFIRM_TIMEOUT_MS);
			btstack_run_loop_add_timer(&g_pair_timeout);
			break;
		}

		case HCI_EVENT_USER_PASSKEY_NOTIFICATION: {
			/* Peer is DisplayOnly; we just log the code it's showing. */
			uint32_t pin =
			    hci_event_user_passkey_notification_get_numeric_value(packet)
			    % 1000000;
			fprintf(stderr, "peer is showing passkey %06u (notification)\n",
			    pin);
			break;
		}

		case HCI_EVENT_PIN_CODE_REQUEST: {
			/* Legacy pairing (BT 2.0). Modern Androids don't trigger
			 * this; if it ever fires, reject — we want SSP only. */
			bd_addr_t a;
			hci_event_pin_code_request_get_bd_addr(packet, a);
			fprintf(stderr,
			    "legacy PIN_CODE_REQUEST from %02x:%02x:%02x:%02x:%02x:%02x"
			    " — rejecting (SSP only)\n",
			    a[0], a[1], a[2], a[3], a[4], a[5]);
			gap_pin_code_negative(a);
			break;
		}
		}
		break;

	case RFCOMM_DATA_PACKET:
		(void)channel;
		(void)mux_feed(g_mux, packet, size);
		break;
	}
}

/*
 * Pair timeout: if the operator doesn't run `panctlctl confirm|deny` within
 * PAIR_CONFIRM_TIMEOUT_MS, we send the negative reply ourselves. Stops a
 * stray pair request from sitting open if nobody is watching the log.
 */
static void
on_pair_timeout(btstack_timer_source_t *ts)
{
	(void)ts;
	if (!g_pending_pair_set)
		return;
	fprintf(stderr, "pair request timed out — auto-denying\n");
	hci_send_cmd(&hci_user_confirmation_request_negative_reply,
	    g_pending_pair_addr);
	g_pending_pair_set = 0;
}

/*
 * Discoverability state, gated over the control socket.  connectable stays
 * on always (paired devices reconnect); discoverable is only on during an
 * operator-initiated "advertise on" .. "advertise off" window.
 */
static int g_discoverable = 0;

/*
 * Called by ctl.c when a newline-terminated command arrives on the control
 * socket. reply_fd is the accepted client fd; we dprintf() a status line.
 */
void
ctl_on_command(const char *cmd, int reply_fd)
{
	if (strcmp(cmd, "confirm") == 0) {
		if (!g_pending_pair_set) {
			dprintf(reply_fd, "no pending pair request\n");
			return;
		}
		hci_send_cmd(&hci_user_confirmation_request_reply,
		    g_pending_pair_addr);
		dprintf(reply_fd, "confirmed pair with "
		    "%02x:%02x:%02x:%02x:%02x:%02x (passkey %06u)\n",
		    g_pending_pair_addr[0], g_pending_pair_addr[1],
		    g_pending_pair_addr[2], g_pending_pair_addr[3],
		    g_pending_pair_addr[4], g_pending_pair_addr[5],
		    g_pending_pair_pin);
		g_pending_pair_set = 0;
		btstack_run_loop_remove_timer(&g_pair_timeout);
	} else if (strcmp(cmd, "deny") == 0) {
		if (!g_pending_pair_set) {
			dprintf(reply_fd, "no pending pair request\n");
			return;
		}
		hci_send_cmd(&hci_user_confirmation_request_negative_reply,
		    g_pending_pair_addr);
		dprintf(reply_fd, "denied pair with "
		    "%02x:%02x:%02x:%02x:%02x:%02x\n",
		    g_pending_pair_addr[0], g_pending_pair_addr[1],
		    g_pending_pair_addr[2], g_pending_pair_addr[3],
		    g_pending_pair_addr[4], g_pending_pair_addr[5]);
		g_pending_pair_set = 0;
		btstack_run_loop_remove_timer(&g_pair_timeout);
	} else if (strcmp(cmd, "status") == 0) {
		dprintf(reply_fd, "advertise %s\n",
		    g_discoverable ? "on" : "off");
		if (g_pending_pair_set) {
			dprintf(reply_fd,
			    "pending pair from %02x:%02x:%02x:%02x:%02x:%02x"
			    " passkey %06u\n",
			    g_pending_pair_addr[0], g_pending_pair_addr[1],
			    g_pending_pair_addr[2], g_pending_pair_addr[3],
			    g_pending_pair_addr[4], g_pending_pair_addr[5],
			    g_pending_pair_pin);
		} else if (g_test_fd >= 0) {
			dprintf(reply_fd, "test stream %u in progress\n", g_test_sid);
		} else {
			dprintf(reply_fd, "idle\n");
		}
	} else if (strncmp(cmd, "test-tcp ", 9) == 0) {
		/*
		 * Direct mux TCP open without pf divert. Format: "test-tcp A.B.C.D PORT".
		 * Opens a stream to that IP:PORT via Android MuxServer, auto-sends
		 * a minimal HTTP GET, streams the response back to the ctl client.
		 */
		if (g_mux == NULL || g_rfcomm_cid == 0) {
			dprintf(reply_fd, "RFCOMM not open (mux idle)\n");
			return;
		}
		if (g_test_fd >= 0) {
			dprintf(reply_fd, "test already in progress\n");
			return;
		}
		unsigned a, b, c, d, port;
		if (sscanf(cmd + 9, "%u.%u.%u.%u %u", &a, &b, &c, &d, &port) != 5
		    || a > 255 || b > 255 || c > 255 || d > 255
		    || port == 0 || port > 65535) {
			dprintf(reply_fd, "usage: test-tcp A.B.C.D PORT\n");
			return;
		}
		int dupd = dup(reply_fd);
		if (dupd < 0) {
			dprintf(reply_fd, "dup: %s\n", strerror(errno));
			return;
		}
		g_test_sid = 0xF001;  /* high to avoid collision with divert allocations */
		g_test_fd  = dupd;
		snprintf(g_test_host_str, sizeof g_test_host_str,
		    "%u.%u.%u.%u", a, b, c, d);
		struct endpoint ep = { 0 };
		ep.addr_type = ADDR_TYPE_IPV4;
		ep.port      = (uint16_t)port;
		ep.addr_len  = 4;
		ep.addr[0] = (uint8_t)a; ep.addr[1] = (uint8_t)b;
		ep.addr[2] = (uint8_t)c; ep.addr[3] = (uint8_t)d;
		if (mux_send_tcp_open(g_mux, g_test_sid, &ep) < 0) {
			dprintf(reply_fd,
			    "mux_send_tcp_open failed (RFCOMM full?)\n");
			test_finish();
			return;
		}
		dprintf(reply_fd, "(test) requested mux open to %s:%u (stream %u)\n",
		    g_test_host_str, (unsigned)port, g_test_sid);
		/* dupd stays open; main_on_tcp_* will dprintf into it. */
	} else if (strcmp(cmd, "advertise on") == 0) {
		/* Become discoverable for this operator-initiated window. */
		gap_discoverable_control(1);
		g_discoverable = 1;
		dprintf(reply_fd, "advertise on (discoverable)\n");
	} else if (strcmp(cmd, "advertise off") == 0) {
		gap_discoverable_control(0);
		g_discoverable = 0;
		dprintf(reply_fd, "advertise off\n");
	} else if (strcmp(cmd, "advertise") == 0) {
		dprintf(reply_fd, "advertise %s\n",
		    g_discoverable ? "on" : "off");
	} else {
		dprintf(reply_fd,
		    "unknown command (use: confirm|deny|status|"
		    "advertise on|advertise off|test-tcp IP PORT)\n");
	}
}

/* ctl listen fd → run loop bridge (accept + eager-read on new client). */
static void
on_ctl_listen_ready(btstack_data_source_t *ds, btstack_data_source_callback_type_t cb)
{
	(void)ds;
	if (cb == DATA_SOURCE_CALLBACK_READ && g_ctl)
		ctl_handle_listen(g_ctl);
}

static void
on_tcp_sweep_timer(btstack_timer_source_t *ts)
{
	(void)ts;
	if (g_divert)
		divert_drain_active_tcp(g_divert);
	if (g_ctl)
		ctl_drain(g_ctl);
	/* tun_tcp sweep wants ~1 Hz; this 50ms timer fires 20× per second. */
	static unsigned slow_ctr = 0;
	if (++slow_ctr >= 20) {
		slow_ctr = 0;
		if (g_tun_tcp)
			tun_tcp_sweep(g_tun_tcp);
	}
	btstack_run_loop_set_timer(&g_tcp_sweep_timer, 50);  /* 50 ms */
	btstack_run_loop_add_timer(&g_tcp_sweep_timer);
}

/* divert fd → run_loop data source bridge. */
static void
on_divert_fd_ready(btstack_data_source_t *ds, btstack_data_source_callback_type_t cb)
{
	if (cb == DATA_SOURCE_CALLBACK_READ && g_divert)
		(void)divert_handle_fd(g_divert, btstack_run_loop_get_data_source_fd(ds));
}

#endif /* HAVE_BTSTACK */

/* ============================ argv ============================ */

struct args {
	const char *transport;     /* "libusb" or "h4" */
	const char *tty_dev;        /* "/dev/cua00" for h4 on DM250 */
	const char *hcd_path;       /* optional. On DM250 the kernel (bcmbt_fdt.c) loads
	                              /etc/firmware/BCM4343A1.hcd at boot, so userland
	                              patchram is unused. Phase A USB dongles that need
	                              patchram set this to a /etc/firmware path. */
	const char *android_bdaddr; /* "AA:BB:CC:DD:EE:FF" */
	enum divert_udp_mode udp_mode;
	const char *tun_dev;
	uint16_t pf_tcp_port;
	uint16_t pf_udp_port;
};

static void
usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s [options] --android-bdaddr AA:BB:CC:DD:EE:FF\n"
		"  -t libusb|h4         HCI transport (default: libusb)\n"
		"  -d /dev/cuaXX        UART device (h4 transport; DM250: /dev/cua00)\n"
		"  -f <file>            optional BCM patchram .hcd (h4 transport).\n"
		"                       DM250: omit (kernel handles patchram).\n"
		"  --android-bdaddr A   Android BT MAC (required)\n"
		"  --udp-mode pf|tun    UDP intercept mode (default: pf)\n"
		"  --tun-dev /dev/tunN  TUN device (when --udp-mode=tun)\n"
		"  --tcp-port N         pf divert-to TCP port (default: 9999)\n"
		"  --udp-port N         pf divert-packet UDP port (default: 9998)\n",
		argv0);
}

static int
parse_args(int argc, char **argv, struct args *a)
{
	a->transport = "libusb";
	a->udp_mode = DIVERT_UDP_MODE_PF;
	a->pf_tcp_port = 9999;
	a->pf_udp_port = 9998;
	a->tun_dev = "/dev/tun0";

	for (int i = 1; i < argc; i++) {
		const char *s = argv[i];
		if (strcmp(s, "-t") == 0 && i + 1 < argc)
			a->transport = argv[++i];
		else if (strcmp(s, "-d") == 0 && i + 1 < argc)
			a->tty_dev = argv[++i];
		else if (strcmp(s, "-f") == 0 && i + 1 < argc)
			a->hcd_path = argv[++i];
		else if (strcmp(s, "--android-bdaddr") == 0 && i + 1 < argc)
			a->android_bdaddr = argv[++i];
		else if (strcmp(s, "--udp-mode") == 0 && i + 1 < argc) {
			const char *m = argv[++i];
			if (strcmp(m, "pf") == 0)        a->udp_mode = DIVERT_UDP_MODE_PF;
			else if (strcmp(m, "tun") == 0)  a->udp_mode = DIVERT_UDP_MODE_TUN;
			else { usage(argv[0]); return -1; }
		}
		else if (strcmp(s, "--tun-dev") == 0 && i + 1 < argc)
			a->tun_dev = argv[++i];
		else if (strcmp(s, "--tcp-port") == 0 && i + 1 < argc)
			a->pf_tcp_port = (uint16_t)atoi(argv[++i]);
		else if (strcmp(s, "--udp-port") == 0 && i + 1 < argc)
			a->pf_udp_port = (uint16_t)atoi(argv[++i]);
		else if (strcmp(s, "-h") == 0 || strcmp(s, "--help") == 0) {
			usage(argv[0]); return -1;
		} else {
			fprintf(stderr, "unknown arg: %s\n", s);
			usage(argv[0]); return -1;
		}
	}
	if (a->android_bdaddr == NULL) {
		fprintf(stderr, "--android-bdaddr required\n");
		usage(argv[0]); return -1;
	}
	if (strcmp(a->transport, "h4") == 0) {
		if (a->tty_dev == NULL) {
			fprintf(stderr, "h4 transport requires -d\n");
			return -1;
		}
		/* hcd_path is optional. On DM250 the kernel loads patchram at boot,
		 * so omitting -f is the normal case. Phase A USB dongles that need
		 * BCM chipset patchram pass -f /etc/firmware/...hcd explicitly. */
	}
	return 0;
}

#if defined(HAVE_BTSTACK)
static int
parse_bdaddr(const char *s, bd_addr_t out)
{
	unsigned int b[6];
	if (sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x",
	    &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6)
		return -1;
	for (int i = 0; i < 6; i++)
		out[i] = (uint8_t)b[i];
	return 0;
}
#endif

/* ============================ main ============================ */

int
main(int argc, char **argv)
{
	setbuf(stdout, NULL);
	setbuf(stderr, NULL);

	struct args a;
	if (parse_args(argc, argv, &a) != 0)
		return 1;

#if !defined(HAVE_BTSTACK)
	/* References to suppress -Wunused on the stub-only build. */
	(void)kServiceUuid128;
	(void)make_mux_callbacks;
	(void)g_divert;
	fprintf(stderr,
	    "panctl: built without HAVE_BTSTACK; run tools/fetch-btstack.sh\n"
	    "        then rebuild with `make panctl-libusb` (or panctl-h4).\n");
	return 2;
#else
	if (parse_bdaddr(a.android_bdaddr, g_android_addr) != 0) {
		fprintf(stderr, "bad bdaddr: %s\n", a.android_bdaddr);
		return 1;
	}

	/* ------ BTstack init ------ */
	btstack_memory_init();
	btstack_run_loop_init(btstack_run_loop_posix_get_instance());

#  if defined(USE_LIBUSB_TRANSPORT)
	if (strcmp(a.transport, "libusb") == 0) {
		hci_init(hci_transport_usb_instance(), NULL);
	} else
#  endif
	{
		/* Phase B: UART H4. On DM250 the kernel (bcmbt_fdt.c) has already
		 * done GPIO bring-up + UART init + HCI Reset + Download Minidriver +
		 * patchram (/etc/firmware/BCM4343A1.hcd) + post-reset + Read BD_ADDR
		 * at boot. We just open /dev/cua00 at 115200 8N1 + CRTSCTS and start
		 * pushing HCI frames. No userland patchram. No baud switch
		 * (baudrate_main = 0 — smoke proved 115200 throughout works). */
		hci_transport_config_uart_t cfg = {
			.type = HCI_TRANSPORT_CONFIG_UART,
			.baudrate_init = 115200,
			.baudrate_main = 0,
			.flowcontrol = 1,
			.device_name = a.tty_dev,
		};
		hci_init(hci_transport_h4_instance(btstack_uart_posix_instance()), &cfg);
#  if defined(USE_BCM_CHIPSET)
		/* Only relevant for Phase A BCM USB dongles that need patchram.
		 * Skip entirely if -f was not given (DM250 path). */
		if (a.hcd_path != NULL) {
			btstack_chipset_bcm_set_hcd_folder_path(a.hcd_path);
			hci_set_chipset(btstack_chipset_bcm_instance());
		}
#  endif
	}

	l2cap_init();
	sdp_init();
	rfcomm_init();

	/* TLV setup (must come before sm_init / hci_set_link_key_db: SM with
	 * ENABLE_CROSS_TRANSPORT_KEY_DERIVATION walks the LE device DB during
	 * init, and asserts if no backing store is configured). */
	static btstack_tlv_posix_t tlv_ctx;
	const btstack_tlv_t *tlv_impl =
	    btstack_tlv_posix_init_instance(&tlv_ctx, "/var/db/panctl/tlv.dat");
	btstack_tlv_set_instance(tlv_impl, &tlv_ctx);
	hci_set_link_key_db(btstack_link_key_db_tlv_get_instance(tlv_impl, &tlv_ctx));
	le_device_db_tlv_configure(tlv_impl, &tlv_ctx);

	sm_init();
	gap_set_local_name("pomera-panctl");
	/*
	 * Classic SSP Numeric Comparison for MITM protection. Both sides
	 * DISPLAY_YES_NO; operator confirms the 6-digit code via panctlctl.
	 *
	 * Earlier this combo (Numeric Comparison) hit `pairing complete
	 * status=0x05` (HCI Authentication Failure) repeatedly when LE Secure
	 * Connections + Cross Transport Key Derivation were enabled in
	 * btstack_config.h — the LE/CTKD path tried to derive an LE LTK from
	 * the Classic link key and the Android side's cached LE state went
	 * out of sync with our changing BD_ADDR. The patch
	 *   btstack/patches/openbsd-rk3128/
	 *     btstack-config-disable-le-secure-for-pair-stability.patch
	 * disables both, leaving pure Classic SSP. With that in place
	 * Numeric Comparison pairs cleanly (status=0x00) and keeps MITM
	 * protection — Just Works was only a temporary fallback.
	 */
	gap_ssp_set_io_capability(SSP_IO_CAPABILITY_DISPLAY_YES_NO);
	gap_ssp_set_auto_accept(1);  /* code 表示するが pomera は user 待たず即答 */
	/*
	 * Accept inbound RFCOMM connects at all times so an already-paired
	 * Android can reconnect.  Discoverability (showing up in Android's
	 * Bluetooth scan) is gated: it starts OFF and is toggled on demand
	 * over the control socket ("advertise on" / "advertise off"), so the
	 * pomera is only scannable during an operator-initiated window.
	 */
	gap_connectable_control(1);
	gap_discoverable_control(0);
	g_discoverable = 0;

	g_hci_event_cb.callback = &on_btstack_packet;
	hci_add_event_handler(&g_hci_event_cb);
	/* v1.6.2: RFCOMM packet handler is bound via rfcomm_create_channel's
	 * first arg; there is no rfcomm_register_packet_handler in this API. */

	/*
	 * Start the chip first. Calling mux_create()/divert_create() before
	 * hci_power_control() on OpenBSD/armv7 reproducibly triggers a
	 * BTSTACK_EVENT_POWERON_FAILED inside hci_power_control() — the calloc
	 * done by mux_create perturbs heap state in a way BTstack's transport
	 * open path doesn't tolerate. Bisected 2026-05-29; minimal repro is
	 * smoke + mux_create. Keeping HCI bring-up isolated by ordering.
	 */
	hci_power_control(HCI_POWER_ON);

	/* ------ mux + divert wiring (post power-on) ------ */
	struct mux_callbacks cb;
	make_mux_callbacks(&cb);
	g_mux = mux_create(&cb);
	if (g_mux == NULL) {
		fprintf(stderr, "mux_create failed\n");
		return 2;
	}

	struct divert_config dcfg = {
		.pf_tcp_port = a.pf_tcp_port,
		.udp_mode = a.udp_mode,
		.pf_udp_port = a.pf_udp_port,
		.tun_dev = a.tun_dev,
		.mux = g_mux,
	};
	g_divert = divert_create(&dcfg);
	if (g_divert == NULL) {
		fprintf(stderr, "divert_create failed: %s\n", strerror(errno));
		return 2;
	}

	/* Userland TCP shim (tun mode only). divert reads tun and demuxes by
	 * IP proto: UDP keeps the existing path, TCP forwards to tun_tcp. */
	int tunfd = divert_get_tun_fd(g_divert);
	if (tunfd >= 0) {
		g_tun_tcp = tun_tcp_create(tunfd, g_mux);
		if (g_tun_tcp == NULL) {
			fprintf(stderr, "tun_tcp_create failed\n");
			return 2;
		}
		divert_set_tun_other_cb(g_divert,
		    (divert_tun_other_cb)tun_tcp_handle_packet, g_tun_tcp);
	}

	/* Register divert fds with BTstack run loop so its select() picks them up. */
	int fds[8]; size_t nfds = 0;
	(void)divert_get_fds(g_divert, fds, sizeof fds / sizeof fds[0], &nfds);
	static btstack_data_source_t ds[8];
	for (size_t i = 0; i < nfds; i++) {
		btstack_run_loop_set_data_source_fd(&ds[i], fds[i]);
		btstack_run_loop_set_data_source_handler(&ds[i], &on_divert_fd_ready);
		btstack_run_loop_enable_data_source_callbacks(&ds[i], DATA_SOURCE_CALLBACK_READ);
		btstack_run_loop_add_data_source(&ds[i]);
	}

	/* ------ ctl socket (operator confirms BT pair requests) ------ */
	g_ctl = ctl_create("/var/run/panctl/ctl.sock");
	if (g_ctl == NULL) {
		fprintf(stderr, "ctl_create failed: %s\n", strerror(errno));
		return 2;
	}
	static btstack_data_source_t ctl_ds;
	btstack_run_loop_set_data_source_fd(&ctl_ds, ctl_listen_fd(g_ctl));
	btstack_run_loop_set_data_source_handler(&ctl_ds, &on_ctl_listen_ready);
	btstack_run_loop_enable_data_source_callbacks(&ctl_ds, DATA_SOURCE_CALLBACK_READ);
	btstack_run_loop_add_data_source(&ctl_ds);
	/* Accepted client fds are drained by the existing 50ms sweep timer
	 * (on_tcp_sweep_timer), which calls ctl_drain() alongside divert. */

	/* Timer for 30s pair auto-deny (one-shot, re-armed on each request). */
	btstack_run_loop_set_timer_handler(&g_pair_timeout, &on_pair_timeout);

	/* Timer for SDP/RFCOMM reconnect (one-shot, armed by schedule_reconnect). */
	btstack_run_loop_set_timer_handler(&g_reconnect_timer, &on_reconnect_timer);

	/* Timer to sweep accepted TCP sockets every 50 ms. */
	btstack_run_loop_set_timer_handler(&g_tcp_sweep_timer, &on_tcp_sweep_timer);
	btstack_run_loop_set_timer(&g_tcp_sweep_timer, 50);
	btstack_run_loop_add_timer(&g_tcp_sweep_timer);

	btstack_run_loop_execute();

	ctl_destroy(g_ctl);
	tun_tcp_destroy(g_tun_tcp);
	divert_destroy(g_divert);
	mux_destroy(g_mux);
	return 0;
#endif /* HAVE_BTSTACK */
}
