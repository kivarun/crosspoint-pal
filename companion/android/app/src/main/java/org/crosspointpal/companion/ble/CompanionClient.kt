package org.crosspointpal.companion.ble

import android.annotation.SuppressLint
import android.bluetooth.*
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.os.ParcelUuid
import androidx.core.content.ContextCompat
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.util.ArrayDeque
import java.util.UUID
import kotlin.coroutines.resume

// Activity-scoped BLE client for the v0 companion session. No foreground
// service, no background scan, no persistence beyond the system bond: the
// connection lives while the app is open.
class CompanionClient(
    private val context: Context,
    private val scope: CoroutineScope,
    private val onStatus: (String) -> Unit,
    private val onEvent: (String) -> Unit,
) {
    private val adapter by lazy {
        (context.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager).adapter
    }
    private var scanner: BluetoothLeScanner? = null
    private var gatt: BluetoothGatt? = null
    private var control: BluetoothGattCharacteristic? = null
    private var event: BluetoothGattCharacteristic? = null
    private val writeQueue = ArrayDeque<ByteArray>()
    private var writeInFlight = false
    private var sessionId = 0
    private var helloSent = false

    val isConnected: Boolean get() = gatt != null && helloSent

    private fun has(permission: String) =
        ContextCompat.checkSelfPermission(context, permission) == android.content.pm.PackageManager.PERMISSION_GRANTED

    @SuppressLint("MissingPermission")
    fun startScan() {
        if (!has(android.Manifest.permission.BLUETOOTH_SCAN)) {
            onStatus("Bluetooth scan permission required")
            return
        }
        if (gatt != null || scanner != null) return
        val leScanner = adapter.bluetoothLeScanner ?: run {
            onStatus("Bluetooth is unavailable")
            return
        }
        scanner = leScanner
        val filter = ScanFilter.Builder().setServiceUuid(ParcelUuid(BleProtocol.SERVICE)).build()
        leScanner.startScan(
            listOf(filter),
            ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build(),
            scanCallback,
        )
        onStatus("Scanning for CrossPoint Pal")
    }

    @SuppressLint("MissingPermission")
    fun stopScan() {
        if (has(android.Manifest.permission.BLUETOOTH_SCAN)) {
            scanner?.stopScan(scanCallback)
        }
        scanner = null
    }

    private val scanCallback = object : ScanCallback() {
        @SuppressLint("MissingPermission")
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            if (!has(android.Manifest.permission.BLUETOOTH_CONNECT)) return
            stopScan()
            val device = result.device
            if (device.bondState == BluetoothDevice.BOND_BONDED) {
                connect(device)
            } else {
                onStatus("Confirm the pairing code on the phone")
                if (!device.createBond()) {
                    onStatus("Bluetooth pairing failed to start")
                    startScan()
                }
            }
        }

        override fun onScanFailed(errorCode: Int) {
            scanner = null
            onStatus("Bluetooth scan failed ($errorCode)")
        }
    }

    // Called by the activity when the system reports the bond completed.
    @SuppressLint("MissingPermission")
    fun connectBonded(device: BluetoothDevice) {
        if (device.bondState == BluetoothDevice.BOND_BONDED) connect(device)
    }

    @SuppressLint("MissingPermission")
    private fun connect(device: BluetoothDevice) {
        if (gatt != null) return
        onStatus("Connecting")
        gatt = device.connectGatt(context, false, callback, BluetoothDevice.TRANSPORT_LE)
    }

    private val callback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            val id = sessionId
            if (status == BluetoothGatt.GATT_SUCCESS && newState == BluetoothProfile.STATE_CONNECTED) {
                if (g !== gatt) {
                    g.close()
                    return
                }
                onStatus("Negotiating MTU")
                if (!g.requestMtu(517)) g.discoverServices()
            } else {
                resetProtocolState()
                if (g !== gatt) {
                    g.close()
                    return
                }
                g.close()
                gatt = null
                onStatus("Disconnected")
            }
            if (id != sessionId) return
        }

        override fun onMtuChanged(g: BluetoothGatt, mtu: Int, status: Int) {
            onStatus("MTU $mtu; discovering services")
            g.discoverServices()
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            if (status != BluetoothGatt.GATT_SUCCESS) {
                onStatus("Service discovery failed ($status)")
                return
            }
            val service = g.getService(BleProtocol.SERVICE)
            if (service == null) {
                onStatus("Companion service not found")
                return
            }
            control = service.getCharacteristic(BleProtocol.CONTROL)
            event = service.getCharacteristic(BleProtocol.EVENT)
            val eventCharacteristic = event ?: run {
                onStatus("Companion characteristics not found")
                return
            }
            g.setCharacteristicNotification(eventCharacteristic, true)
            val descriptor = eventCharacteristic.getDescriptor(BleProtocol.CLIENT_CONFIG)
            if (descriptor == null) {
                sendHello()
            } else {
                val started = if (android.os.Build.VERSION.SDK_INT >= 33) {
                    g.writeDescriptor(descriptor, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE) ==
                        BluetoothStatusCodes.SUCCESS
                } else {
                    @Suppress("DEPRECATION")
                    descriptor.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                    @Suppress("DEPRECATION")
                    g.writeDescriptor(descriptor)
                }
                if (!started) sendHello()
            }
        }

        override fun onDescriptorWrite(gatt: BluetoothGatt, descriptor: BluetoothGattDescriptor, status: Int) {
            if (descriptor.uuid == BleProtocol.CLIENT_CONFIG) sendHello()
        }

        override fun onCharacteristicWrite(
            g: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            status: Int,
        ) {
            writeInFlight = false
            if (status != BluetoothGatt.GATT_SUCCESS) {
                onStatus("Write failed ($status)")
            }
            drainWriteQueue()
        }

        override fun onCharacteristicChanged(
            g: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            value: ByteArray,
        ) {
            handleEvent(value)
        }

        @Deprecated("Deprecated by Android")
        @Suppress("DEPRECATION")
        override fun onCharacteristicChanged(gatt: BluetoothGatt, characteristic: BluetoothGattCharacteristic) {
            handleEvent(characteristic.value ?: ByteArray(0))
        }
    }

    private fun sendHello() {
        helloSent = true
        onStatus("Connected; sending hello")
        write(JSONObject().put("op", "hello").put("version", BleProtocol.VERSION))
    }

    fun sendContent(title: String, body: String, contentId: String): Boolean {
        if (!isConnected) {
            onStatus("Not connected")
            return false
        }
        // Truncate at UTF-8 code-point boundaries; the reader rejects anything
        // over the limits rather than truncating.
        val t = utf8Limited(title, BleProtocol.MAX_TITLE_BYTES)
        val b = utf8Limited(body, BleProtocol.MAX_BODY_BYTES)
        val id = contentId.take(BleProtocol.MAX_CONTENT_ID_BYTES).filter { it.isLetterOrDigit() || it in "._-:" }
        if (id.isEmpty()) {
            onStatus("Content id must be 1..32 letters/digits/._-:")
            return false
        }
        write(
            JSONObject()
                .put("op", "content")
                .put("title", t.toString("UTF-8"))
                .put("body", b.toString("UTF-8"))
                .put("contentId", id)
        )
        return true
    }

    @SuppressLint("MissingPermission")
    fun disconnect() {
        sessionId++
        resetProtocolState()
        gatt?.disconnect()
        gatt?.close()
        gatt = null
        onStatus("Disconnected")
    }

    // Best-effort bond removal (hidden API via reflection): the reader side
    // must also run its pairing reset.
    @SuppressLint("MissingPermission")
    fun forgetBond() {
        val device = gatt?.device ?: adapter.bondedDevices.firstOrNull {
            try {
                it.uuids?.any { p -> p.uuid == BleProtocol.SERVICE } == true
            } catch (_: SecurityException) {
                false
            }
        }
        disconnect()
        if (device == null) {
            onStatus("No bond found; forget CrossPoint Pal in Android settings")
            return
        }
        try {
            val method = device.javaClass.getMethod("removeBond")
            val removed = method.invoke(device) as? Boolean ?: false
            onStatus(if (removed) "Bond removed; also run pairing reset on the reader" else "removeBond refused; forget CrossPoint Pal in Android settings")
        } catch (e: Exception) {
            onStatus("Forget CrossPoint Pal in Android settings (${e.javaClass.simpleName})")
        }
    }

    private fun resetProtocolState() {
        control = null
        event = null
        writeQueue.clear()
        writeInFlight = false
        helloSent = false
    }

    private fun write(message: JSONObject) {
        val bytes = message.toString().toByteArray(Charsets.UTF_8)
        if (bytes.size > 512) {
            onStatus("Message too large")
            return
        }
        writeQueue.addLast(bytes)
        drainWriteQueue()
    }

    @SuppressLint("MissingPermission")
    private fun drainWriteQueue() {
        val g = gatt ?: return
        val c = control ?: return
        if (writeInFlight) return
        val bytes = writeQueue.pollFirst() ?: return
        writeInFlight = true
        if (android.os.Build.VERSION.SDK_INT >= 33) {
            if (!g.writeCharacteristic(c, bytes, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT)) {
                writeInFlight = false
                onStatus("Write rejected")
            }
        } else {
            @Suppress("DEPRECATION")
            c.value = bytes
            @Suppress("DEPRECATION")
            if (!g.writeCharacteristic(c)) {
                writeInFlight = false
                onStatus("Write rejected")
            }
        }
    }

    private fun handleEvent(bytes: ByteArray) {
        val message = runCatching { JSONObject(bytes.toString(Charsets.UTF_8)) }.getOrNull() ?: run {
            onEvent("Unparseable event")
            return
        }
        when (message.optString("op")) {
            "hello_ack" -> onEvent("hello_ack (v${message.optInt("version")})")
            "content_ack" -> onEvent("content_ack: ${message.optString("contentId")}")
            "button" -> onEvent("button ${message.optString("button")} ${if (message.optBoolean("pressed")) "down" else "up"}")
            "error" -> onEvent("error: ${message.optString("code")} ${message.optString("message")}")
        }
    }

    private fun utf8Limited(value: String, maxBytes: Int): ByteArray {
        val bytes = value.toByteArray(Charsets.UTF_8)
        if (bytes.size <= maxBytes) return bytes
        var end = maxBytes
        while (end > 0 && bytes[end].toInt() and 0xc0 == 0x80) end--
        return bytes.copyOf(end)
    }
}
