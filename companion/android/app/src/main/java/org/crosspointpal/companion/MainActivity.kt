package org.crosspointpal.companion

import android.Manifest
import android.bluetooth.BluetoothDevice
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import androidx.lifecycle.lifecycleScope
import org.crosspointpal.companion.ble.CompanionClient
import java.util.UUID

class MainActivity : AppCompatActivity() {
    private var client: CompanionClient? = null
    private lateinit var status: TextView
    private lateinit var events: TextView
    private lateinit var titleField: EditText
    private lateinit var bodyField: EditText

    private val bondReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context?, intent: Intent?) {
            if (intent?.action != BluetoothDevice.ACTION_BOND_STATE_CHANGED) return
            val device = if (Build.VERSION.SDK_INT >= 33) {
                intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE, BluetoothDevice::class.java)
            } else {
                @Suppress("DEPRECATION")
                intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE)
            } ?: return
            val state = intent.getIntExtra(BluetoothDevice.EXTRA_BOND_STATE, BluetoothDevice.ERROR)
            if (state == BluetoothDevice.BOND_BONDED) {
                status("Paired; connecting")
                client?.connectBonded(device)
            } else if (state == BluetoothDevice.BOND_NONE) {
                status("Pairing cancelled")
                client?.startScan()
            }
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val pad = (16 * resources.displayMetrics.density).toInt()
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(pad, pad, pad, pad)
        }

        status = TextView(this)
        root.addView(status)

        val scan = Button(this).apply { text = "Scan for reader" }
        root.addView(scan)

        titleField = EditText(this).apply { hint = "Title" }
        root.addView(titleField)
        bodyField = EditText(this).apply { hint = "Body" }
        root.addView(bodyField)

        val send = Button(this).apply { text = "Send content" }
        root.addView(send)

        events = TextView(this).apply { text = "Events appear here" }
        root.addView(events)

        val disconnect = Button(this).apply { text = "Disconnect" }
        root.addView(disconnect)

        val forget = Button(this).apply { text = "Reset pairing (both sides)" }
        root.addView(forget)

        setContentView(root)

        scan.setOnClickListener { ensurePermissions(); client?.startScan() }
        send.setOnClickListener {
            val ok = client?.sendContent(
                titleField.text.toString(),
                bodyField.text.toString(),
                contentId = "v0-" + (titleField.text.toString().take(8).ifEmpty { "msg" }),
            ) ?: false
            if (!ok) status("Connect first")
        }
        disconnect.setOnClickListener { client?.disconnect() }
        forget.setOnClickListener { client?.forgetBond() }

        ContextCompat.registerReceiver(
            this,
            bondReceiver,
            IntentFilter(BluetoothDevice.ACTION_BOND_STATE_CHANGED),
            ContextCompat.RECEIVER_NOT_EXPORTED,
        )
    }

    override fun onDestroy() {
        runCatching { unregisterReceiver(bondReceiver) }
        client?.disconnect()
        client = null
        super.onDestroy()
    }

    private fun ensurePermissions() {
        if (client == null) {
            client = CompanionClient(
                this,
                lifecycleScope,
                onStatus = { runOnUiThread { status(it) } },
                onEvent = { runOnUiThread { event(it) } },
            )
        }
        val needed = buildList {
            if (Build.VERSION.SDK_INT >= 31) {
                add(Manifest.permission.BLUETOOTH_SCAN)
                add(Manifest.permission.BLUETOOTH_CONNECT)
            } else {
                add(Manifest.permission.ACCESS_FINE_LOCATION)
            }
        }.filter {
            ActivityCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED
        }
        if (needed.isNotEmpty()) {
            ActivityCompat.requestPermissions(this, needed.toTypedArray(), 1)
            status("Grant Bluetooth permissions")
        }
    }

    private fun status(text: String) {
        status.text = text
    }

    private fun event(text: String) {
        events.text = text + "\n" + events.text
    }
}
