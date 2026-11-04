#pragma once

// Companion identity mirror: a tiny NVS record of the paired phone's identity
// that the headless gate can read WITHOUT mounting the SD card (SETTINGS lives
// in /.crosspoint/settings.json). Three stores hold identity-relevant state
// (p1-companion-gate preflight §2 Q8):
//   - the NimBLE bond store (NVS) is the cryptographic ground truth;
//   - this mirror names which bonded peer the background window may talk to;
//   - SETTINGS is the user-visible record and decides nothing about access.
// Background access therefore requires mirror ∩ bond agreement; a
// SETTINGS-vs-mirror divergence is data-quality (logged) and can never grant
// access, because the old phone holds no LTK.
//
// The pure decision core is inline here and host-testable; the Preferences
// I/O lives in the .cpp and is device-side only.

#include <CompanionPeer.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace companionidentity {

// Canonical form mirrors the settings text: "aa:bb:cc:dd:ee:ff" (17 chars) +
// the NimBLE address type byte (0 public, 1 random).
struct StoredIdentity {
  char addr[18] = {};
  uint8_t type = 0xFF;
};

namespace detail {
inline bool hexDigit(const char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }
}  // namespace detail

// Pure validation: exactly 17 lowercase-hex/colon characters and a known
// address type.
inline bool textValid(const char* addr, const uint8_t type) {
  if (addr == nullptr) return false;
  if (type != blecompanion::ADDR_TYPE_PUBLIC && type != blecompanion::ADDR_TYPE_RANDOM) return false;
  if (strlen(addr) != 17) return false;
  for (size_t i = 0; i < 17; ++i) {
    if (i % 3 == 2) {
      if (addr[i] != ':') return false;
    } else if (!detail::hexDigit(addr[i])) {
      return false;
    }
  }
  return true;
}

// Pure migration decision (mirror-if-absent): a valid mirror is NEVER
// overwritten from SETTINGS — the mirror tracks the last successfully
// persisted pairing, which in both the SD-write-failure and the
// backup-restore cases matches the real bond better than SETTINGS does.
enum class Migration : uint8_t { None, WriteMirrorFromSettings };

inline Migration migrationDecision(const bool mirrorValid, const char* settingsAddr, const uint8_t settingsType) {
  if (mirrorValid) return Migration::None;
  if (textValid(settingsAddr, settingsType)) return Migration::WriteMirrorFromSettings;
  return Migration::None;
}

// Pure access predicate (mirror ∩ bond): both must hold before the gate
// treats the window as productive. The bond-presence half is evaluated by
// BleCompanion::storedIdentityBonded() (device-side); this predicate keeps
// the composition host-testable.
inline bool gateMayProceed(const bool identityValid, const bool bondPresent) { return identityValid && bondPresent; }

// Pure read-back acceptance — the core of load(). save() writes a 17-character
// address; the Arduino core's getString(key, char*, maxLen) reports the stored
// length including the zero terminator on IDF >= 5.x (18), so the reported
// length is core-version dependent and must never be required to be exactly
// 17. Contract: accepts exactly what save() wrote, under either reporting
// semantics, and fails closed on everything else:
//   - read must be 17 or 18 — the only reported lengths getString can produce
//     for a save() record; arbitrary non-zero lengths are foreign records
//     (a core bump re-verifies the reported length against this pair);
//   - the buffer must be NUL-terminated within its 18 bytes (checked before
//     any strlen, which would run past the buffer otherwise);
//   - textValid enforces the 17-character format and the address type.
inline bool decodeMirrorRecord(const size_t read, const char (&addr)[18], const uint8_t type, StoredIdentity& out) {
  if (read != 17 && read != 18) return false;
  if (memchr(addr, '\0', sizeof(addr)) == nullptr) return false;
  if (!textValid(addr, type)) return false;
  memcpy(out.addr, addr, sizeof(out.addr));
  out.type = type;
  return true;
}

// Device-side NVS I/O (namespace "cpgate"). load() validates before
// returning; save() validates first and fails without writing on garbage.
bool load(StoredIdentity& out);
bool save(const char* addr, uint8_t type);
bool clear();

// Full-boot migration helper: applies migrationDecision and, on
// WriteMirrorFromSettings, persists the SETTINGS identity to the mirror.
// Called after SETTINGS.loadFromFile().
void migrateFromSettings(const char* settingsAddr, uint8_t settingsType);

}  // namespace companionidentity
