// SPDX-License-Identifier: MIT
// Copyright (c) 2026 4noha

package com.fournoha.wakeandroidtether.bt

import java.util.UUID

/**
 * RFCOMM SDP record constants. The actual wire protocol is binary mux frames
 * defined in [com.fournoha.wakeandroidtether.mux.Frame]; the spec lives at
 * ../../PROTOCOL.md in this repo.
 */
object Protocol {
    val UUID_SERVICE: UUID = UUID.fromString("1f2f8a3e-7c4f-4f3a-9d2b-c0ffeec0ffee")
    const val SDP_NAME = "WakeAndroidTether"

    /** Current mux protocol version. v0 = draft. */
    const val PROTO_VERSION: Int = 0x00
}
