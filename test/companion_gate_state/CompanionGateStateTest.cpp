#include <gtest/gtest.h>

#include "util/CompanionGateState.h"

// The gate arm record's pure validation policy (lib-side core of
// src/util/CompanionGateState.h). The device-side RTC global lives in
// CompanionGateState.cpp and is not exercised here.

namespace {

using companiongate::GATE_ARM_MAGIC;
using companiongate::GateArmRecord;
using companiongate::Kind;

GateArmRecord armed(const bool diagnostic) {
  GateArmRecord record{};
  record.magic = GATE_ARM_MAGIC;
  record.kind = static_cast<uint8_t>(Kind::BackgroundCheck);
  record.flags = diagnostic ? companiongate::DIAGNOSTIC_FLAG : 0;
  record.reserved = 0;
  return record;
}

}  // namespace

TEST(GateArmRecord, LayoutIsPinned) {
  static_assert(sizeof(GateArmRecord) == 8, "RTC budget");
  EXPECT_EQ(offsetof(GateArmRecord, magic), 0u);
  EXPECT_EQ(offsetof(GateArmRecord, kind), 4u);
  EXPECT_EQ(offsetof(GateArmRecord, flags), 5u);
  EXPECT_EQ(offsetof(GateArmRecord, reserved), 6u);
}

TEST(GateArmRecord, AcceptsArmedRecord) {
  EXPECT_TRUE(companiongate::recordValid(armed(false)));
  EXPECT_TRUE(companiongate::recordValid(armed(true)));
}

TEST(GateArmRecord, DiagnosticBitReadsOnlyWhenValid) {
  EXPECT_TRUE(companiongate::recordDiagnostic(armed(true)));
  EXPECT_FALSE(companiongate::recordDiagnostic(armed(false)));
  auto torn = armed(true);
  torn.magic = 0;  // a torn write: magic cleared
  EXPECT_FALSE(companiongate::recordValid(torn));
  EXPECT_FALSE(companiongate::recordDiagnostic(torn));
}

// RTC_NOINIT is garbage on a cold boot: any wrong field fails closed.
TEST(GateArmRecord, RejectsGarbage) {
  auto record = armed(false);
  record.magic = GATE_ARM_MAGIC + 1;
  EXPECT_FALSE(companiongate::recordValid(record));
  record = armed(false);
  record.magic = 0;
  EXPECT_FALSE(companiongate::recordValid(record));
  record = armed(false);
  record.kind = 2;  // an unknown kind
  EXPECT_FALSE(companiongate::recordValid(record));
  record = armed(false);
  record.reserved = 1;  // reserved must stay zero
  EXPECT_FALSE(companiongate::recordValid(record));
}

TEST(GateArmRecord, MagicIsWrittenLastSemantics) {
  // A record whose magic arrives before the payload fields cannot validate
  // mid-write with garbage payload: the flags/kind bytes of a zeroed record
  // must not read as a valid arm.
  GateArmRecord record{};
  record.magic = GATE_ARM_MAGIC;  // magic written first by mistake
  EXPECT_FALSE(companiongate::recordValid(record));  // kind 0 != BackgroundCheck
}
