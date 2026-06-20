// SPDX-License-Identifier: MIT
// Copyright (c) 2026 4noha

package com.fournoha.wakeandroidtether.mux

import android.bluetooth.BluetoothSocket
import android.util.Log
import com.fournoha.wakeandroidtether.bt.Protocol
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.DataInputStream
import java.io.IOException
import java.util.concurrent.ConcurrentHashMap

/**
 * Runs the mux protocol (PROTOCOL.md) over a connected BluetoothSocket.
 * Spawns a reader coroutine plus a single writer coroutine, and creates
 * per-stream TCP/UDP handlers on demand. Suspends until the link drops or
 * a fatal protocol error is observed.
 *
 * Lifecycle:
 *   ctor → [run] (blocks) → BluetoothSocket eventually closed externally
 *                            or peer sends BYE → returns
 */
class MuxServer(
    private val btSocket: BluetoothSocket,
    parent: Job? = null,
) {
    private val tag = "WAT/Mux"

    private val scope: CoroutineScope = CoroutineScope(SupervisorJob(parent) + Dispatchers.IO)

    private val writer = FrameWriter(btSocket.outputStream)

    private val tcpStreams = ConcurrentHashMap<Int, TcpStream>()
    private val udpStreams = ConcurrentHashMap<Int, UdpStream>()

    /** Window the peer (pomera) advertised in HELLO. Bytes we may send on each TCP stream. */
    @Volatile private var peerInitialWinBytes: Int = DEFAULT_INITIAL_WIN_KIB * 1024

    suspend fun run() {
        // Start writer drain on its own coroutine; it lives until writer.close().
        val writerJob = scope.launch { writer.drain() }

        try {
            sendHello()
            handshakeAndLoop()
        } catch (t: IOException) {
            Log.w(tag, "mux IO error: ${t.message}")
        } finally {
            shutdown()
            writerJob.cancel()
        }
    }

    private suspend fun sendHello() {
        val hello = Hello(
            protoVer = Protocol.PROTO_VERSION,
            flags = 0,
            maxStreams = MAX_STREAMS,
            initialWinKib = DEFAULT_INITIAL_WIN_KIB,
        )
        writer.send(FrameType.HELLO, 0, hello.encode())
    }

    private suspend fun handshakeAndLoop() {
        val input = DataInputStream(btSocket.inputStream.buffered())
        var helloSeen = false

        while (true) {
            val frame: RawFrame = withContext(Dispatchers.IO) {
                FrameCodec.read(input)
            } ?: run {
                Log.i(tag, "peer closed input stream")
                return
            }

            if (frame.version != Protocol.PROTO_VERSION) {
                Log.w(tag, "version mismatch peer=${frame.version} ours=${Protocol.PROTO_VERSION}")
                sendBye(REASON_VERSION_MISMATCH)
                return
            }

            // Peer must HELLO first (or send it concurrently with ours). We
            // only refuse data frames before HELLO; control frames (PING/BYE)
            // are tolerated either way.
            if (!helloSeen && frame.type !in EARLY_TYPES) {
                Log.w(tag, "data frame ${FrameType.name(frame.type)} before HELLO")
                sendBye(REASON_PROTOCOL)
                return
            }

            when (frame.type) {
                FrameType.HELLO -> {
                    val h = Hello.decode(frame.payload)
                    if (h.protoVer != Protocol.PROTO_VERSION) {
                        sendBye(REASON_VERSION_MISMATCH); return
                    }
                    peerInitialWinBytes = (h.initialWinKib * 1024).coerceAtLeast(1024)
                    helloSeen = true
                    Log.i(tag, "HELLO peer ver=${h.protoVer} maxStreams=${h.maxStreams} initialWin=${h.initialWinKib}KiB")
                }
                FrameType.BYE -> {
                    val reason = if (frame.payload.size >= 2) frame.payload.readU16BE(0) else 0
                    Log.i(tag, "BYE from peer reason=$reason")
                    return
                }
                FrameType.PING -> {
                    writer.send(FrameType.PONG, 0, frame.payload)
                }
                FrameType.PONG -> { /* no liveness state on server v0 */ }

                FrameType.TCP_OPEN -> handleTcpOpen(frame)
                FrameType.TCP_DATA -> handleTcpData(frame)
                FrameType.TCP_CLOSE_WR -> handleTcpCloseWr(frame)
                FrameType.TCP_RST -> handleTcpRst(frame)
                FrameType.TCP_WINDOW -> handleTcpWindow(frame)

                FrameType.UDP_BIND -> handleUdpBind(frame)
                FrameType.UDP_PACKET -> handleUdpPacket(frame)
                FrameType.UDP_CLOSE -> handleUdpClose(frame)

                FrameType.TCP_OPEN_ACK, FrameType.UDP_BIND_ACK -> {
                    Log.w(tag, "unexpected server-side ACK type=${FrameType.name(frame.type)}")
                    sendBye(REASON_PROTOCOL); return
                }
                else -> {
                    Log.w(tag, "unknown type=0x%02x".format(frame.type))
                    sendBye(REASON_PROTOCOL); return
                }
            }
        }
    }

    // ---- TCP handlers -----------------------------------------------------

    private suspend fun handleTcpOpen(f: RawFrame) {
        if (tcpStreams.containsKey(f.streamId) || udpStreams.containsKey(f.streamId)) {
            writer.send(FrameType.TCP_OPEN_ACK, f.streamId, u16BE(AckStatus.EOTHER))
            return
        }
        val (ep, _) = try { Endpoint.decode(f.payload, 0) } catch (t: Exception) {
            sendBye(REASON_PROTOCOL); return
        }
        val s = TcpStream(
            streamId = f.streamId,
            endpoint = ep,
            writer = writer,
            scope = scope,
            onClosed = { id -> tcpStreams.remove(id) },
            initialPeerWindowBytes = peerInitialWinBytes,
        )
        tcpStreams[f.streamId] = s
        // open() sends OPEN_ACK and spawns the read pump if connect succeeds
        scope.launch { s.open() }
    }

    private suspend fun handleTcpData(f: RawFrame) {
        val s = tcpStreams[f.streamId]
        if (s == null) {
            writer.send(FrameType.TCP_RST, f.streamId, u16BE(RstReason.OTHER))
            return
        }
        if (!s.onPeerData(f.payload)) {
            writer.send(FrameType.TCP_RST, f.streamId, u16BE(RstReason.PEER_RESET))
            s.reset()
        }
    }

    private suspend fun handleTcpCloseWr(f: RawFrame) {
        tcpStreams[f.streamId]?.onPeerCloseWr()
    }

    private fun handleTcpRst(f: RawFrame) {
        tcpStreams[f.streamId]?.reset()
    }

    private suspend fun handleTcpWindow(f: RawFrame) {
        if (f.payload.size < 4) return
        val credit = f.payload.readU32BE(0)
        tcpStreams[f.streamId]?.onPeerWindowUpdate(credit)
    }

    // ---- UDP handlers -----------------------------------------------------

    private suspend fun handleUdpBind(f: RawFrame) {
        if (tcpStreams.containsKey(f.streamId) || udpStreams.containsKey(f.streamId)) {
            writer.send(FrameType.UDP_BIND_ACK, f.streamId, u16BE(AckStatus.EOTHER))
            return
        }
        val u = UdpStream(
            streamId = f.streamId,
            writer = writer,
            scope = scope,
            onClosed = { id -> udpStreams.remove(id) },
        )
        udpStreams[f.streamId] = u
        scope.launch { u.open() }
    }

    private suspend fun handleUdpPacket(f: RawFrame) {
        val u = udpStreams[f.streamId] ?: return
        val (ep, dataOff) = try { Endpoint.decode(f.payload, 0) } catch (t: Exception) {
            sendBye(REASON_PROTOCOL); return
        }
        val datagram = f.payload.copyOfRange(dataOff, f.payload.size)
        u.onPeerPacket(ep, datagram)
    }

    private fun handleUdpClose(f: RawFrame) {
        udpStreams[f.streamId]?.close()
    }

    // ---- shutdown ---------------------------------------------------------

    private suspend fun sendBye(reason: Int) {
        try { writer.send(FrameType.BYE, 0, u16BE(reason)) } catch (_: Throwable) {}
    }

    private fun shutdown() {
        for (s in tcpStreams.values) s.reset()
        tcpStreams.clear()
        for (u in udpStreams.values) u.close()
        udpStreams.clear()
        writer.close()
        try { btSocket.close() } catch (_: Throwable) {}
        scope.cancel()
    }

    companion object {
        private const val MAX_STREAMS = 256
        private const val DEFAULT_INITIAL_WIN_KIB = 64
        private const val REASON_VERSION_MISMATCH = 0x0001
        private const val REASON_PROTOCOL = 0x0002

        /** Types that are legal to receive before HELLO completes. */
        private val EARLY_TYPES = setOf(FrameType.HELLO, FrameType.BYE, FrameType.PING, FrameType.PONG)
    }
}
