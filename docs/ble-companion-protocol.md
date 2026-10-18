# Phone companion protocol (v1)

The BLE phone companion is a session-scoped feature of this firmware: the
reader advertises while the user is in **Settings → Network → Phone
Companion**, a bonded Android phone connects, pushes a short title/body that
is painted on e-ink, and receives physical button events. Everything else
(books, annotations, Wi-Fi, notifications) is out of scope for v1.

The Android client lives in its own repository:
https://github.com/kivarun/crosspoint-pal-android

## Service

| Purpose | UUID | Direction | Properties |
| --- | --- | --- | --- |
| Service | `7b1f1001-6f70-4f69-8f31-63726f737370` | — | — |
| Control | `7b1f1002-6f70-4f69-8f31-63726f737370` | phone → reader | Write, encrypted |
| Event | `7b1f1003-6f70-4f69-8f31-63726f737370` | reader → phone | Notify, encrypted |

Control and event values are complete UTF-8 JSON objects of at most 512
bytes. The reader is the GATT peripheral and displays the six-digit passkey
used to create an encrypted bond (bond + MITM + SC, DISPLAY_ONLY IO cap).
The phone requests a 517-byte MTU before service discovery.

## Messages

```text
phone → reader:
  {"op":"hello","version":1}
  {"op":"content","title":"…","body":"…","contentId":"id.1"}

reader → phone:
  {"op":"hello_ack","version":1}
  {"op":"content_ack","contentId":"id.1"}
  {"op":"button","button":"back","pressed":true}
  {"op":"error","code":"…","message":"…"}
```

- `hello` is the ready barrier: every other message before it is answered
  with `error{code:"hello_first"}`. An unknown version is answered with
  `error{code:"version"}`.
- Limits (bytes, enforced by the reader; the phone truncates at UTF-8
  code-point boundaries): title 64, body 384, contentId 32. `contentId` may
  contain `A-Z a-z 0 9 . _ - :` only, so it can be echoed without escaping.
- Repeating a `contentId` with an identical title/body is idempotent: the
  reader replies `content_ack` but does not repaint.
- Button names: `back`, `confirm`, `left`, `right`, `up`, `down`. Power is
  never forwarded; it leaves the session.
- Malformed or oversized messages are answered with
  `error{code:"bad_message"}` and the session stays alive.
- If the reader's internal event queue overflows, it drops the link without
  an error notification; reconnect and send `hello` again.
- Reader→phone notifications are at most MTU−3 bytes; every message above
  fits comfortably once MTU 517 is negotiated.

## Security and identity

- The bond is encrypted with a six-digit passkey shown on the reader.
- One phone at a time: the reader accepts only the persisted companion
  identity, adopts the first bonded peer, and disconnects any other bonded
  peer. The identity is the **bonded ID address plus its address type** —
  never the over-the-air address, which changes when the phone uses BLE
  privacy (RPA).
- Resetting pairing must be done on BOTH sides: forget the reader in
  Android Bluetooth settings, and on the reader hold **Down or Confirm for
  1.5 s** inside the companion screen. The reader deletes only its own
  companion bond; page-turner (HID) bonds are never touched.

## Hardware UAT checklist (X4 Classic)

1. Flash the `x4c` build; open Settings → Network → Phone Companion.
2. Record the `BLECOMP` heap log lines: "after BLE init", "after connect"
   and "after teardown" (the before-init figures travel inside the teardown
   line, in parentheses).
3. Pair the phone (passkey dialog), send title/body, verify the e-ink paint
   and `content_ack` on the phone.
4. Press the reader's side keys; verify `button` events on the phone.
5. Leave the session with Back; verify the reader works normally and the
   Bluetooth page turner still starts afterwards.
6. Reset pairing on both sides; verify a second phone is rejected while the
   first identity is stored.
7. With no phone ever paired, hold the reset (Down or Confirm, 1.5 s) and
   verify the log reports "No companion bond configured; nothing to delete"
   and nothing changes.
