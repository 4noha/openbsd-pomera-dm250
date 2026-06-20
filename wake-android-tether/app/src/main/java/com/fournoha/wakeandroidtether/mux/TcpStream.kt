// SPDX-License-Identifier: MIT
// Copyright (c) 2026 4noha

package com.fournoha.wakeandroidtether.mux

import android.util.Log
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.cancel
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import java.io.IOException
import java.net.ConnectException
import java.net.Inet4Address
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.NoRouteToHostException
import java.net.Socket
import java.net.SocketTimeoutException
import java.net.UnknownHostException

/**
 * A single TCP stream multiplexed over the RFCOMM mux. Owns one outbound
 * Socket to the cell network. Reads from the socket are forwarded to pomera
 * as TCP_DATA frames; writes coming in via [onPeerData] are pushed to the
 * socket. Closes are bridged in both directions.
 */
class TcpStream(
    private val streamId: Int,
    private val endpoint: Endpoint,
    private val writer: FrameWriter,
    private val scope: CoroutineScope,
    private val onClosed: (Int) -> Unit,
    /** Peer's declared receive window in bytes. We must not send beyond this. */
    initialPeerWindowBytes: Int,
) {
    private val tag = "WAT/Tcp[$streamId]"

    private val socket = Socket()
    private var pumpJob: Job? = null

    /** Window the peer (pomera) has advertised to us — bytes we may send. */
    private var peerWindowRemaining: Int = initialPeerWindowBytes
    private val winLock = Mutex()
    private val winSignal = kotlinx.coroutines.channels.Channel<Unit>(capacity = 1)

    /**
     * Bytes received from peer that have been written to the destination
     * socket but not yet credited back. When this reaches half of our own
     * advertised window we send a TCP_WINDOW.
     */
    private var receivedSinceCredit: Int = 0

    /** Connect, send TCP_OPEN_ACK, and spawn the read pump. Blocking IO inside. */
    suspend fun open(connectTimeoutMs: Int = 10_000): Boolean = withContext(Dispatchers.IO) {
        val addr: InetAddress = try {
            if (endpoint.addrType == AddrType.IPV4) {
                Inet4Address.getByAddress(endpoint.addr)
            } else {
                writer.send(FrameType.TCP_OPEN_ACK, streamId, u16BE(AckStatus.EAFNOSUPPORT))
                onClosed(streamId)
                return@withContext false
            }
        } catch (t: UnknownHostException) {
            writer.send(FrameType.TCP_OPEN_ACK, streamId, u16BE(AckStatus.EOTHER))
            onClosed(streamId)
            return@withContext false
        }

        val status: Int = try {
            socket.connect(InetSocketAddress(addr, endpoint.port), connectTimeoutMs)
            socket.tcpNoDelay = true
            AckStatus.OK
        } catch (t: ConnectException) {
            AckStatus.ECONNREFUSED
        } catch (t: NoRouteToHostException) {
            AckStatus.EHOSTUNREACH
        } catch (t: SocketTimeoutException) {
            AckStatus.ETIMEDOUT
        } catch (t: IOException) {
            Log.w(tag, "connect failed", t)
            AckStatus.EOTHER
        }

        writer.send(FrameType.TCP_OPEN_ACK, streamId, u16BE(status))
        if (status != AckStatus.OK) {
            try { socket.close() } catch (_: Throwable) {}
            onClosed(streamId)
            return@withContext false
        }

        pumpJob = scope.launch(Dispatchers.IO) { pumpReadLoop() }
        true
    }

    /**
     * Forward a TCP_DATA frame from peer to the socket. Returns false if the
     * socket is already closed (caller should send TCP_RST).
     */
    suspend fun onPeerData(data: ByteArray): Boolean = withContext(Dispatchers.IO) {
        try {
            val out = socket.getOutputStream()
            out.write(data)
            out.flush()
        } catch (t: IOException) {
            return@withContext false
        }
        // Credit peer back after every chunk. With v0 we do not implement
        // explicit windowing on this side; just acknowledge what we forwarded.
        receivedSinceCredit += data.size
        if (receivedSinceCredit >= CREDIT_CHUNK) {
            val credit = receivedSinceCredit
            receivedSinceCredit = 0
            writer.send(FrameType.TCP_WINDOW, streamId, u32BE(credit.toLong()))
        }
        true
    }

    /** Peer is done writing; close our socket write half. */
    suspend fun onPeerCloseWr() = withContext(Dispatchers.IO) {
        try { socket.shutdownOutput() } catch (_: Throwable) {}
    }

    /** Update credit allowance for our outbound writes. */
    suspend fun onPeerWindowUpdate(credit: Long) {
        winLock.withLock {
            val newWin = (peerWindowRemaining.toLong() + credit).coerceAtMost(Int.MAX_VALUE.toLong())
            peerWindowRemaining = newWin.toInt()
        }
        winSignal.trySend(Unit)
    }

    /** Force-close (TCP_RST received or local error). */
    fun reset() {
        try { socket.close() } catch (_: Throwable) {}
        pumpJob?.cancel()
        onClosed(streamId)
    }

    // ---- internals --------------------------------------------------------

    private suspend fun pumpReadLoop() {
        val buf = ByteArray(CHUNK_SIZE)
        try {
            val input = socket.getInputStream()
            while (scope.isActive) {
                val avail = waitForCredit()
                if (avail <= 0) break
                val toRead = minOf(buf.size, avail)
                val n = input.read(buf, 0, toRead)
                if (n < 0) {
                    writer.send(FrameType.TCP_CLOSE_WR, streamId)
                    break
                }
                if (n == 0) continue
                consumeCredit(n)
                writer.send(FrameType.TCP_DATA, streamId, buf.copyOf(n))
            }
        } catch (t: IOException) {
            // Peer-side socket closed or network reset
            writer.send(FrameType.TCP_RST, streamId, u16BE(RstReason.PEER_RESET))
        } finally {
            try { socket.close() } catch (_: Throwable) {}
            onClosed(streamId)
        }
    }

    private suspend fun waitForCredit(): Int {
        while (scope.isActive) {
            val w = winLock.withLock { peerWindowRemaining }
            if (w > 0) return w
            // Block until onPeerWindowUpdate signals us, or we're cancelled.
            winSignal.receive()
        }
        return 0
    }

    private suspend fun consumeCredit(n: Int) {
        winLock.withLock { peerWindowRemaining -= n }
    }

    companion object {
        /** Single TCP_DATA frame payload size. Smaller frames let other streams interleave. */
        private const val CHUNK_SIZE: Int = 2048
        /** Send TCP_WINDOW back to peer after we've absorbed this many bytes. */
        private const val CREDIT_CHUNK: Int = 16 * 1024
    }
}
