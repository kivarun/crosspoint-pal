#include "CompanionGateState.h"

#include <esp_attr.h>

namespace companiongate {

// RTC_NOINIT: survives deep sleep, garbage on a cold boot — the magic guards
// every read (the SlideshowState pattern).
RTC_NOINIT_ATTR GateArmRecord armedRecord;

void clearArm() { armedRecord.magic = 0; }

bool arm(const bool diagnostic) {
  // Invalidate FIRST: a rejected arm never leaves a valid record behind.
  armedRecord.magic = 0;
  armedRecord.kind = static_cast<uint8_t>(Kind::BackgroundCheck);
  armedRecord.flags = diagnostic ? DIAGNOSTIC_FLAG : 0;
  armedRecord.reserved = 0;
  armedRecord.magic = GATE_ARM_MAGIC;  // LAST: the record is valid only now
  return true;
}

bool hasArmedWork() { return recordValid(armedRecord); }

bool armedIsDiagnostic() { return recordDiagnostic(armedRecord); }

bool takeArmedWork(bool& diagnosticOut) {
  if (!recordValid(armedRecord)) return false;
  diagnosticOut = recordDiagnostic(armedRecord);
  clearArm();
  return true;
}

namespace {

uint32_t verdictPayload() {
  return static_cast<uint32_t>(armedRecord.kind) | (static_cast<uint32_t>(armedRecord.flags) << 8) |
         (static_cast<uint32_t>(armedRecord.reserved) << 16);
}

void writeVerdictPayload(const uint32_t payload) {
  armedRecord.kind = static_cast<uint8_t>(payload & 0xFFu);
  armedRecord.flags = static_cast<uint8_t>((payload >> 8) & 0xFFu);
  armedRecord.reserved = static_cast<uint16_t>((payload >> 16) & 0xFFFFu);
}

}  // namespace

void storeDispatchVerdict(const DispatchVerdict& verdict) {
  const uint32_t payload = packDispatchVerdict(verdict);
  armedRecord.magic = 0;  // never leave an arm-looking record behind
  writeVerdictPayload(payload);
  armedRecord.magic = DISPATCH_VERDICT_MAGIC;  // LAST: valid only now
}

bool readDispatchVerdict(DispatchVerdict& out) {
  if (armedRecord.magic != DISPATCH_VERDICT_MAGIC) return false;
  return unpackDispatchVerdict(verdictPayload(), out);
}
void updateRunOutcome(const RunOutcome outcome) {
  if (armedRecord.magic != DISPATCH_VERDICT_MAGIC) return;
  // The magic deliberately stays valid during the rewrite: only the flags byte
  // carries changed bits, so a concurrent reader always sees the old or the
  // new verdict, never an invalid record.
  const uint32_t payload = (verdictPayload() & ~(uint32_t{0x7} << 11)) | (static_cast<uint32_t>(outcome) << 11);
  writeVerdictPayload(payload);
}

}  // namespace companiongate
