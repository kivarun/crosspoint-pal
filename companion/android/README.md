# CrossPoint Pal Companion for Android (v0)

Minimal manual companion client for the CrossPoint Pal BLE companion session
(firmware `feature/ble-companion-v0`, protocol v1). It scans for the reader,
pairs through the six-digit code the reader shows on its e-ink screen, sends a
short title/body, and displays the physical button events the reader forwards.

## v0 scope

- Scan by companion service UUID; pair (system passkey dialog); connect.
- MTU 517 request, service discovery, Event-characteristic subscription.
- `hello` → `hello_ack`; `content {title, body, contentId}` → `content_ack`.
- Reader `button {button, pressed}` events listed in the log.
- Disconnect, and a best-effort "reset pairing" (`removeBond`) — the reader
  side must also run its pairing reset (hold Down/Confirm in the companion
  screen).

Deliberately out of scope until V1/V2: notification listener, foreground
service, background/autoConnect reconnect, PendingIntent scan, Wi-Fi, Room.

## Build

Android Studio (JDK 17, SDK 36) or command line:

```
cd companion/android
./gradlew :app:assembleDebug   # wrapper not committed yet; use your local Gradle 8.13+ / AS
```

The Gradle wrapper jar is intentionally not committed in v0 (binary file);
Android Studio generates it on open, or run `gradle wrapper --gradle-version
8.13` once with a local Gradle. Toolchain pins follow the proven Terramoto
companion app: AGP 8.13.0, Kotlin 2.2.10, compileSdk 36, minSdk 26.

## Flow

1. Start the companion session on the reader (Settings → Phone Companion).
   The reader shows a six-digit pairing code while it advertises.
2. Tap **Scan for reader**; grant the Bluetooth permissions.
3. Confirm the passkey in the system pairing dialog.
4. Type a title and body, tap **Send content**; the reader paints it.
5. Press the reader's side keys; events appear in the log.

## Notes

- One phone at a time: the reader rejects a second phone; reset pairing on
  both sides to switch phones.
- The bond is stored by Android and the reader's NimBLE NVS; it survives
  restarts. The session state (hello/content) does not.
