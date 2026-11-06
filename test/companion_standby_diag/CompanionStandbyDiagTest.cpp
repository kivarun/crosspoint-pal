// The P1-B9 standby diagnostic's pure contract: record geometry, bounded
// appends with drop accounting, 64-bit interval accumulation across the
// 32-bit microsecond boundary, millisecond interval attribution (whole-second
// truncation produced the C2 101.76% failure), coverage and the separated
// residency/clock verdicts that never turn missing or impossible evidence
// into a PASS.
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

// A valid advertising span [startMs,endMs) carrying the requested sleep, in
// the transition-driven bucket like the hardware smoke-test records.
csd::Buffer& addAdvertisingSpan(csd::Buffer& b, const uint32_t startMs, const uint32_t endMs, const int64_t sleepUs,
                                const bool valid = true) {
  csd::PmRecord record;
  record.startMs = startMs;
  record.endMs = endMs;
  record.modeUsDelta = valid ? sleepUs : 0;
  record.intervalClass = csd::IntervalClass::Advertising;
  record.samplesValid = valid;
  csd::appendPmSample(b, record, true);
  csd::accumulateInterval(b, csd::IntervalClass::Advertising, {endMs > startMs ? endMs - startMs : 0u, 0, 0, sleepUs},
                          valid);
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
  // Format version 3: session-relative millisecond timestamps.
  EXPECT_EQ(csd::kBufferVersion, 3u);
  EXPECT_LT(csd::kSessionCapMs, 0xFFFFFFFFu) << "24 h of milliseconds must fit u32";
}

TEST(CompanionStandbyDiag, ResetStampsAValidHeader) {
  auto& b = buffer();
  csd::reset(b);
  EXPECT_TRUE(csd::headerValid(b));
  EXPECT_EQ(csd::flags(b), 0);
  EXPECT_EQ(csd::entriesTotal(b), 0u);
  EXPECT_EQ(csd::sleepUsTotal(b), 0);
  EXPECT_EQ(csd::classMillis(b, csd::IntervalClass::Advertising), 0u);
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

TEST(CompanionStandbyDiag, SessionRelativeMillisecondsSurviveMillisWrap) {
  // millis() wraps at 2^32; the session-relative subtraction is unsigned, so
  // a session spanning the wrap reads continuously. The 24 h cap keeps the
  // result inside u32 (the same cast on a 64-bit host reproduces the wrap).
  EXPECT_EQ(csd::sessionRelMs(5000, 4290000000UL), (5000u - 4290000000u) & 0xFFFFFFFFu);
  EXPECT_EQ(csd::sessionRelMs(4290000000UL, 4290000000UL), 0u);
  // A full 24-hour session at an arbitrary offset stays exact.
  EXPECT_EQ(csd::sessionRelMs(100 + csd::kSessionCapMs, 100), csd::kSessionCapMs);
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
    ASSERT_TRUE(csd::appendHeartbeat(b, i * 1000, csd::kRadioAdvertising, 1000, 900));
  }
  EXPECT_FALSE(csd::appendHeartbeat(b, csd::kHeartbeatCapacity * 1000, csd::kRadioAdvertising, 1000, 900));
  EXPECT_EQ(csd::dropped(b, csd::kHdrDroppedHeartbeat), 1u);
  EXPECT_EQ(csd::count(b, csd::kHdrHeartbeatCount), csd::kHeartbeatCapacity);
  EXPECT_TRUE(csd::headerValid(b));
  // Periodic and transition-driven PM samples budget separately.
  csd::PmRecord record;
  record.samplesValid = true;
  for (uint32_t i = 0; i < csd::kPeriodicPmCapacity; ++i) {
    record.startMs = i * 60000;
    record.endMs = (i + 1) * 60000;
    ASSERT_TRUE(csd::appendPmSample(b, record, /*transitionDriven=*/false));
  }
  EXPECT_FALSE(csd::appendPmSample(b, record, false));
  EXPECT_EQ(csd::dropped(b, csd::kHdrDroppedPeriodicPm), 1u);
  EXPECT_TRUE(csd::appendPmSample(b, record, /*transitionDriven=*/true))
      << "a transition-driven sample survives a full periodic bucket";
  EXPECT_EQ(csd::dropped(b, csd::kHdrDroppedTransitionPm), 0u);
  // The start/exit heap snapshots budget separately from every other class.
  ASSERT_TRUE(csd::appendSnapshot(b, 0, 1000, 900, false));
  ASSERT_TRUE(csd::appendSnapshot(b, 1000, 990, 890, true));
  for (uint32_t i = 0; i < csd::kSnapshotCapacity; ++i) {
    (void)csd::appendSnapshot(b, i * 1000, 1000, 900, false);
  }
  EXPECT_EQ(csd::count(b, csd::kHdrSnapshotCount), csd::kSnapshotCapacity);
  EXPECT_GT(csd::dropped(b, csd::kHdrDroppedSnapshot), 0u);
}

TEST(CompanionStandbyDiag, TransitionLogOverflowSetsTheInconclusiveFlag) {
  auto& b = buffer();
  seedAdvertising(b);
  for (uint32_t i = 0; i < csd::kTransitionCapacity; ++i) {
    ASSERT_TRUE(csd::appendTransition(b, i * 1000, csd::kRadioAdvertising, csd::kRadioConnected));
  }
  EXPECT_FALSE(csd::appendTransition(b, csd::kTransitionCapacity * 1000, csd::kRadioAdvertising, csd::kRadioConnected));
  EXPECT_EQ(csd::dropped(b, csd::kHdrDroppedTransition), 1u);
  EXPECT_NE(csd::flags(b) & csd::kFlagTransitionOverflow, 0u);
  EXPECT_EQ(csd::advertisingResidencyVerdict(b), csd::Verdict::Inconclusive)
      << "an incomplete transition history must never produce a PASS";
}

TEST(CompanionStandbyDiag, AccumulationSurvivesThe32BitMicrosecondBoundary) {
  auto& b = buffer();
  seedAdvertising(b);
  // Two intervals of one hour each: 3.6e9 us per interval already exceeds
  // 2^32 (4294.97 s) — exactly the wrap the 32-bit record cannot carry. Wall
  // time rides in milliseconds and stays exact.
  const int64_t hour = 3600LL * 1000000;
  csd::accumulateInterval(b, csd::IntervalClass::Advertising, {3600000, 0, 0, hour}, true);
  csd::accumulateInterval(b, csd::IntervalClass::Advertising, {3600000, 0, 0, hour}, true);
  EXPECT_EQ(csd::sleepUsTotal(b), 2 * hour) << "the 64-bit total must not wrap";
  EXPECT_GT(csd::sleepUsTotal(b), static_cast<long long>(UINT32_MAX));
  EXPECT_EQ(csd::classMillis(b, csd::IntervalClass::Advertising), 7200000u);
}

TEST(CompanionStandbyDiag, InvalidIntervalsCarryWallTimeButNoSleep) {
  auto& b = buffer();
  seedAdvertising(b);
  csd::accumulateInterval(b, csd::IntervalClass::Advertising, {600000, 500, 0, 60000000}, false);
  EXPECT_EQ(csd::classMillis(b, csd::IntervalClass::Advertising), 600000u)
      << "an invalid span stays in its state's measurement population";
  EXPECT_EQ(csd::sleepUsTotal(b), 0) << "an unattributable span must not fabricate sleep";
  EXPECT_EQ(csd::entriesTotal(b), 0u);
  csd::accumulateInterval(b, csd::IntervalClass::Advertising, {600000, 500, 0, 60000000}, true);
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

// The original hardware failure (C2): the first advertising interval was
// shorter than one second, its wall seconds truncated to zero while its PM
// sample carried 696712 us of real sleep, and the whole-second denominators
// summed to 3 s against 3.053 s of sleep — "residency 101.76%" and a false
// PASS. With millisecond boundaries the same measurement cannot exceed 100%.
TEST(CompanionStandbyDiag, SubsecondTruncationCannotExceedHundredPercent) {
  auto& b = buffer();
  seedAdvertising(b);
  // The hardware shape: subsecond advertising spans whose whole-second
  // truncation used to drop 53 ms of wall time below the measured sleep.
  addAdvertisingSpan(b, 300, 1000, 696712);    // was "0-0": zero wall, 0.697 s sleep
  addAdvertisingSpan(b, 1000, 2300, 1250000);  // was "1-2"
  addAdvertisingSpan(b, 2300, 3450, 1106163);  // was "2-3"
  EXPECT_EQ(csd::classMillis(b, csd::IntervalClass::Advertising), 3150u)
      << "the full wall time survives; whole-second truncation would say 3000 ms";
  EXPECT_EQ(csd::sleepUsTotal(b), 3052875);
  const uint64_t residency = csd::residencyPermyriad(3052875, 3150);
  EXPECT_EQ(residency, 9691u) << "3052875us over 3.150s = 96.91%, never the impossible 101.76%";
  EXPECT_LE(residency, 10000u);
  EXPECT_EQ(csd::advertisingResidencyVerdict(b), csd::Verdict::Pass)
      << "a valid sub-100% measurement keeps its verdict";
}

TEST(CompanionStandbyDiag, MultipleTransitionsWithinOneSecondKeepTheirWalls) {
  auto& b = buffer();
  seedAdvertising(b);
  // Three state changes inside one second: whole-second accounting would
  // collapse all of them to 0; millisecond accounting separates them.
  addAdvertisingSpan(b, 0, 300, 250000);
  csd::accumulateInterval(b, csd::IntervalClass::Connected, {300, 0, 0, 280000}, true);
  csd::appendPmSample(b, {300, 600, 0, 0, 280000, csd::IntervalClass::Connected, true}, true);
  addAdvertisingSpan(b, 600, 900, 270000);
  EXPECT_EQ(csd::classMillis(b, csd::IntervalClass::Advertising), 600u);
  EXPECT_EQ(csd::classMillis(b, csd::IntervalClass::Connected), 300u);
  EXPECT_EQ(csd::sleepUsTotal(b), 250000 + 280000 + 270000);
  EXPECT_LT(csd::residencyPermyriad(520000, 600), 10000u)
      << "advertising sleep (250000+270000) over its 600 ms wall stays under 100%";
}

TEST(CompanionStandbyDiag, LongConnectedIntervalKeepsMillisecondPrecision) {
  auto& b = buffer();
  seedAdvertising(b);
  // A one-hour connected interval with fractional-second boundaries.
  csd::accumulateInterval(b, csd::IntervalClass::Connected, {3600757, 0, 0, 3456789012LL}, true);
  EXPECT_EQ(csd::classMillis(b, csd::IntervalClass::Connected), 3600757u);
  EXPECT_EQ(csd::sleepUsTotal(b), 3456789012LL);
  EXPECT_LT(csd::residencyPermyriad(3456789012ULL, 3600757u), 10000u);
}

TEST(CompanionStandbyDiag, FinalAdvertisingSpanWithFractionalBoundariesReconciles) {
  auto& b = buffer();
  seedAdvertising(b);
  // Full-session reconciliation: the class wall equals the sum of the stored
  // valid spans plus the invalid share, with no whole-second rounding loss.
  addAdvertisingSpan(b, 100, 900, 700000);
  addAdvertisingSpan(b, 900, 1500, 0, /*valid=*/false);  // invalid: wall only
  addAdvertisingSpan(b, 1500, 86399999, 85500000000LL);
  EXPECT_EQ(csd::classMillis(b, csd::IntervalClass::Advertising), 86399999u - 100u);
  uint64_t covered = 0, sleep = 0;
  csd::storedAdvertisingEvidence(b, covered, sleep);
  EXPECT_EQ(covered, (900u - 100u) + (86399999u - 1500u));
  EXPECT_EQ(sleep, 700000u + 85500000000ULL);
  EXPECT_LT(csd::advertisingCoveragePermyriad(b), 10000u) << "the invalid span lowers coverage, never inflates it";
}

TEST(CompanionStandbyDiag, ImpossibleResidencyIsInconclusiveNeverClamped) {
  // Residual sampling uncertainty at the aggregate level: more recorded
  // sleep than wall time cannot be clamped to 100% — it must be called out.
  auto& b = buffer();
  seedAdvertising(b);
  addAdvertisingSpan(b, 0, 1000, 1200000);  // 1.2 s of "sleep" in a 1 s span
  EXPECT_GT(csd::residencyPermyriad(1200000, 1000), 10000u) << "the raw ratio exceeds 100%";
  EXPECT_EQ(csd::advertisingResidencyVerdict(b), csd::Verdict::Inconclusive);
  EXPECT_EQ(csd::standbyAcceptanceVerdict(b), csd::Verdict::Inconclusive);
}

TEST(CompanionStandbyDiag, IntervalConsistencyRuleGuardsTheDevicePath) {
  // The device-side rule: a delta whose sleep exceeds its wall span is
  // sampling uncertainty and must not be measured. The C2 hardware numbers:
  // 696712 us of sleep over a ~700 ms span is consistent; over a 500 ms
  // whole-second-truncated span it was not.
  EXPECT_TRUE(csd::intervalConsistent(700, 696712));
  EXPECT_FALSE(csd::intervalConsistent(500, 696712));
  // The measured connected cycle (15.05 ms sleep in a 15.65 ms wall) stays
  // measurable; its whole-second-truncated shape (15 ms wall) would not.
  EXPECT_TRUE(csd::intervalConsistent(15650, 15050000));
  EXPECT_FALSE(csd::intervalConsistent(15000, 15050000));
  EXPECT_FALSE(csd::intervalConsistent(0, 1));
  EXPECT_TRUE(csd::intervalConsistent(1000, 0));
}

TEST(CompanionStandbyDiag, ImpossibleCoverageIsInconclusive) {
  auto& b = buffer();
  seedAdvertising(b);
  // Hand-crafted records could claim more covered time than the class wall
  // time holds; the verdict must refuse rather than divide. (The device
  // telescoping cannot produce this; the guard protects the evidence path.)
  addAdvertisingSpan(b, 0, 1000, 900000);
  csd::PmRecord extra;
  extra.startMs = 500;
  extra.endMs = 1500;
  extra.modeUsDelta = 100000;
  extra.intervalClass = csd::IntervalClass::Advertising;
  extra.samplesValid = true;
  csd::appendPmSample(b, extra, true);
  EXPECT_EQ(csd::classMillis(b, csd::IntervalClass::Advertising), 1000u);
  uint64_t covered = 0, sleep = 0;
  csd::storedAdvertisingEvidence(b, covered, sleep);
  EXPECT_EQ(covered, 2000u);
  EXPECT_GT(csd::advertisingCoveragePermyriad(b), 10000u);
  EXPECT_EQ(csd::advertisingResidencyVerdict(b), csd::Verdict::Inconclusive);
}

TEST(CompanionStandbyDiag, ResidencyThresholdIsNinetyPercent) {
  // 1000 ms wall; sleep is measured in microseconds (0.95 s = 95%).
  const uint64_t wallMs = 1000;
  EXPECT_EQ(csd::residencyPermyriad(950000, wallMs), 9500u);
  EXPECT_GE(csd::residencyPermyriad(900000, wallMs), 9000u);
  EXPECT_LT(csd::residencyPermyriad(899000, wallMs), 9000u);
  EXPECT_EQ(csd::residencyPermyriad(1000, 0), 0u) << "no wall time, no ratio";
}

TEST(CompanionStandbyDiag, VerdictBelowNinetyPercentIsAFail) {
  // 95%: pass. 89.9%: fail. Both with full coverage and a valid clock.
  auto& b = buffer();
  seedAdvertising(b);
  addAdvertisingSpan(b, 0, 1000, 950000);
  EXPECT_EQ(csd::advertisingResidencyVerdict(b), csd::Verdict::Pass);
  seedAdvertising(b);
  addAdvertisingSpan(b, 0, 1000, 899000);
  EXPECT_EQ(csd::advertisingResidencyVerdict(b), csd::Verdict::Fail);
}

TEST(CompanionStandbyDiag, MissingClockEvidenceKeepsOverallInconclusive) {
  auto& b = buffer();
  csd::reset(b);  // no startup record: the clock was never read
  csd::PmRecord record;
  record.startMs = 0;
  record.endMs = 1000;
  record.modeUsDelta = 950000;
  record.intervalClass = csd::IntervalClass::Advertising;
  record.samplesValid = true;
  csd::appendPmSample(b, record, false);
  csd::accumulateInterval(b, csd::IntervalClass::Advertising, {1000, 0, 0, 950000}, true);
  EXPECT_EQ(csd::advertisingResidencyVerdict(b), csd::Verdict::Pass) << "residency alone is still measurable";
  EXPECT_EQ(csd::standbyAcceptanceVerdict(b), csd::Verdict::Inconclusive);
}

TEST(CompanionStandbyDiag, ResidencyVerdictIsIndependentOfTheClockSource) {
  // C1 correction 1: a MAIN_XTAL fallback behind a 95% measured residency is
  // a PASS on residency alone — but the clock verdict is Invalid and the
  // overall B9 acceptance can never read as PASS.
  seedAdvertising(buffer());
  csd::recordStartup(buffer(), csd::kLpclkMainXtal, csd::kCrystalNotRecorded, 0);
  addAdvertisingSpan(buffer(), 0, 1000, 950000);
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

TEST(CompanionStandbyDiag, InsufficientCoverageIsInconclusive) {
  // Wall time 1000 ms (800 ms measured + a 200 ms invalid span) but only
  // 800 ms covered: 80% coverage < 98%.
  auto& b = buffer();
  seedAdvertising(b);
  addAdvertisingSpan(b, 0, 800, 760000);
  addAdvertisingSpan(b, 800, 1000, 0, /*valid=*/false);
  EXPECT_EQ(csd::advertisingResidencyVerdict(b), csd::Verdict::Inconclusive);
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
  csd::appendError(b, 0, csd::kErrSampleBeyondWall, 1, 1);
  csd::appendSnapshot(b, 0, 1, 1, false);
  EXPECT_EQ(csd::totalRecords(b), 6u);
}

TEST(CompanionStandbyDiag, SnapshotRecordsCarryHeapAndBoundary) {
  auto& b = buffer();
  seedAdvertising(b);
  ASSERT_TRUE(csd::appendSnapshot(b, 0, 100000, 90000, false));
  ASSERT_TRUE(csd::appendSnapshot(b, 42999, 99000, 89000, true));
  EXPECT_EQ(csd::count(b, csd::kHdrSnapshotCount), 2u);
  const uint8_t* start = b.bytes + csd::kSnapshotOffset;
  const uint8_t* exit = start + csd::kSnapshotSize;
  EXPECT_EQ(csd::loadLE32(start + 4), 100000u);
  EXPECT_EQ(start[12], 1) << "tag 1 = session start";
  EXPECT_EQ(csd::loadLE32(exit), 42999u);
  EXPECT_EQ(csd::loadLE32(exit + 8), 89000u);
  EXPECT_EQ(exit[12], 2) << "tag 2 = session exit";
}

TEST(CompanionStandbyDiag, PmRecordRoundTripsMillisecondTimestamps) {
  // The v3 field semantics must survive encoding at both ends of the u32
  // millisecond range (24 h cap and near-wrap values).
  auto& b = buffer();
  seedAdvertising(b);
  const csd::PmRecord in{86399999u, 86400500u, 7u, 1u, 15500000, csd::IntervalClass::Connected, true};
  csd::appendPmSample(b, in, false);
  const uint8_t* p = b.bytes + csd::kPeriodicPmOffset;
  EXPECT_EQ(csd::loadLE32(p), 86399999u);
  EXPECT_EQ(csd::loadLE32(p + 4), 86400500u);
  EXPECT_EQ(static_cast<uint8_t>(p[24]), static_cast<uint8_t>(csd::IntervalClass::Connected));
  EXPECT_EQ(p[25], 1);
}
