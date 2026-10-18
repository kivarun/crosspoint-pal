#pragma once

// Bluetooth phone companion: a bonded Android phone pushes a short text to
// the reader and receives physical button events over an encrypted BLE link.
//
// The firmware starts the companion explicitly from its settings; the module
// owns the NimBLE radio while the session is open and hands it back fully
// deinitialized on stop. Nothing here runs at boot and nothing persists
// session state: the bond is NimBLE's NVS store, the chosen-phone address is
// the firmware's settings, and everything else is dropped on disconnect.
//
// Capability-gated like the SDK's BleKeyboardHost (FREEINK_CAP_BLE_COMPANION,
// checked in the .cpp): when the cap is off every method links a stub body,
// no BLE code is pulled in, and callers need no #ifdefs. This header is
// deliberately NimBLE-free so host builds can include it freely.

#include <cstddef>
#include <cstdint>

#include "BleCompanionProtocol.h"

namespace blecompanion {

enum class RadioState : uint8_t { Stopped, Advertising, AwaitingAuthentication, Connected };

struct Status {
  bool compiledIn = false;
  RadioState state = RadioState::Stopped;
  bool sessionActive = false;   // hello accepted
  bool contentPresent = false;  // the content slot holds something
  uint32_t passkey = 0;         // six digits, shown on screen while pairing
  uint32_t revision = 0;        // bumps on every state change the UI shows
};

// The content slot, read for rendering. Pointers stay valid until the next
// loop() that processed messages.
struct ContentView {
  const char* title = nullptr;
  const char* body = nullptr;
};

// True when this build compiled the companion in (Settings shows the row only
// then). False in stub builds.
bool compiledIn();

class BleCompanion {
 public:
  ~BleCompanion();
  // Starts NimBLE as a GATT peripheral and advertises. `storedPeerAddr` is
  // the persisted companion phone identity text ("aa:bb:cc:dd:ee:ff", "" when
  // none) and `storedPeerAddrType` its NimBLE address type (0 public, 1
  // random). Both callbacks run on the main loop: `savePeerAddr` stores a
  // first-time peer's identity, `forgetPeerAddr` clears it.
  bool begin(const char* storedPeerAddr, uint8_t storedPeerAddrType,
             void (*savePeerAddr)(const char* addr, uint8_t type), void (*forgetPeerAddr)());
  void loop();
  // Fully tears the stack down (NimBLEDevice::deinit); RAM returns to the heap.
  void stop();
  // Deletes ONLY the configured companion bond and clears the persisted
  // identity (via `forgetPeerAddr`). False when no companion identity is
  // configured or the bond could not be removed; no other bond is ever
  // touched, and the whole store is never cleared.
  bool forgetBond();
  // One physical button edge to the phone. `button` is a Button enum value.
  bool sendButtonEvent(uint8_t button, bool pressed);

  Status status() const;
  ContentView content() const;

  // The NimBLE adapter lives behind this pointer; the type is public so the
  // adapter's callback classes can name it. The pointer itself is private.
  struct Impl;

 private:
  Impl* impl_ = nullptr;
};

}  // namespace blecompanion
