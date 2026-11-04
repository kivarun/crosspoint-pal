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
  record.magic = GATE_ARM_MAGIC;                     // magic written first by mistake
  EXPECT_FALSE(companiongate::recordValid(record));  // kind 0 != BackgroundCheck
}

// The dispatch verdict shares the arm record's 8 RTC bytes: after the arm is
// consumed, the magic becomes DISPATCH_VERDICT_MAGIC and the packed verdict
// lives in kind/flags/reserved. It must never read as an armed record.

TEST(DispatchVerdict, MagicsStayDistinct) {
  EXPECT_NE(companiongate::DISPATCH_VERDICT_MAGIC, GATE_ARM_MAGIC);
  EXPECT_NE(companiongate::DISPATCH_VERDICT_MAGIC, 0u);
}

TEST(DispatchVerdict, RoundTripsEveryInputCombination) {
  for (uint32_t mask = 0; mask < 256u; ++mask) {
    for (int decision = 0; decision <= static_cast<int>(companiongate::Action::Run); ++decision) {
      for (int outcome = 0; outcome <= static_cast<int>(companiongate::RunOutcome::NoBond); ++outcome) {
        const companiongate::DispatchVerdict in{(mask & 1u) != 0,
                                                (mask & 2u) != 0,
                                                (mask & 4u) != 0,
                                                (mask & 8u) != 0,
                                                (mask & 16u) != 0,
                                                (mask & 32u) != 0,
                                                (mask & 64u) != 0,
                                                (mask & 128u) != 0,
                                                static_cast<companiongate::Action>(decision),
                                                static_cast<companiongate::RunOutcome>(outcome)};
        companiongate::DispatchVerdict out{};
        ASSERT_TRUE(companiongate::unpackDispatchVerdict(companiongate::packDispatchVerdict(in), out));
        EXPECT_EQ(out.wakeTimer, in.wakeTimer);
        EXPECT_EQ(out.armValid, in.armValid);
        EXPECT_EQ(out.diagnostic, in.diagnostic);
        EXPECT_EQ(out.recovery, in.recovery);
        EXPECT_EQ(out.usb, in.usb);
        EXPECT_EQ(out.slideshow, in.slideshow);
        EXPECT_EQ(out.identity, in.identity);
        EXPECT_EQ(out.battery, in.battery);
        EXPECT_EQ(out.decision, in.decision);
        EXPECT_EQ(out.outcome, in.outcome);
      }
    }
  }
}

// Every decision and every outcome must map to a distinguishable payload: a
// refused wake and a refused window attempt must never read as success.
TEST(DispatchVerdict, DecisionsAndOutcomesAreDistinct) {
  const companiongate::DispatchVerdict base{true,
                                            true,
                                            true,
                                            false,
                                            false,
                                            false,
                                            true,
                                            true,
                                            companiongate::Action::Ordinary,
                                            companiongate::RunOutcome::NotRun};
  for (int decision = 0; decision <= static_cast<int>(companiongate::Action::Run); ++decision) {
    for (int other = 0; other <= static_cast<int>(companiongate::Action::Run); ++other) {
      auto a = base;
      auto b = base;
      a.decision = static_cast<companiongate::Action>(decision);
      b.decision = static_cast<companiongate::Action>(other);
      if (decision != other) {
        EXPECT_NE(companiongate::packDispatchVerdict(a), companiongate::packDispatchVerdict(b));
      }
    }
  }
  for (int outcome = 0; outcome <= static_cast<int>(companiongate::RunOutcome::NoBond); ++outcome) {
    for (int other = 0; other <= static_cast<int>(companiongate::RunOutcome::NoBond); ++other) {
      auto a = base;
      auto b = base;
      a.outcome = static_cast<companiongate::RunOutcome>(outcome);
      b.outcome = static_cast<companiongate::RunOutcome>(other);
      if (outcome != other) {
        EXPECT_NE(companiongate::packDispatchVerdict(a), companiongate::packDispatchVerdict(b));
      }
    }
  }
  // A Run that was refused before the window (no-bond) must not read as a
  // completed window (window-timeout).
  auto refused = base;
  refused.decision = companiongate::Action::Run;
  refused.outcome = companiongate::RunOutcome::NoBond;
  auto completed = refused;
  completed.outcome = companiongate::RunOutcome::WindowTimeout;
  EXPECT_NE(companiongate::packDispatchVerdict(refused), companiongate::packDispatchVerdict(completed));
}

// RTC_NOINIT is garbage on a cold boot: reserved bits nonzero, an unknown
// decision, or an unknown outcome must all fail the unpack.
TEST(DispatchVerdict, RejectsGarbage) {
  const companiongate::DispatchVerdict base{true,
                                            true,
                                            true,
                                            false,
                                            false,
                                            false,
                                            true,
                                            true,
                                            companiongate::Action::Ordinary,
                                            companiongate::RunOutcome::NotRun};
  companiongate::DispatchVerdict out{};
  EXPECT_FALSE(companiongate::unpackDispatchVerdict(0xFFFFFFFFu, out));
  EXPECT_FALSE(companiongate::unpackDispatchVerdict(1u << 14, out));  // reserved bit set
  EXPECT_FALSE(companiongate::unpackDispatchVerdict(1u << 31, out));
  const uint32_t badDecision = companiongate::packDispatchVerdict(base) | (0x5u << 8);
  EXPECT_FALSE(companiongate::unpackDispatchVerdict(badDecision, out));
  const uint32_t badOutcome = companiongate::packDispatchVerdict(base) | (0x6u << 11);
  EXPECT_FALSE(companiongate::unpackDispatchVerdict(badOutcome, out));
  // Boundary values are the valid maxima and must still unpack.
  const uint32_t maxLegal = companiongate::packDispatchVerdict(base) | (4u << 8) | (5u << 11);
  EXPECT_TRUE(companiongate::unpackDispatchVerdict(maxLegal, out));
  EXPECT_EQ(out.decision, companiongate::Action::Run);
  EXPECT_EQ(out.outcome, companiongate::RunOutcome::NoBond);
}

TEST(DispatchVerdict, NamesAreDistinctAndNonNull) {
  const char* actions[] = {companiongate::gateActionName(companiongate::Action::Ordinary),
                           companiongate::gateActionName(companiongate::Action::SlideshowWins),
                           companiongate::gateActionName(companiongate::Action::SleepNoRadio),
                           companiongate::gateActionName(companiongate::Action::StandDown),
                           companiongate::gateActionName(companiongate::Action::Run)};
  for (size_t i = 0; i < 5; ++i) {
    ASSERT_NE(actions[i], nullptr);
    for (size_t j = i + 1; j < 5; ++j) {
      EXPECT_STRNE(actions[i], actions[j]);
    }
  }
  const char* outcomes[] = {companiongate::runOutcomeName(companiongate::RunOutcome::NotRun),
                            companiongate::runOutcomeName(companiongate::RunOutcome::WindowTimeout),
                            companiongate::runOutcomeName(companiongate::RunOutcome::PowerAbort),
                            companiongate::runOutcomeName(companiongate::RunOutcome::NoIdentity),
                            companiongate::runOutcomeName(companiongate::RunOutcome::RadioStartFailed),
                            companiongate::runOutcomeName(companiongate::RunOutcome::NoBond)};
  for (size_t i = 0; i < 6; ++i) {
    ASSERT_NE(outcomes[i], nullptr);
    for (size_t j = i + 1; j < 6; ++j) {
      EXPECT_STRNE(outcomes[i], outcomes[j]);
    }
  }
}
