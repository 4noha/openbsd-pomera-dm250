// SPDX-License-Identifier: MIT
// Copyright (c) 2026 4noha

package com.fournoha.wakeandroidtether.prefs

import android.content.Context
import android.content.SharedPreferences

/**
 * Tiny persistent state. v0 only remembers whether the user has enabled the
 * listener, so that BootReceiver knows whether to restart the service.
 * Authentication is provided by BT pairing (Secure Simple Pairing), not by
 * a shared token at this layer — see PROTOCOL.md §6.
 */
class Prefs(context: Context) {
    private val sp: SharedPreferences = context.getSharedPreferences("wat", Context.MODE_PRIVATE)

    var listenerEnabled: Boolean
        get() = sp.getBoolean(K_LISTENER_ENABLED, false)
        set(value) { sp.edit().putBoolean(K_LISTENER_ENABLED, value).apply() }

    companion object {
        private const val K_LISTENER_ENABLED = "listener_enabled"
    }
}
