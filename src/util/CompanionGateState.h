#pragma once

// One-shot arm record for the headless Sleep Companion gate (the bounded BLE
// window between a deep-sleep timer wake and the next deep sleep). The record
// lives in RTC memory: it survives deep sleep and is garbage on a cold boot,
// so validity is magic-guarded and fails closed — an armed gate dispatches
// exactly once, and every wake that does not run the window clears the arm.
// The flags byte carries the dev-only diagnostic bit (USB bypass override);
// production builds never set or honor it.

#include <cstddef>
#include <cstdint>

namespace companiongate {

enum Kind : uint8_t {
  BackgroundCheck = 1,  // the only kind in Slice A
};

inline constexpr uint8_t DIAGNOSTIC_FLAG = 0x01;

// 8 B total — a deliberate cap on the RTC SLOW budget already shared with the
// 512 B slideshow state (~2.6 KB free on both targets). The magic is written
// LAST so a torn write can never validate.
struct GateArmRecord {
  uint32_t magic = 0;  // GATE_ARM_MAGIC, written LAST; zero = invalid
  uint8_t kind = 0;    // Kind, validated by recordValid
  uint8_t flags = 0;   // DIAGNOSTIC_FLAG bit
  uint16_t reserved = 0;
};
static_assert(sizeof(GateArmRecord) == 8, "gate arm record must stay within its RTC budget");
static_assert(offsetof(GateArmRecord, magic) == 0, "gate arm layout: magic first");
static_assert(offsetof(GateArmRecord, kind) == 4, "gate arm layout: kind after the magic");
static_assert(offsetof(GateArmRecord, flags) == 5, "gate arm layout: flags after the kind");
static_assert(offsetof(GateArmRecord, reserved) == 6, "gate arm layout: reserved last");

inline constexpr uint32_t GATE_ARM_MAGIC = 0x47415445;  // 'GATE'

// Pure validation policy (host-testable): exact magic, the known kind, and a
// zeroed reserved field.
inline bool recordValid(const GateArmRecord& record) {
  return record.magic == GATE_ARM_MAGIC && record.kind == static_cast<uint8_t>(Kind::BackgroundCheck) &&
         record.reserved == 0;
}

// Pure diagnostic-bit read (host-testable); false for an invalid record.
inline bool recordDiagnostic(const GateArmRecord& record) {
  return recordValid(record) && (record.flags & DIAGNOSTIC_FLAG) != 0;
}

// Device-side RTC accessors (the global lives in the .cpp). arm() invalidates
// FIRST, then writes the kind/flags and the magic last; the record is only
// valid once fully written. takeArmedWork() consumes: a valid arm reads the
// diagnostic bit, clears the record, and returns true.
bool arm(bool diagnostic);
void clearArm();
bool hasArmedWork();
bool armedIsDiagnostic();
bool takeArmedWork(bool& diagnosticOut);

}  // namespace companiongate
