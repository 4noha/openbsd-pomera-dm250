// SPDX-License-Identifier: MIT
// Copyright (c) 2026 4noha

package com.fournoha.wakeandroidtether.mux

import android.util.Log
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.IOException
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.Inet4Address
import java.net.InetSocketAddress
import java.net.SocketException
import java.util.concurrent.atomic.AtomicLong

/**
 * A UDP "stream" multiplexed over the RFCOMM mux. Backed by a single
 * DatagramSocket bound to an ephemeral port. Outbound packets are sent
 * to whatever remote endpoint the latest UDP_PACKET specifies; the receive
 * pump forwards return datagrams back to pomera.
 *
 * Idle timeout (no traffic in either direction) closes the socket and
 * emits UDP_CLOSE (PROTOCOL.md §3.13).
 */
class UdpStream(
    private val streamId: Int,
    private val writer: FrameWriter,
    private val scope: CoroutineScope,
    private val onClosed: (Int) -> Unit,
) {
    private val tag = "WAT/Udp[$streamId]"

    private val socket = DatagramSocket().apply { reuseAddress = true }
    private val lastActivityMs = AtomicLong(System.currentTimeMillis())
    private var pumpJob: Job? = null

    suspend fun open(): Boolean = withContext(Dispatchers.IO) {
        writer.send(FrameType.UDP_BIND_ACK, streamId, u16BE(AckStatus.OK))
        pumpJob = scope.launch(Dispatchers.IO) { pumpReceiveLoop() }
        true
    }

    suspend fun onPeerPacket(remote: Endpoint, datagram: ByteArray): Boolean =
        withContext(Dispatchers.IO) {
            if (remote.addrType != AddrType.IPV4) return@withContext false
            val addr = Inet4Address.getByAddress(remote.addr)
            try {
                socket.send(DatagramPacket(datagram, datagram.size, addr, remote.port))
                lastActivityMs.set(System.currentTimeMillis())
                true
            } catch (t: IOException) {
                Log.w(tag, "udp send failed", t)
                false
            }
        }

    fun close() {
        try { socket.close() } catch (_: Throwable) {}
        pumpJob?.cancel()
        onClosed(streamId)
    }

    private suspend fun pumpReceiveLoop() {
        val buf = ByteArray(MAX_UDP)
        val pkt = DatagramPacket(buf, buf.size)
        try {
            socket.soTimeout = TIMEOUT_TICK_MS
            while (scope.isActive) {
                try {
                    socket.receive(pkt)
                } catch (_: java.net.SocketTimeoutException) {
                    // idle tick; check for full idle timeout below
                    if (System.currentTimeMillis() - lastActivityMs.get() > IDLE_TIMEOUT_MS) {
                        break
                    }
                    continue
                } catch (_: SocketException) {
                    break
                }
                lastActivityMs.set(System.currentTimeMillis())
                val src = pkt.socketAddress as InetSocketAddress
                val addr = src.address
                if (addr !is Inet4Address) continue
                val ep = Endpoint(AddrType.IPV4, src.port, addr.address)
                val header = ByteArray(ep.encodedSize())
                ep.encodeInto(header, 0)
                val datagram = pkt.data.copyOfRange(pkt.offset, pkt.offset + pkt.length)
                writer.send(FrameType.UDP_PACKET, streamId, header, datagram)
            }
        } finally {
            writer.send(FrameType.UDP_CLOSE, streamId)
            try { socket.close() } catch (_: Throwable) {}
            onClosed(streamId)
        }
    }

    companion object {
        private const val MAX_UDP: Int = 65507
        private const val IDLE_TIMEOUT_MS: Long = 120_000
        private const val TIMEOUT_TICK_MS: Int = 5_000
    }
}
