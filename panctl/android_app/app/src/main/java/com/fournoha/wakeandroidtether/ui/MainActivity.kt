// SPDX-License-Identifier: MIT
// Copyright (c) 2026 4noha
package com.fournoha.wakeandroidtether.ui

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothManager
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.widget.Button
import android.widget.TextView
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import com.fournoha.wakeandroidtether.R
import com.fournoha.wakeandroidtether.bt.Protocol
import com.fournoha.wakeandroidtether.bt.RfcommService
import com.fournoha.wakeandroidtether.prefs.Prefs

class MainActivity : AppCompatActivity() {

    private lateinit var prefs: Prefs

    private lateinit var btStatus: TextView
    private lateinit var serviceStatus: TextView
    private lateinit var uuidValue: TextView
    private lateinit var macValue: TextView

    private val requestBt = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { refresh() }

    private val requestNotif = registerForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { refresh() }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        prefs = Prefs(this)

        btStatus = findViewById(R.id.btStatus)
        serviceStatus = findViewById(R.id.serviceStatus)
        uuidValue = findViewById(R.id.uuidValue)
        macValue = findViewById(R.id.macValue)

        uuidValue.text = Protocol.UUID_SERVICE.toString()

        findViewById<Button>(R.id.btnRequestBt).setOnClickListener {
            val perms = mutableListOf(
                Manifest.permission.BLUETOOTH_CONNECT,
                Manifest.permission.BLUETOOTH_ADVERTISE,
            )
            if (Build.VERSION.SDK_INT >= 31) perms += Manifest.permission.BLUETOOTH_SCAN
            requestBt.launch(perms.toTypedArray())
        }
        findViewById<Button>(R.id.btnRequestNotif).setOnClickListener {
            if (Build.VERSION.SDK_INT >= 33) {
                requestNotif.launch(Manifest.permission.POST_NOTIFICATIONS)
            }
        }
        findViewById<Button>(R.id.btnStartService).setOnClickListener {
            prefs.listenerEnabled = true
            RfcommService.start(this); refresh()
        }
        findViewById<Button>(R.id.btnStopService).setOnClickListener {
            prefs.listenerEnabled = false
            RfcommService.stop(this); refresh()
        }
    }

    override fun onResume() {
        super.onResume()
        refresh()
    }

    @SuppressLint("MissingPermission")
    private fun refresh() {
        val adapter: BluetoothAdapter? =
            (getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager)?.adapter
        val btEnabled = adapter?.isEnabled == true
        val btPermOk = ContextCompat.checkSelfPermission(this, Manifest.permission.BLUETOOTH_CONNECT) ==
            PackageManager.PERMISSION_GRANTED
        btStatus.text = "BT: " + buildString {
            append(if (btEnabled) "enabled" else "disabled")
            append(", ")
            append(if (btPermOk) "permission granted" else "permission missing")
        }
        macValue.text = if (btPermOk && adapter != null) {
            try { adapter.address ?: "?" } catch (_: Throwable) { "?" }
        } else "(grant BT permission)"

        serviceStatus.text = "Service: " + if (prefs.listenerEnabled) "running" else "stopped"
    }
}
