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

#include "CompanionGatePolicy.h"

namespace companiongate {

enum Kind : uint8_t {
  BackgroundCheck = 1,  // the only kind in Slice A
};

inline constexpr uint8_t DIAGNOSTIC_FLAG = 0x01;

// 8 B total — a deliberate cap on the RTC SLOW budget already shared with the
// 512 B slideshow state (~2.6 KB free on both targets). The magic is written
// LAST so a torn write can never validate. When the arm is consumed, the same
// bytes take a second life as a DispatchVerdict under DISPATCH_VERDICT_MAGIC —
// kind/flags/reserved are then the packed verdict payload, not arm fields.
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

// Why a dispatched Run attempt ended. Values are stable across builds (they
// are stored in RTC); NotRun is the state stored at the dispatch site before
// the window starts.
enum class RunOutcome : uint8_t {
  NotRun = 0,
  WindowTimeout = 1,
  PowerAbort = 2,
  NoIdentity = 3,
  RadioStartFailed = 4,
  NoBond = 5,
};

// The last timer-wake dispatch decision. Stored into the SAME 8 RTC bytes as
// the consumed arm record: the magic becomes DISPATCH_VERDICT_MAGIC and the
// payload lives in kind/flags/reserved, so no additional RTC budget is used.
// A non-timer wake never stores a verdict, so the last timer-wake decision
// survives the interactive boots that follow it. No identifiers, keys, or
// session content are stored — only booleans and enum values.
struct DispatchVerdict {
  bool wakeTimer;
  bool armValid;
  bool diagnostic;
  bool recovery;
  bool usb;
  bool slideshow;
  bool identity;
  bool battery;
  Action decision;
  RunOutcome outcome;
};

inline constexpr uint32_t DISPATCH_VERDICT_MAGIC = 0x47425644u;  // 'GBVD'

// Payload layout (stable across builds):
//   bit 0..7   wake-timer | arm-valid | diag | recovery | usb | slideshow |
//              identity | battery
//   bit 8..10  Action
//   bit 11..13 RunOutcome
//   bit 14..31 zero — unpack rejects nonzero, a second guard against a
//              coincidental magic hit on cold-boot garbage. Extend this guard
//              when the payload layout grows.
inline uint32_t packDispatchVerdict(const DispatchVerdict& verdict) {
  uint32_t packed = 0;
  if (verdict.wakeTimer) packed |= 1u << 0;
  if (verdict.armValid) packed |= 1u << 1;
  if (verdict.diagnostic) packed |= 1u << 2;
  if (verdict.recovery) packed |= 1u << 3;
  if (verdict.usb) packed |= 1u << 4;
  if (verdict.slideshow) packed |= 1u << 5;
  if (verdict.identity) packed |= 1u << 6;
  if (verdict.battery) packed |= 1u << 7;
  packed |= static_cast<uint32_t>(verdict.decision) << 8;
  packed |= static_cast<uint32_t>(verdict.outcome) << 11;
  return packed;
}

inline bool unpackDispatchVerdict(const uint32_t payload, DispatchVerdict& out) {
  out = DispatchVerdict{};
  if ((payload >> 14) != 0) return false;
  const uint32_t decision = (payload >> 8) & 0x7u;
  if (decision > static_cast<uint32_t>(Action::Run)) return false;
  const uint32_t outcome = (payload >> 11) & 0x7u;
  if (outcome > static_cast<uint32_t>(RunOutcome::NoBond)) return false;
  out.wakeTimer = (payload & (1u << 0)) != 0;
  out.armValid = (payload & (1u << 1)) != 0;
  out.diagnostic = (payload & (1u << 2)) != 0;
  out.recovery = (payload & (1u << 3)) != 0;
  out.usb = (payload & (1u << 4)) != 0;
  out.slideshow = (payload & (1u << 5)) != 0;
  out.identity = (payload & (1u << 6)) != 0;
  out.battery = (payload & (1u << 7)) != 0;
  out.decision = static_cast<Action>(decision);
  out.outcome = static_cast<RunOutcome>(outcome);
  return true;
}

inline const char* gateActionName(const Action action) {
  switch (action) {
    case Action::Ordinary:
      return "ordinary";
    case Action::SlideshowWins:
      return "slideshow-wins";
    case Action::SleepNoRadio:
      return "sleep-no-radio";
    case Action::StandDown:
      return "stand-down";
    case Action::Run:
      return "run";
  }
  return "unknown";
}

inline const char* runOutcomeName(const RunOutcome outcome) {
  switch (outcome) {
    case RunOutcome::NotRun:
      return "not-run";
    case RunOutcome::WindowTimeout:
      return "window-timeout";
    case RunOutcome::PowerAbort:
      return "power-abort";
    case RunOutcome::NoIdentity:
      return "no-identity";
    case RunOutcome::RadioStartFailed:
      return "radio-failed";
    case RunOutcome::NoBond:
      return "no-bond";
  }
  return "unknown";
}

// Device side: store the verdict over the consumed arm record (payload bytes
// first, magic LAST — the file's torn-write convention). readDispatchVerdict
// is false after a cold boot (garbage), after GATEARM re-arms, and whenever no
// timer wake has stored a verdict yet. updateRunOutcome rewrites only the
// outcome bits of an existing verdict; it is a no-op without one.
void storeDispatchVerdict(const DispatchVerdict& verdict);
bool readDispatchVerdict(DispatchVerdict& out);
void updateRunOutcome(const RunOutcome outcome);
}  // namespace companiongate
