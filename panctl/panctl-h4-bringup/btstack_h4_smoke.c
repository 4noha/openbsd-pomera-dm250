/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 4noha */
/* btstack_h4_smoke.c — minimal BTstack H4 smoke test for pomera DM250.
 *
 * Opens /dev/cua00, brings up HCI, waits for HCI_STATE_WORKING, prints
 * Local Version Info and BD_ADDR, then exits. Validates that BTstack's
 * H4 transport + posix run loop work on OpenBSD against the AP6212A.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

#include "btstack_config.h"
#include "btstack_run_loop.h"
#include "btstack_run_loop_posix.h"
/* removed */
#include "btstack_uart.h"
#include "btstack_event.h"
#include "btstack_signal.h"
#include "btstack_debug.h"
#include "btstack_memory.h"
#include "hci.h"
#include "hci_dump.h"
#include "hci_transport.h"
#include "hci_transport_h4.h"

static hci_transport_config_uart_t uart_cfg = {
	.type           = HCI_TRANSPORT_CONFIG_UART,
	.baudrate_init  = 115200,
	.baudrate_main  = 0,           /* no baud switch */
	.flowcontrol    = 1,           /* RTS/CTS */
	.device_name    = "/dev/cua00",
};

static btstack_packet_callback_registration_t hci_event_callback;

static void packet_handler(uint8_t packet_type, uint16_t channel,
                           uint8_t *packet, uint16_t size) {
	(void)channel;
	(void)size;
	if (packet_type != HCI_EVENT_PACKET) return;

	uint8_t evt = hci_event_packet_get_type(packet);
	printf("[evt] type=0x%02x size=%u\n", evt, size);

	switch (evt) {
	case BTSTACK_EVENT_STATE: {
		uint8_t st = btstack_event_state_get_state(packet);
		printf("  [state] %u\n", st);
		if (st == HCI_STATE_WORKING) {
			bd_addr_t a;
			gap_local_bd_addr(a);
			printf("  ★ HCI working! BD_ADDR %02x:%02x:%02x:%02x:%02x:%02x\n",
			    a[0],a[1],a[2],a[3],a[4],a[5]);
			hci_power_control(HCI_POWER_OFF);
			exit(0);
		}
		break;
	}
	case HCI_EVENT_COMMAND_COMPLETE:
		printf("  [cmd-complete] opcode=0x%04x\n",
		    hci_event_command_complete_get_command_opcode(packet));
		break;
	}
}

int main(int argc, char **argv) {
	(void)argc; (void)argv;
	printf("=== btstack_h4_smoke for /dev/cua00 ===\n");

	btstack_memory_init();
	btstack_run_loop_init(btstack_run_loop_posix_get_instance());

	const hci_transport_t *transport = hci_transport_h4_instance(
	    btstack_uart_posix_instance());
	hci_init(transport, &uart_cfg);

	hci_event_callback.callback = &packet_handler;
	hci_add_event_handler(&hci_event_callback);

	hci_power_control(HCI_POWER_ON);

	btstack_run_loop_execute();
	return 0;
}
