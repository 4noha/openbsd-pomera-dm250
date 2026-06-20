// SPDX-License-Identifier: MIT
// Copyright (c) 2026 4noha

package com.fournoha.wakeandroidtether.bt

import android.bluetooth.BluetoothAdapter
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.util.Log
import com.fournoha.wakeandroidtether.prefs.Prefs

/**
 * Restart [RfcommService] when the user toggles Bluetooth back on, and stop it
 * cleanly when BT turns off. Without this, a BT off→on cycle leaves the service
 * in a zombie state (foreground notification up but no accept loop), and pomera
 * cannot reconnect.
 */
class BluetoothStateReceiver : BroadcastReceiver() {
    private val tag = "WAT/BtState"

    override fun onReceive(context: Context, intent: Intent?) {
        if (intent?.action != BluetoothAdapter.ACTION_STATE_CHANGED) return
        val state = intent.getIntExtra(BluetoothAdapter.EXTRA_STATE, BluetoothAdapter.ERROR)
        Log.i(tag, "BT state -> $state")
        when (state) {
            BluetoothAdapter.STATE_ON -> {
                if (Prefs(context).listenerEnabled) {
                    Log.i(tag, "BT on; restarting RfcommService")
                    RfcommService.start(context)
                }
            }
            BluetoothAdapter.STATE_TURNING_OFF, BluetoothAdapter.STATE_OFF -> {
                Log.i(tag, "BT off; stopping RfcommService")
                RfcommService.stop(context)
            }
        }
    }
}
