// SPDX-License-Identifier: MIT
// Copyright (c) 2026 4noha

package com.fournoha.wakeandroidtether.mux

import com.fournoha.wakeandroidtether.bt.Protocol
import java.io.DataInputStream
import java.io.EOFException
import java.io.IOException

/**
 * Frame types from PROTOCOL.md §3.
 */
object FrameType {
    const val HELLO: Int          = 0x01
    const val BYE: Int            = 0x02
    const val PING: Int           = 0x03
    const val PONG: Int           = 0x04
    const val TCP_OPEN: Int       = 0x10
    const val TCP_OPEN_ACK: Int   = 0x11
    const val TCP_DATA: Int       = 0x12
    const val TCP_CLOSE_WR: Int   = 0x13
    const val TCP_RST: Int        = 0x14
    const val TCP_WINDOW: Int     = 0x15
    const val UDP_BIND: Int       = 0x20
    const val UDP_BIND_ACK: Int   = 0x21
    const val UDP_PACKET: Int     = 0x22
    const val UDP_CLOSE: Int      = 0x23

    fun name(t: Int): String = when (t) {
        HELLO -> "HELLO"
        BYE -> "BYE"
        PING -> "PING"
        PONG -> "PONG"
        TCP_OPEN -> "TCP_OPEN"
        TCP_OPEN_ACK -> "TCP_OPEN_ACK"
        TCP_DATA -> "TCP_DATA"
        TCP_CLOSE_WR -> "TCP_CLOSE_WR"
        TCP_RST -> "TCP_RST"
        TCP_WINDOW -> "TCP_WINDOW"
        UDP_BIND -> "UDP_BIND"
        UDP_BIND_ACK -> "UDP_BIND_ACK"
        UDP_PACKET -> "UDP_PACKET"
        UDP_CLOSE -> "UDP_CLOSE"
        else -> "type=0x%02x".format(t)
    }
}

/** Address types from PROTOCOL.md §3.4. v0 only emits IPv4. */
object AddrType {
    const val IPV4: Int = 0x01
    const val IPV6: Int = 0x02
    const val DOMAIN: Int = 0x03
}

/** Status codes for *_ACK frames (PROTOCOL.md §3.5). */
object AckStatus {
    const val OK: Int                = 0x0000
    const val ECONNREFUSED: Int      = 0x0001
    const val EHOSTUNREACH: Int      = 0x0002
    const val ETIMEDOUT: Int         = 0x0003
    const val EAFNOSUPPORT: Int      = 0x0004
    const val EOTHER: Int            = 0x00FF
}

/** Reason codes for TCP_RST (PROTOCOL.md §3.8). */
object RstReason {
    const val PEER_RESET: Int        = 0x0001
    const val IDLE_TIMEOUT: Int      = 0x0002
    const val FLOW_VIOLATION: Int    = 0x0003
    const val OTHER: Int             = 0x00FF
}

/** Raw frame as read off the wire. Payload interpretation is type-dependent. */
data class RawFrame(
    val version: Int,
    val type: Int,
    val streamId: Int,
    val payload: ByteArray,
) {
    override fun equals(other: Any?): Boolean = this === other
    override fun hashCode(): Int = System.identityHashCode(this)
}

object FrameCodec {
    const val HEADER_SIZE: Int = 6
    /** Hard limit for a single frame payload. Practical limit is much smaller. */
    const val MAX_PAYLOAD: Int = 65535

    /**
     * Read one full frame off the stream. Returns null on clean EOF.
     * Throws IOException on a protocol violation (bad header) or transport error.
     */
    @Throws(IOException::class)
    fun read(input: DataInputStream): RawFrame? {
        val ver = try {
            input.readUnsignedByte()
        } catch (_: EOFException) {
            return null
        }
        val type = input.readUnsignedByte()
        val streamId = input.readUnsignedShort()
        val length = input.readUnsignedShort()
        val payload = ByteArray(length)
        if (length > 0) input.readFully(payload)
        return RawFrame(ver, type, streamId, payload)
    }

    /**
     * Serialize a frame into the provided buffer-friendly form. Header is
     * always written; payload is appended without copy.
     */
    fun header(type: Int, streamId: Int, length: Int): ByteArray {
        require(streamId in 0..0xFFFF) { "streamId=$streamId out of range" }
        require(length in 0..MAX_PAYLOAD) { "length=$length out of range" }
        val h = ByteArray(HEADER_SIZE)
        h[0] = Protocol.PROTO_VERSION.toByte()
        h[1] = (type and 0xFF).toByte()
        h[2] = ((streamId ushr 8) and 0xFF).toByte()
        h[3] = (streamId and 0xFF).toByte()
        h[4] = ((length ushr 8) and 0xFF).toByte()
        h[5] = (length and 0xFF).toByte()
        return h
    }
}

/** Parsed payload for HELLO (PROTOCOL.md §3.1). */
data class Hello(
    val protoVer: Int,
    val flags: Int,
    val maxStreams: Int,
    val initialWinKib: Int,
) {
    fun encode(): ByteArray = ByteArray(6).also {
        it[0] = (protoVer and 0xFF).toByte()
        it[1] = (flags and 0xFF).toByte()
        it[2] = ((maxStreams ushr 8) and 0xFF).toByte()
        it[3] = (maxStreams and 0xFF).toByte()
        it[4] = ((initialWinKib ushr 8) and 0xFF).toByte()
        it[5] = (initialWinKib and 0xFF).toByte()
    }

    companion object {
        @Throws(IOException::class)
        fun decode(payload: ByteArray): Hello {
            if (payload.size != 6) throw IOException("HELLO payload size=${payload.size}, want 6")
            return Hello(
                protoVer = payload[0].toInt() and 0xFF,
                flags = payload[1].toInt() and 0xFF,
                maxStreams = ((payload[2].toInt() and 0xFF) shl 8) or (payload[3].toInt() and 0xFF),
                initialWinKib = ((payload[4].toInt() and 0xFF) shl 8) or (payload[5].toInt() and 0xFF),
            )
        }
    }
}

/** TCP_OPEN and UDP_PACKET share the addr/port encoding. */
data class Endpoint(
    val addrType: Int,
    val port: Int,
    val addr: ByteArray,
) {
    init {
        require(port in 1..0xFFFF) { "port=$port out of range" }
        when (addrType) {
            AddrType.IPV4 -> require(addr.size == 4) { "IPv4 addr len=${addr.size}" }
            AddrType.IPV6 -> require(addr.size == 16) { "IPv6 addr len=${addr.size}" }
            AddrType.DOMAIN -> require(addr.isNotEmpty()) { "DOMAIN addr empty" }
            else -> error("unknown addr_type=$addrType")
        }
    }

    fun encodedSize(): Int = 1 + 2 + 1 + addr.size

    /** Encodes [addr_type, dst_port_be, addr_len, addr]. */
    fun encodeInto(dst: ByteArray, offset: Int): Int {
        var p = offset
        dst[p++] = (addrType and 0xFF).toByte()
        dst[p++] = ((port ushr 8) and 0xFF).toByte()
        dst[p++] = (port and 0xFF).toByte()
        dst[p++] = (addr.size and 0xFF).toByte()
        System.arraycopy(addr, 0, dst, p, addr.size)
        return p + addr.size
    }

    override fun equals(other: Any?): Boolean = this === other
    override fun hashCode(): Int = System.identityHashCode(this)

    companion object {
        @Throws(IOException::class)
        fun decode(payload: ByteArray, offset: Int): Pair<Endpoint, Int> {
            if (offset + 4 > payload.size) throw IOException("endpoint truncated")
            val addrType = payload[offset].toInt() and 0xFF
            val port = ((payload[offset + 1].toInt() and 0xFF) shl 8) or
                (payload[offset + 2].toInt() and 0xFF)
            val addrLen = payload[offset + 3].toInt() and 0xFF
            val end = offset + 4 + addrLen
            if (end > payload.size) throw IOException("addr bytes truncated")
            val addr = payload.copyOfRange(offset + 4, end)
            return Endpoint(addrType, port, addr) to end
        }
    }
}

/** Helpers for fixed-size scalar payloads. */
internal fun ByteArray.readU16BE(off: Int): Int =
    ((this[off].toInt() and 0xFF) shl 8) or (this[off + 1].toInt() and 0xFF)

internal fun ByteArray.readU32BE(off: Int): Long =
    ((this[off].toLong() and 0xFF) shl 24) or
        ((this[off + 1].toLong() and 0xFF) shl 16) or
        ((this[off + 2].toLong() and 0xFF) shl 8) or
        (this[off + 3].toLong() and 0xFF)

internal fun u16BE(v: Int): ByteArray = byteArrayOf(
    ((v ushr 8) and 0xFF).toByte(),
    (v and 0xFF).toByte(),
)

internal fun u32BE(v: Long): ByteArray = byteArrayOf(
    ((v ushr 24) and 0xFF).toByte(),
    ((v ushr 16) and 0xFF).toByte(),
    ((v ushr 8) and 0xFF).toByte(),
    (v and 0xFF).toByte(),
)
