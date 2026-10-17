package org.crosspointpal.companion.ble

import java.util.UUID

// Mirrors lib/BleCompanion/include/BleCompanionProtocol.h (protocol v1).
object BleProtocol {
    const val VERSION = 1
    val SERVICE: UUID = UUID.fromString("7b1f1001-6f70-4f69-8f31-63726f737370")
    val CONTROL: UUID = UUID.fromString("7b1f1002-6f70-4f69-8f31-63726f737370")
    val EVENT: UUID = UUID.fromString("7b1f1003-6f70-4f69-8f31-63726f737370")
    val CLIENT_CONFIG: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")

    const val MAX_TITLE_BYTES = 64
    const val MAX_BODY_BYTES = 384
    const val MAX_CONTENT_ID_BYTES = 32
}
