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

}  // namespace companiongate
