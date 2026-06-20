// SPDX-License-Identifier: MIT
// Copyright (c) 2026 4noha

package com.fournoha.wakeandroidtether.mux

import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.isActive
import java.io.OutputStream

/**
 * Serializes all outbound frames onto a single BluetoothSocket OutputStream.
 * Stream handlers enqueue ready-to-send frames via [send]; this writer pulls
 * them off in order. Closes when the channel is closed or the underlying
 * stream throws.
 */
class FrameWriter(private val out: OutputStream) {
    private val queue: Channel<Payload> = Channel(capacity = Channel.BUFFERED)

    data class Payload(
        val type: Int,
        val streamId: Int,
        val body: ByteArray,
        /** Extra bytes appended after [body], without a copy. Optional. */
        val extra: ByteArray? = null,
    )

    suspend fun send(type: Int, streamId: Int, body: ByteArray = EMPTY, extra: ByteArray? = null) {
        queue.send(Payload(type, streamId, body, extra))
    }

    fun close() {
        queue.close()
    }

    /**
     * Drain loop. Run on a single coroutine pinned to Dispatchers.IO.
     * Returns when the channel is closed or the underlying write throws.
     */
    suspend fun drain() = coroutineScope {
        try {
            for (p in queue) {
                if (!isActive) break
                val totalLen = p.body.size + (p.extra?.size ?: 0)
                val header = FrameCodec.header(p.type, p.streamId, totalLen)
                out.write(header)
                if (p.body.isNotEmpty()) out.write(p.body)
                p.extra?.let { if (it.isNotEmpty()) out.write(it) }
                out.flush()
            }
        } finally {
            try { out.close() } catch (_: Throwable) {}
        }
    }

    companion object {
        private val EMPTY = ByteArray(0)
    }
}
