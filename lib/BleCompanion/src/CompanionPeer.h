#pragma once

// Companion peer identity: the bonded peer's ID address plus its address
// type. This is the stable identity the bond store keys on — an Android
// phone using BLE privacy changes its over-the-air (RPA) address between
// connections, but its identity address stays fixed, so the one-phone
// policy must compare identities, never OTA addresses.
//
// Pure and host-testable: the NimBLE adapter converts NimBLEAddress <->
// PeerId; the session policy and the bond-target seam operate on PeerId.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace blecompanion {

// NimBLE address types (BLE_ADDR_PUBLIC / BLE_ADDR_RANDOM).
constexpr uint8_t ADDR_TYPE_PUBLIC = 0;
constexpr uint8_t ADDR_TYPE_RANDOM = 1;
constexpr uint8_t ADDR_TYPE_UNSET = 0xFF;

struct PeerId {
  // Six bytes in display (MSB-first) order — the byte order a phone shows
  // and the persisted text carries. The NimBLE adapter translates to and
  // from NimBLE's LSB-first ble_addr_t at its boundary.
  uint8_t addr[6] = {};
  uint8_t type = ADDR_TYPE_UNSET;

  bool operator==(const PeerId& other) const {
    return type == other.type && memcmp(addr, other.addr, sizeof(addr)) == 0;
  }
  bool operator!=(const PeerId& other) const { return !(*this == other); }
  bool unset() const { return type == ADDR_TYPE_UNSET; }
  bool set() const { return !unset(); }
};

// Canonical persisted form: "aa:bb:cc:dd:ee:ff" lowercase hex, 17 chars.
// The type travels as its own settings byte, not inside the string.
constexpr size_t PEER_ID_TEXT_LEN = 17;  // without the terminator

// Writes the canonical text plus terminator; false when cap is too small or
// the peer is unset.
bool peerIdToString(const PeerId& peer, char* out, size_t cap);

// Parses the canonical text; type is taken from `type` (the text carries no
// type). False on malformed input.
bool peerIdFromString(const char* text, uint8_t type, PeerId& out);

// The forget seam: answers exactly which bond the companion may delete.
// True fills `out` with the stored identity; false means "no companion bond
// is configured" and the caller must delete NOTHING (in particular never the
// whole bond store).
bool companionBondTarget(const PeerId& stored, PeerId& out);

}  // namespace blecompanion
