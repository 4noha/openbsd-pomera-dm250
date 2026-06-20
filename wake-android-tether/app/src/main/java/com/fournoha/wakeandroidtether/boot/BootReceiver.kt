// SPDX-License-Identifier: MIT
// Copyright (c) 2026 4noha

package com.fournoha.wakeandroidtether.boot

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import com.fournoha.wakeandroidtether.bt.RfcommService
import com.fournoha.wakeandroidtether.prefs.Prefs

class BootReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent?) {
        if (!Prefs(context).listenerEnabled) return
        RfcommService.start(context)
    }
}
