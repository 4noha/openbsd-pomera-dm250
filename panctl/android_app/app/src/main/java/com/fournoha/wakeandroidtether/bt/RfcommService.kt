// SPDX-License-Identifier: MIT
// Copyright (c) 2026 4noha
package com.fournoha.wakeandroidtether.bt

import android.Manifest
import android.annotation.SuppressLint
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothServerSocket
import android.bluetooth.BluetoothSocket
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import android.util.Log
import androidx.core.app.NotificationCompat
import androidx.core.content.ContextCompat
import com.fournoha.wakeandroidtether.R
import com.fournoha.wakeandroidtether.mux.MuxServer
import com.fournoha.wakeandroidtether.ui.MainActivity
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.concurrent.thread

/**
 * Persistent foreground service that holds the BluetoothServerSocket open and accepts
 * one RFCOMM client at a time (pomera). Each accepted socket is wrapped in a [MuxServer]
 * which runs until the link drops. accept() is blocking native IO so it lives on its own
 * thread; mux work is on coroutines.
 */
class RfcommService : Service() {

    private val tag = "WAT/Rfcomm"

    private val running = AtomicBoolean(false)
    @Volatile private var serverSocket: BluetoothServerSocket? = null
    @Volatile private var acceptThread: Thread? = null

    private val supervisor: Job = SupervisorJob()
    private val scope: CoroutineScope = CoroutineScope(supervisor + Dispatchers.IO)

    override fun onCreate() {
        super.onCreate()
        ensureChannel()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_STOP -> {
                stopListener()
                stopSelf()
                return START_NOT_STICKY
            }
        }
        startForegroundCompat(getString(R.string.notification_text_listening))
        if (running.compareAndSet(false, true)) {
            startListener()
        }
        return START_STICKY
    }

    override fun onDestroy() {
        stopListener()
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    // ---- listener loop ----------------------------------------------------

    @SuppressLint("MissingPermission")
    private fun startListener() {
        if (!hasBtConnectPerm()) {
            Log.w(tag, "BLUETOOTH_CONNECT not granted; refusing to start")
            running.set(false)
            stopSelf()
            return
        }
        val adapter: BluetoothAdapter? =
            (getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager)?.adapter
        if (adapter == null || !adapter.isEnabled) {
            Log.w(tag, "BT adapter unavailable or disabled")
            running.set(false)
            stopSelf()
            return
        }

        val sock = try {
            adapter.listenUsingRfcommWithServiceRecord(Protocol.SDP_NAME, Protocol.UUID_SERVICE)
        } catch (t: Throwable) {
            Log.e(tag, "listenUsingRfcommWithServiceRecord failed", t)
            running.set(false)
            stopSelf()
            return
        }
        serverSocket = sock

        acceptThread = thread(name = "WAT-accept", isDaemon = true) {
            Log.i(tag, "accept() loop ready on uuid=${Protocol.UUID_SERVICE}")
            while (running.get()) {
                val client: BluetoothSocket = try {
                    sock.accept()
                } catch (t: Throwable) {
                    if (running.get()) Log.e(tag, "accept() failed", t)
                    break
                }
                handleClient(client)
            }
            try { sock.close() } catch (_: Throwable) {}
            Log.i(tag, "accept() loop exited")
            // accept() がエラーで抜けた場合 (典型: BT が disable された)、
            // service を畳んで BluetoothStateReceiver からの再起動に委ねる
            if (running.compareAndSet(true, false)) stopSelf()
        }
    }

    private fun stopListener() {
        if (!running.compareAndSet(true, false)) return
        try { serverSocket?.close() } catch (_: Throwable) {}
        serverSocket = null
        acceptThread?.interrupt()
        acceptThread = null
        scope.cancel()
    }

    @SuppressLint("MissingPermission")
    private fun handleClient(client: BluetoothSocket) {
        val peer = safePeerString(client)
        updateNotification(getString(R.string.notification_text_handling, peer))
        scope.launch {
            try {
                Log.i(tag, "mux start peer=$peer")
                MuxServer(client, parent = supervisor).run()
            } catch (t: Throwable) {
                Log.e(tag, "mux peer=$peer threw", t)
            } finally {
                try { client.close() } catch (_: Throwable) {}
                Log.i(tag, "mux end peer=$peer")
                updateNotification(getString(R.string.notification_text_listening))
            }
        }
    }

    @SuppressLint("MissingPermission")
    private fun safePeerString(client: BluetoothSocket): String {
        if (!hasBtConnectPerm()) return "?"
        return try { client.remoteDevice?.address ?: "?" } catch (_: Throwable) { "?" }
    }

    private fun hasBtConnectPerm(): Boolean =
        ContextCompat.checkSelfPermission(this, Manifest.permission.BLUETOOTH_CONNECT) ==
            PackageManager.PERMISSION_GRANTED

    // ---- foreground notification ------------------------------------------

    private fun ensureChannel() {
        val nm = getSystemService(NotificationManager::class.java) ?: return
        if (nm.getNotificationChannel(CHANNEL_ID) != null) return
        val ch = NotificationChannel(CHANNEL_ID, getString(R.string.channel_name),
            NotificationManager.IMPORTANCE_LOW).apply {
            description = getString(R.string.channel_description)
            setShowBadge(false)
        }
        nm.createNotificationChannel(ch)
    }

    private fun buildNotification(text: String): Notification {
        val openIntent = PendingIntent.getActivity(this, 0,
            Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)

        val stopIntent = PendingIntent.getService(this, 1,
            Intent(this, RfcommService::class.java).setAction(ACTION_STOP),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)

        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setSmallIcon(android.R.drawable.stat_sys_data_bluetooth)
            .setContentTitle(getString(R.string.notification_title))
            .setContentText(text)
            .setContentIntent(openIntent)
            .setOngoing(true)
            .addAction(android.R.drawable.ic_media_pause, "Stop", stopIntent)
            .build()
    }

    private fun startForegroundCompat(text: String) {
        val n = buildNotification(text)
        if (Build.VERSION.SDK_INT >= 34) {
            startForeground(NOTIF_ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE)
        } else {
            startForeground(NOTIF_ID, n)
        }
    }

    private fun updateNotification(text: String) {
        val nm = getSystemService(NotificationManager::class.java) ?: return
        nm.notify(NOTIF_ID, buildNotification(text))
    }

    companion object {
        const val ACTION_STOP = "com.fournoha.wakeandroidtether.STOP"
        private const val CHANNEL_ID = "rfcomm"
        private const val NOTIF_ID = 1001

        fun start(context: Context) {
            val i = Intent(context, RfcommService::class.java)
            ContextCompat.startForegroundService(context, i)
        }

        fun stop(context: Context) {
            val i = Intent(context, RfcommService::class.java).setAction(ACTION_STOP)
            context.startService(i)
        }
    }
}
