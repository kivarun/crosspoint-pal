// The P1-B9 standby diagnostic's pure contract: record geometry, bounded
// appends with drop accounting, 64-bit interval accumulation across the
// 32-bit microsecond boundary, interval-state attribution (invalid captures
// stay in their state's population), coverage and the separated
// residency/clock verdicts that never turn missing evidence into a PASS.
#include <gtest/gtest.h>

#include <climits>
#include <cstring>

#include "util/CompanionStandbyDiag.h"

namespace {

namespace csd = companionstandbydiag;

csd::Buffer& buffer() {
  static csd::Buffer b;
  return b;
}

void seedAdvertising(csd::Buffer& b) {
  csd::reset(b);
  csd::recordStartup(b, csd::kLpclkExt32kXtal, csd::kCrystalNotRecorded, 0);
}

// One valid advertising span plus one optional invalid advertising span, the
// shape of the C1 blocker-1 reproduction.
csd::Buffer& makeAdvertisingSpans(const uint64_t firstSleepUs, const uint32_t firstSec, const bool secondValid,
                                  const uint32_t secondSec) {
  auto& b = buffer();
  seedAdvertising(b);
  csd::PmRecord record;
  record.startSec = 0;
  record.endSec = firstSec;
  record.modeUsDelta = static_cast<int64_t>(firstSleepUs);
  record.intervalClass = csd::IntervalClass::Advertising;
  record.samplesValid = true;
  csd::appendPmSample(b, record, false);
  csd::accumulateInterval(b, csd::IntervalClass::Advertising, {firstSec, 0, 0, static_cast<int64_t>(firstSleepUs)},
                          true);
  record.startSec = firstSec;
  record.endSec = firstSec + secondSec;
  record.modeUsDelta = 0;
  record.samplesValid = secondValid;
  csd::appendPmSample(b, record, false);
  csd::accumulateInterval(b, csd::IntervalClass::Advertising, {secondSec, 0, 0, 0}, secondValid);
  return b;
}

}  // namespace

TEST(CompanionStandbyDiag, RecordGeometryFitsTheBudget) {
  // Packed PM fields: 4+4+4+4+8+1+1 = 26 bytes.
  EXPECT_EQ(csd::kPmSampleSize, 26u);
  EXPECT_EQ(csd::kHeaderSize, 84u);
  EXPECT_EQ(csd::kHeartbeatSize, 16u);
  EXPECT_EQ(csd::kTransitionSize, 8u);
  EXPECT_EQ(csd::kErrorSize, 12u);
  EXPECT_EQ(csd::kStartupSize, 8u);
  EXPECT_EQ(csd::kSnapshotSize, 16u);
  EXPECT_LE(csd::kBufferNeeded, csd::kBufferBudget);
  // The regions do not overlap and end inside the budget.
  EXPECT_EQ(csd::kBufferNeeded, csd::kSnapshotOffset + csd::kSnapshotCapacity * csd::kSnapshotSize);
}

TEST(CompanionStandbyDiag, ResetStampsAValidHeader) {
  auto& b = buffer();
  csd::reset(b);
  EXPECT_TRUE(csd::headerValid(b));
  EXPECT_EQ(csd::flags(b), 0);
  EXPECT_EQ(csd::entriesTotal(b), 0u);
  EXPECT_EQ(csd::sleepUsTotal(b), 0);
  EXPECT_EQ(csd::classSeconds(b, csd::IntervalClass::Advertising), 0u);
  EXPECT_EQ(csd::totalRecords(b), 0u);
}

TEST(CompanionStandbyDiag, LeFieldRoundTrip) {
  uint8_t raw[8] = {};
  csd::storeLE16(raw, 0xBEEF);
  EXPECT_EQ(csd::loadLE16(raw), 0xBEEF);
  csd::storeLE32(raw, 0xDEADBEEFu);
  EXPECT_EQ(csd::loadLE32(raw), 0xDEADBEEFu);
  csd::storeLE64(raw, 0x123456789ABCDEF0ull);
  EXPECT_EQ(csd::loadLE64(raw), 0x123456789ABCDEF0ull);
}

TEST(CompanionStandbyDiag, Crc32MatchesTheStandardCheckValue) {
  // The readback integrity check builds on the standard CRC-32: the canonical
  // "123456789" check value pins the exact variant.
  EXPECT_EQ(csd::crc32(reinterpret_cast<const uint8_t*>("123456789"), 9), 0xCBF43926u);
  EXPECT_EQ(csd::crc32(nullptr, 0), 0u);
}

TEST(CompanionStandbyDiag, BufferCrcIsStableAndContentSensitive) {
  auto& b = buffer();
  seedAdvertising(b);
  const uint32_t before = csd::bufferCrc32(b);
  EXPECT_EQ(csd::bufferCrc32(b), before) << "a finished session's CRC is deterministic";
  csd::appendHeartbeat(b, 0, csd::kRadioAdvertising, 1000, 900);
  EXPECT_NE(csd::bufferCrc32(b), before) << "any recorded byte must change the CRC";
}

TEST(CompanionStandbyDiag, BoundedAppendsCountDropsWithoutTouchingTotals) {
  auto& b = buffer();
  seedAdvertising(b);
  for (uint32_t i = 0; i < csd::kHeartbeatCapacity; ++i) {
    ASSERT_TRUE(csd::appendHeartbeat(b, i, csd::kRadioAdvertising, 1000, 900));
  }
  EXPECT_FALSE(csd::appendHeartbeat(b, csd::kHeartbeatCapacity, csd::kRadioAdvertising, 1000, 900));
  EXPECT_EQ(csd::dropped(b, csd::kHdrDroppedHeartbeat), 1u);
  EXPECT_EQ(csd::count(b, csd::kHdrHeartbeatCount), csd::kHeartbeatCapacity);
  EXPECT_TRUE(csd::headerValid(b));
  // Periodic and transition-driven PM samples budget separately.
  csd::PmRecord record;
  record.samplesValid = true;
  for (uint32_t i = 0; i < csd::kPeriodicPmCapacity; ++i) {
    record.startSec = i;
    record.endSec = i + 1;
    ASSERT_TRUE(csd::appendPmSample(b, record, /*transitionDriven=*/false));
  }
  EXPECT_FALSE(csd::appendPmSample(b, record, false));
  EXPECT_EQ(csd::dropped(b, csd::kHdrDroppedPeriodicPm), 1u);
  EXPECT_TRUE(csd::appendPmSample(b, record, /*transitionDriven=*/true))
      << "a transition-driven sample survives a full periodic bucket";
  EXPECT_EQ(csd::dropped(b, csd::kHdrDroppedTransitionPm), 0u);
  // The start/exit heap snapshots budget separately from every other class.
  ASSERT_TRUE(csd::appendSnapshot(b, 0, 1000, 900, false));
  ASSERT_TRUE(csd::appendSnapshot(b, 1, 990, 890, true));
  for (uint32_t i = 0; i < csd::kSnapshotCapacity; ++i) {
    (void)csd::appendSnapshot(b, i, 1000, 900, false);
  }
  EXPECT_EQ(csd::count(b, csd::kHdrSnapshotCount), csd::kSnapshotCapacity);
  EXPECT_GT(csd::dropped(b, csd::kHdrDroppedSnapshot), 0u);
}

TEST(CompanionStandbyDiag, TransitionLogOverflowSetsTheInconclusiveFlag) {
  auto& b = buffer();
  seedAdvertising(b);
  for (uint32_t i = 0; i < csd::kTransitionCapacity; ++i) {
    ASSERT_TRUE(csd::appendTransition(b, i, csd::kRadioAdvertising, csd::kRadioConnected));
  }
  EXPECT_FALSE(csd::appendTransition(b, csd::kTransitionCapacity, csd::kRadioAdvertising, csd::kRadioConnected));
  EXPECT_EQ(csd::dropped(b, csd::kHdrDroppedTransition), 1u);
  EXPECT_NE(csd::flags(b) & csd::kFlagTransitionOverflow, 0u);
  EXPECT_EQ(csd::advertisingResidencyVerdict(b), csd::Verdict::Inconclusive)
      << "an incomplete transition history must never produce a PASS";
}

TEST(CompanionStandbyDiag, AccumulationSurvivesThe32BitMicrosecondBoundary) {
  auto& b = buffer();
  seedAdvertising(b);
  // Two intervals of one hour each: 3.6e9 us per interval already exceeds
  // 2^32 (4294.97 s) — exactly the wrap the 32-bit record cannot carry.
  const int64_t hour = 3600LL * 1000000;
  csd::accumulateInterval(b, csd::IntervalClass::Advertising, {3600, 0, 0, hour}, true);
  csd::accumulateInterval(b, csd::IntervalClass::Advertising, {3600, 0, 0, hour}, true);
  EXPECT_EQ(csd::sleepUsTotal(b), 2 * hour) << "the 64-bit total must not wrap";
  EXPECT_GT(csd::sleepUsTotal(b), static_cast<long long>(UINT32_MAX));
  EXPECT_EQ(csd::classSeconds(b, csd::IntervalClass::Advertising), 7200u);
}

TEST(CompanionStandbyDiag, InvalidIntervalsCarryWallTimeButNoSleep) {
  auto& b = buffer();
  seedAdvertising(b);
  csd::accumulateInterval(b, csd::IntervalClass::Advertising, {600, 500, 0, 60000000}, false);
  EXPECT_EQ(csd::classSeconds(b, csd::IntervalClass::Advertising), 600u)
      << "an invalid span stays in its state's measurement population";
  EXPECT_EQ(csd::sleepUsTotal(b), 0) << "an unattributable span must not fabricate sleep";
  EXPECT_EQ(csd::entriesTotal(b), 0u);
  csd::accumulateInterval(b, csd::IntervalClass::Advertising, {600, 500, 0, 60000000}, true);
  EXPECT_EQ(csd::sleepUsTotal(b), 60000000) << "a valid span counts its sleep";
  EXPECT_EQ(csd::entriesTotal(b), 500u);
}

TEST(CompanionStandbyDiag, AttributionFollowsTheRadioStateOnly) {
  // C1 blocker 1: validity no longer redirects a span into a Mixed bucket —
  // the class always names the state that held; samplesValid marks validity.
  EXPECT_EQ(csd::classifyInterval(csd::kRadioAdvertising), csd::IntervalClass::Advertising);
  EXPECT_EQ(csd::classifyInterval(csd::kRadioConnected), csd::IntervalClass::Connected);
  EXPECT_EQ(csd::classifyInterval(csd::kRadioAwaitingAuth), csd::IntervalClass::Connected);
  EXPECT_EQ(csd::classifyInterval(csd::kRadioStopped), csd::IntervalClass::Other);
}

TEST(CompanionStandbyDiag, InvalidAdvertisingCaptureCannotProduceAPass) {
  // The C1 blocker-1 reproduction: 300 s advertising with 95% measured
  // residency, then 300 s advertising whose PM capture failed. The invalid
  // span must stay in the advertising population: coverage drops to 50% and
  // the verdict can never be PASS.
  const auto& b = makeAdvertisingSpans(285000000, 300, /*secondValid=*/false, 300);
  EXPECT_EQ(csd::classSeconds(b, csd::IntervalClass::Advertising), 600u)
      << "the invalid span's wall time must remain in the advertising denominator";
  EXPECT_EQ(csd::storedAdvertisingSec(b), 300u) << "only the valid span covers the numerator";
  EXPECT_LT(csd::advertisingCoveragePermyriad(b), 9800u);
  EXPECT_NE(csd::advertisingResidencyVerdict(b), csd::Verdict::Pass);
  EXPECT_EQ(csd::advertisingResidencyVerdict(b), csd::Verdict::Inconclusive);
}

TEST(CompanionStandbyDiag, SmallInvalidSpanDilutesResidencyConservatively) {
  // A tiny invalid span stays in the denominator as zero measured sleep: a
  // PASS then requires the measured part to survive that dilution.
  const auto& b = makeAdvertisingSpans(285000000, 300, /*secondValid=*/false, 5);
  EXPECT_LT(csd::advertisingCoveragePermyriad(b), 10000u);
  EXPECT_LT(csd::residencyPermyriad(285000000, 305), 9500u) << "the unmeasured span may not be credited with sleep";
}

TEST(CompanionStandbyDiag, ResidencyThresholdIsNinetyPercent) {
  // 1000 s wall; sleep is measured in microseconds.
  const uint64_t wall = 1000;
  EXPECT_EQ(csd::residencyPermyriad(950000000, wall), 9500u);
  EXPECT_GE(csd::residencyPermyriad(900000000, wall), 9000u);
  EXPECT_LT(csd::residencyPermyriad(899000000, wall), 9000u);
  EXPECT_EQ(csd::residencyPermyriad(1000, 0), 0u) << "no wall time, no ratio";
}

TEST(CompanionStandbyDiag, VerdictBelowNinetyPercentIsAFail) {
  // 95%: pass. 89.9%: fail. Both with full coverage and a valid clock.
  EXPECT_EQ(csd::advertisingResidencyVerdict(makeAdvertisingSpans(950000000, 1000, false, 0)), csd::Verdict::Pass);
  EXPECT_EQ(csd::advertisingResidencyVerdict(makeAdvertisingSpans(899000000, 1000, false, 0)), csd::Verdict::Fail);
}

TEST(CompanionStandbyDiag, ResidencyVerdictIsIndependentOfTheClockSource) {
  // C1 correction 1: a MAIN_XTAL fallback behind a 95% measured residency is
  // a PASS on residency alone — but the clock verdict is Invalid and the
  // overall B9 acceptance can never read as PASS.
  seedAdvertising(buffer());
  csd::recordStartup(buffer(), csd::kLpclkMainXtal, csd::kCrystalNotRecorded, 0);
  csd::PmRecord record;
  record.startSec = 0;
  record.endSec = 1000;
  record.modeUsDelta = 950000000;
  record.intervalClass = csd::IntervalClass::Advertising;
  record.samplesValid = true;
  csd::appendPmSample(buffer(), record, false);
  csd::accumulateInterval(buffer(), csd::IntervalClass::Advertising, {1000, 0, 0, 950000000}, true);
  EXPECT_EQ(csd::advertisingResidencyVerdict(buffer()), csd::Verdict::Pass);
  EXPECT_EQ(csd::clockVerdict(buffer()), csd::ClockVerdict::Invalid);
  EXPECT_EQ(csd::standbyAcceptanceVerdict(buffer()), csd::Verdict::Inconclusive)
      << "a MAIN_XTAL fallback must never be mistaken for B9 acceptance";
}

TEST(CompanionStandbyDiag, ClockVerdictReflectsTheStoredSource) {
  auto& b = buffer();
  csd::reset(b);  // no startup record at all
  EXPECT_EQ(csd::clockVerdict(b), csd::ClockVerdict::NotRecorded);
  csd::reset(b);
  csd::recordStartup(b, csd::kLpclkExt32kXtal, csd::kCrystalNotRecorded, 0);
  EXPECT_EQ(csd::clockVerdict(b), csd::ClockVerdict::Valid);
  csd::reset(b);
  csd::recordStartup(b, csd::kLpclkMainXtal, csd::kCrystalNotRecorded, 0);
  EXPECT_EQ(csd::clockVerdict(b), csd::ClockVerdict::Invalid);
  csd::reset(b);
  csd::recordStartup(b, 0, csd::kCrystalNotRecorded, 0);
  EXPECT_EQ(csd::clockVerdict(b), csd::ClockVerdict::NotRecorded) << "a zero source never set the recorded flag";
}

TEST(CompanionStandbyDiag, MissingClockEvidenceKeepsOverallInconclusive) {
  auto& b = buffer();
  csd::reset(b);  // no startup record: the clock was never read
  csd::PmRecord record;
  record.startSec = 0;
  record.endSec = 1000;
  record.modeUsDelta = 950000000;
  record.intervalClass = csd::IntervalClass::Advertising;
  record.samplesValid = true;
  csd::appendPmSample(b, record, false);
  csd::accumulateInterval(b, csd::IntervalClass::Advertising, {1000, 0, 0, 950000000}, true);
  EXPECT_EQ(csd::advertisingResidencyVerdict(b), csd::Verdict::Pass) << "residency alone is still measurable";
  EXPECT_EQ(csd::standbyAcceptanceVerdict(b), csd::Verdict::Inconclusive);
}

TEST(CompanionStandbyDiag, InsufficientCoverageIsInconclusive) {
  // Wall time 1000 s (800 s measured + a 200 s invalid span) but only 800 s
  // covered: 80% coverage < 98%.
  EXPECT_EQ(csd::advertisingResidencyVerdict(makeAdvertisingSpans(950000000, 800, /*secondValid=*/false, 200)),
            csd::Verdict::Inconclusive);
}

TEST(CompanionStandbyDiag, PmValidityCensusCountsBothBuckets) {
  auto& b = buffer();
  seedAdvertising(b);
  csd::PmRecord record;
  record.intervalClass = csd::IntervalClass::Advertising;
  record.samplesValid = true;
  csd::appendPmSample(b, record, false);
  record.samplesValid = false;
  csd::appendPmSample(b, record, false);
  csd::appendPmSample(b, record, /*transitionDriven=*/true);
  record.samplesValid = true;
  csd::appendPmSample(b, record, /*transitionDriven=*/true);
  csd::PmValidity validity{};
  csd::pmValidity(b, validity);
  EXPECT_EQ(validity.periodicValid, 1u);
  EXPECT_EQ(validity.periodicInvalid, 1u);
  EXPECT_EQ(validity.transitionValid, 1u);
  EXPECT_EQ(validity.transitionInvalid, 1u);
}

TEST(CompanionStandbyDiag, StartupRoundTripCarriesTheClockFields) {
  auto& b = buffer();
  csd::reset(b);
  csd::recordStartup(b, csd::kLpclkMainXtal, csd::kCrystalNotRecorded, 0x12345678u);
  const csd::StartupRecord startup = csd::readStartup(b);
  EXPECT_EQ(startup.lpclk, csd::kLpclkMainXtal);
  EXPECT_EQ(startup.crystal, csd::kCrystalNotRecorded);
  EXPECT_EQ(startup.configEcho, 0x12345678u);
}

TEST(CompanionStandbyDiag, TotalRecordsSumsEveryClass) {
  auto& b = buffer();
  seedAdvertising(b);
  csd::PmRecord record;
  record.samplesValid = true;
  csd::appendPmSample(b, record, false);
  csd::appendPmSample(b, record, true);
  csd::appendHeartbeat(b, 0, csd::kRadioStopped, 1, 1);
  csd::appendTransition(b, 0, csd::kRadioStopped, csd::kRadioAdvertising);
  csd::appendError(b, 0, 1, 1, 1);
  csd::appendSnapshot(b, 0, 1, 1, false);
  EXPECT_EQ(csd::totalRecords(b), 6u);
}

TEST(CompanionStandbyDiag, SnapshotRecordsCarryHeapAndBoundary) {
  auto& b = buffer();
  seedAdvertising(b);
  ASSERT_TRUE(csd::appendSnapshot(b, 0, 100000, 90000, false));
  ASSERT_TRUE(csd::appendSnapshot(b, 42, 99000, 89000, true));
  EXPECT_EQ(csd::count(b, csd::kHdrSnapshotCount), 2u);
  const uint8_t* start = b.bytes + csd::kSnapshotOffset;
  const uint8_t* exit = start + csd::kSnapshotSize;
  EXPECT_EQ(csd::loadLE32(start + 4), 100000u);
  EXPECT_EQ(start[12], 1) << "tag 1 = session start";
  EXPECT_EQ(csd::loadLE32(exit), 42u);
  EXPECT_EQ(csd::loadLE32(exit + 8), 89000u);
  EXPECT_EQ(exit[12], 2) << "tag 2 = session exit";
}
