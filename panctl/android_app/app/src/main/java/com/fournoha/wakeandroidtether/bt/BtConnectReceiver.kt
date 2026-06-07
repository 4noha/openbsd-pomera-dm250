// SPDX-License-Identifier: MIT
// Copyright (c) 2026 4noha
package com.fournoha.wakeandroidtether.bt

import android.bluetooth.BluetoothDevice
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.os.Build
import android.util.Log
import com.fournoha.wakeandroidtether.prefs.Prefs

/**
 * Wake the RFCOMM listener when pomera initiates a Bluetooth connection.
 *
 * Pomera's panctl is the active party: it ACL-connects + SDP-queries + RFCOMM-
 * connects. Without this receiver, Android would only accept if the listener
 * was already started manually from the launcher. With it, the user just opens
 * pomera — Android's BT stack delivers ACL_CONNECTED, we start the foreground
 * service in time for the SDP/RFCOMM round-trip.
 *
 * ACL_CONNECTED is on the Android 8+ implicit-broadcast exemption list, so
 * manifest registration is sufficient (no need for context-registered receiver
 * + persistent process).
 *
 * Filtering by Prefs.listenerEnabled means the user keeps control: if they
 * have toggled the listener off in the app, we don't auto-revive on every BT
 * connect (Bluetooth audio devices, etc. would all trigger the broadcast).
 */
class BtConnectReceiver : BroadcastReceiver() {
    private val tag = "WAT/BtConnect"

    override fun onReceive(context: Context, intent: Intent?) {
        if (intent?.action != BluetoothDevice.ACTION_ACL_CONNECTED) return

        val device: BluetoothDevice? = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE, BluetoothDevice::class.java)
        } else {
            @Suppress("DEPRECATION")
            intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE)
        }

        // We don't check addr against a stored allowlist — pairing already
        // gated who can reach RFCOMM. listenerEnabled is the user's master
        // switch; if off, ignore.
        val addr = device?.address ?: "?"
        if (!Prefs(context).listenerEnabled) {
            Log.i(tag, "ACL_CONNECTED from $addr but listener disabled, ignoring")
            return
        }
        Log.i(tag, "ACL_CONNECTED from $addr; starting RfcommService")
        RfcommService.start(context)
    }
}
