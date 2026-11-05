#include <gtest/gtest.h>

#include <string>

#include "util/CompanionGatePower.h"

// The pure core of CompanionGatePower: the PM-dump parse, the bounded capture
// buffer, the window accounting, and the RTC record layout. The device side
// (esp_pm configure, the dump capture, the serial command) runs only in the
// experimental env.

namespace {

using companiongatepower::Capture;
using companiongatepower::GatePowerRecord;
using companiongatepower::kCaptureCapacity;
using companiongatepower::kGatePowerMagic;
using companiongatepower::kGatePowerMagicV1;
using companiongatepower::SleepSample;
using companiongatepower::WindowDelta;

// Shaped after pm_locks.c / pm_impl.c (WITH_PROFILING): the SLEEP mode row
// carries the 40 MHz clock as two tokens ("40 M"), CPU_MAX as one ("240M").
constexpr const char* kDump =
    "Time since boot up: 1234567 us\n"
    "\n"
    "Lock stats:\n"
    "Name            Type           Arg    Active    Total_count    Time(us)       Time(%)\n"
    "rtos0           CPU_FREQ_MAX   0      0             12             4995898    99%\n"
    "\n"
    "Mode stats:\n"
    "Mode      CPU_freq    Time(us)    Time(%)\n"
    "CPU_MAX   240M         4995898       97%\n"
    "SLEEP     40 M          5545        0%\n"
    "\n"
    "Sleep stats:\n"
    "light_sleep_counts:1  light_sleep_reject_counts:0\n";

constexpr const char* kNoSleepRow =
    "Time since boot up: 1234567 us\n"
    "\n"
    "Mode stats:\n"
    "Mode      CPU_freq    Time(us)    Time(%)\n"
    "CPU_MAX   240M         4995898       97%\n"
    "\n"
    "Sleep stats:\n"
    "light_sleep_counts:0  light_sleep_reject_counts:0\n";

GatePowerRecord fresh() {
  GatePowerRecord record{};
  record.magic = kGatePowerMagic;
  return record;
}

}  // namespace

TEST(GatePowerRecord, LayoutIsPinned) {
  static_assert(sizeof(GatePowerRecord) == 28, "RTC budget");
  EXPECT_EQ(offsetof(GatePowerRecord, magic), 0u);
  EXPECT_EQ(offsetof(GatePowerRecord, lastBleState), 24u);
}

TEST(GatePowerRecord, GarbageFailsClosed) {
  GatePowerRecord record = fresh();
  EXPECT_TRUE(companiongatepower::recordValid(record));
  record.magic = 0;
  EXPECT_FALSE(companiongatepower::recordValid(record));
  record = fresh();
  record.lastOutcome = 7;
  EXPECT_TRUE(companiongatepower::recordValid(record));  // a garbage byte in a data field reads as data
}

TEST(GatePowerRecord, OldLayoutRecordIsReset) {
  // The regression this test pins: the pre-B2h 32-byte 'GAPW' record shared
  // the old magic, so reinterpreting it as the 28-byte 'GAP2' record seeded
  // noSleepRuns and sleepUs with the old cross-boot baseline — garbage no
  // commit ever corrects. The retired magic must fail recordValid so
  // ensureValidRecord resets the diagnostics instead.
  GatePowerRecord old = fresh();
  old.magic = kGatePowerMagicV1;
  old.windows = 1;
  old.entries = 4;
  old.rejects = 2;
  old.noSleepRuns = 0x3039;  // the old lastEntries lands here under reinterpretation
  old.sleepUs = 0x2710;      // the old lastRejects lands here under reinterpretation
  old.lastBleState = 3;
  old.lastOutcome = 1;
  EXPECT_NE(kGatePowerMagicV1, kGatePowerMagic);
  EXPECT_FALSE(companiongatepower::recordValid(old));
  EXPECT_TRUE(companiongatepower::recordValid(fresh()));
}

TEST(ParseSleepStats, ReadsCountsAndSleepModeTime) {
  SleepSample sample;
  ASSERT_TRUE(companiongatepower::parseSleepStats(kDump, sample));
  EXPECT_EQ(sample.entries, 1);
  EXPECT_EQ(sample.rejects, 0);
  EXPECT_EQ(sample.modeUs, 5545);
}

TEST(ParseSleepStats, SleepRowWithoutSingleTokenFrequency) {
  SleepSample sample;
  ASSERT_TRUE(
      companiongatepower::parseSleepStats("\nSLEEP     240M      7000        0%\n"
                                          "\nSleep stats:\n"
                                          "light_sleep_counts:0  light_sleep_reject_counts:0\n",
                                          sample));
  EXPECT_EQ(sample.entries, 0);
  EXPECT_EQ(sample.modeUs, 7000);
}

TEST(ParseSleepStats, MissingSleepRowStillCarriesCounts) {
  SleepSample sample;
  ASSERT_TRUE(companiongatepower::parseSleepStats(kNoSleepRow, sample));
  EXPECT_EQ(sample.entries, 0);
  EXPECT_EQ(sample.rejects, 0);
  EXPECT_EQ(sample.modeUs, -1);
}

TEST(ParseSleepStats, MissingSleepStatsLineFails) {
  SleepSample sample;
  EXPECT_FALSE(companiongatepower::parseSleepStats("Mode stats:\nSLEEP     40 M   100   1%\n", sample));
  EXPECT_FALSE(companiongatepower::parseSleepStats(nullptr, sample));
}

namespace {
Capture filledCapture(const char* text) {
  Capture capture;
  companiongatepower::captureReset(capture);
  companiongatepower::captureAppend(capture, text, strlen(text));
  return capture;
}
}  // namespace

TEST(PmDumpCapture, RepeatedCapturesLeaveNoStaleTail) {
  // The regression this test pins, in the shape the defect happens: ONE
  // capture buffer, a longer capture first, then a shorter one. The buffer is
  // read with strstr/sscanf and the terminator is what stops the parser; the
  // un-terminated old captureWrite let it scan past the new content into the
  // previous capture's stale tail — the kDump SLEEP mode row that kNoSleepRow
  // does not have — and report the previous window's mode time.
  Capture capture;
  companiongatepower::captureReset(capture);
  companiongatepower::captureAppend(capture, kDump, strlen(kDump));  // entries=1, modeUs=5545
  EXPECT_FALSE(capture.truncated);
  companiongatepower::captureReset(capture);
  companiongatepower::captureAppend(capture, kNoSleepRow, strlen(kNoSleepRow));  // shorter, no SLEEP row
  EXPECT_EQ(capture.buf[capture.len], '\0');
  EXPECT_EQ(capture.len, strlen(capture.buf));
  SleepSample sample;
  ASSERT_TRUE(companiongatepower::parseSleepStats(capture.buf, sample));
  EXPECT_EQ(sample.entries, 0);
  EXPECT_EQ(sample.rejects, 0);
  EXPECT_EQ(sample.modeUs, -1);  // the stale kDump SLEEP row must stay invisible
}

TEST(PmDumpCapture, ExactCapacityIsNotTruncated) {
  // A dump that fills the buffer exactly dropped nothing: the terminator
  // takes the reserved byte and the capture is not flagged truncated.
  Capture capture;
  companiongatepower::captureReset(capture);
  const std::string dump(kCaptureCapacity, 'x');
  companiongatepower::captureAppend(capture, dump.data(), dump.size());
  EXPECT_FALSE(capture.truncated);
  EXPECT_EQ(capture.len, kCaptureCapacity);
  EXPECT_EQ(capture.buf[capture.len], '\0');
}

TEST(PmDumpCapture, OverflowMarksTruncatedAndStaysTerminated) {
  Capture capture = filledCapture(kDump);
  const std::string more(kCaptureCapacity + 16, 'y');
  companiongatepower::captureAppend(capture, more.data(), more.size());
  EXPECT_TRUE(capture.truncated);
  EXPECT_EQ(capture.len, kCaptureCapacity);
  EXPECT_EQ(capture.buf[capture.len], '\0');
}

TEST(PmDumpCapture, ResetClearsStaleContent) {
  // A reset must terminate at offset 0: an empty dump after a longer one
  // must not leave the previous capture visible to the parser.
  const Capture capture = filledCapture(kDump);
  EXPECT_EQ(capture.len, strlen(kDump));
  Capture reset = capture;
  companiongatepower::captureReset(reset);
  EXPECT_EQ(reset.len, 0u);
  EXPECT_EQ(reset.buf[0], '\0');
  SleepSample sample;
  EXPECT_FALSE(companiongatepower::parseSleepStats(reset.buf, sample));
}

TEST(GateAccounting, PerBootDeltaCountsOneWindow) {
  // The window is measured against the sample captured at window entry, both
  // from the same boot: the entry sample establishes nothing in the record.
  GatePowerRecord record = fresh();
  SleepSample entry;
  ASSERT_TRUE(companiongatepower::parseSleepStats(kDump, entry));  // entries=1 rejects=0 modeUs=5545
  WindowDelta delta;
  constexpr const char* kLater =
      "\nSleep stats:\n"
      "light_sleep_counts:9  light_sleep_reject_counts:2\n"
      "\nSLEEP     40 M         12000        0%\n";
  SleepSample exit;
  ASSERT_TRUE(companiongatepower::parseSleepStats(kLater, exit));
  ASSERT_TRUE(companiongatepower::windowDelta(entry, exit, delta));
  EXPECT_EQ(delta.entries, 8);
  EXPECT_EQ(delta.rejects, 2);
  EXPECT_EQ(delta.modeUs, 12000 - 5545);
  companiongatepower::commitWindow(record, delta, true, 3, companiongatepower::WindowPowerAbort);
  EXPECT_EQ(record.windows, 1u);
  EXPECT_EQ(record.sleepUs, 12000u - 5545u);
  EXPECT_EQ(record.entries, 8u);
  EXPECT_EQ(record.rejects, 2u);
  EXPECT_EQ(record.lastBleState, 3u);
  EXPECT_EQ(record.lastOutcome, companiongatepower::WindowPowerAbort);
  EXPECT_EQ(record.noSleepRuns, 0u);
}

TEST(GateAccounting, TwoWindowsAcrossDeepSleepBootsStayIndependent) {
  // The defect this test pins: the old accounting subtracted the previous
  // boot's stored counters from the current boot's boot-relative sample. With
  // the per-boot baseline the second boot's delta is independent of whatever
  // the record accumulated before.
  GatePowerRecord record = fresh();
  SleepSample boot1Entry;
  boot1Entry.entries = 0;
  boot1Entry.rejects = 0;
  boot1Entry.modeUs = 0;
  SleepSample boot1Exit;
  boot1Exit.entries = 5;
  boot1Exit.rejects = 2;
  boot1Exit.modeUs = 40000;
  WindowDelta delta;
  ASSERT_TRUE(companiongatepower::windowDelta(boot1Entry, boot1Exit, delta));
  companiongatepower::commitWindow(record, delta, true, 1, companiongatepower::WindowTimeout);
  EXPECT_EQ(record.windows, 1u);
  EXPECT_EQ(record.entries, 5u);
  EXPECT_EQ(record.rejects, 2u);

  // The device deep-sleeps and reboots: the record persists, the esp_pm
  // counters restart near zero.
  SleepSample boot2Entry;
  boot2Entry.entries = 1;
  boot2Entry.rejects = 0;
  boot2Entry.modeUs = 3000;
  SleepSample boot2Exit;
  boot2Exit.entries = 7;
  boot2Exit.rejects = 3;
  boot2Exit.modeUs = 44000;
  ASSERT_TRUE(companiongatepower::windowDelta(boot2Entry, boot2Exit, delta));
  EXPECT_EQ(delta.entries, 6);
  EXPECT_EQ(delta.rejects, 3);
  EXPECT_EQ(delta.modeUs, 41000);
  companiongatepower::commitWindow(record, delta, true, 2, companiongatepower::WindowTimeout);
  EXPECT_EQ(record.windows, 2u);
  EXPECT_EQ(record.entries, 11u);
  EXPECT_EQ(record.rejects, 5u);
  EXPECT_EQ(record.sleepUs, 40000u + 41000u);  // accumulated across boots, per-window deltas only
}

TEST(GateAccounting, BackwardsCounterIsSkipped) {
  // A counter going backwards inside one boot is a wrap or garbage: the
  // window contributes nothing, the end state is still recorded.
  SleepSample entry;
  entry.entries = 100;
  entry.rejects = 1;
  entry.modeUs = 90000;
  SleepSample backwards;
  backwards.entries = 4;
  backwards.rejects = 5;
  backwards.modeUs = 1000;
  WindowDelta delta;
  EXPECT_FALSE(companiongatepower::windowDelta(entry, backwards, delta));
  GatePowerRecord record = fresh();
  companiongatepower::commitWindow(record, delta, false, 1, companiongatepower::WindowTimeout);
  EXPECT_EQ(record.windows, 0u);
  EXPECT_EQ(record.entries, 0u);
  EXPECT_EQ(record.rejects, 0u);
  EXPECT_EQ(record.lastBleState, 1u);
  EXPECT_EQ(record.lastOutcome, companiongatepower::WindowTimeout);
}

TEST(GateAccounting, FailedSamplesAreNotCounted) {
  // A missing entry or exit sample (dump failure) records the end state only.
  SleepSample entry;
  entry.entries = 3;
  entry.rejects = 0;
  entry.modeUs = 100;
  WindowDelta delta;
  ASSERT_TRUE(companiongatepower::windowDelta(entry, entry, delta));
  GatePowerRecord record = fresh();
  companiongatepower::commitWindow(record, delta, false, 1, companiongatepower::WindowTimeout);
  EXPECT_EQ(record.windows, 0u);
  EXPECT_EQ(record.lastOutcome, companiongatepower::WindowTimeout);
}

TEST(GateAccounting, NoSleepWindowIsCounted) {
  // A counted window with neither entry nor reject is flagged as no-sleep.
  GatePowerRecord record = fresh();
  SleepSample same;
  same.entries = 10;
  same.rejects = 0;
  same.modeUs = 500;
  WindowDelta delta;
  ASSERT_TRUE(companiongatepower::windowDelta(same, same, delta));
  companiongatepower::commitWindow(record, delta, true, 1, companiongatepower::WindowTimeout);
  EXPECT_EQ(record.windows, 1u);
  EXPECT_EQ(record.noSleepRuns, 1u);
  companiongatepower::commitWindow(record, delta, true, 1, companiongatepower::WindowTimeout);
  EXPECT_EQ(record.noSleepRuns, 2u);
}

TEST(GateAccounting, ControlWindowIsNeverCounted) {
  // The P1-B3 control build runs the identical window flow with light sleep
  // off by design: its zero delta must not accumulate into the sleep totals
  // (that would be indistinguishable from the experimental build's "sleep
  // never engaged" signal), and a metrics failure must not count either way.
  WindowDelta delta;
  delta.entries = 0;
  delta.rejects = 0;
  delta.modeUs = 0;
  EXPECT_FALSE(companiongatepower::windowCounts(false, true, true, true));
  EXPECT_FALSE(companiongatepower::windowCounts(false, false, true, true));
  EXPECT_FALSE(companiongatepower::windowCounts(false, true, false, false));
  GatePowerRecord record = fresh();
  companiongatepower::commitWindow(record, delta, companiongatepower::windowCounts(false, true, true, true), 3,
                                   companiongatepower::WindowTimeout);
  EXPECT_EQ(record.windows, 0u);
  EXPECT_EQ(record.noSleepRuns, 0u);
  EXPECT_EQ(record.lastBleState, 3u);  // the end state is still recorded
  EXPECT_EQ(record.lastOutcome, companiongatepower::WindowTimeout);
  EXPECT_TRUE(companiongatepower::windowCounts(true, true, true, true));
  EXPECT_FALSE(companiongatepower::windowCounts(true, false, true, true));
  EXPECT_FALSE(companiongatepower::windowCounts(true, true, false, true));
  EXPECT_FALSE(companiongatepower::windowCounts(true, true, true, false));
}

TEST(GateAccounting, ModeTimeWrapContributesNothing) {
  // A wrap-suspected mode delta contributes nothing but still counts entries.
  SleepSample entry;
  entry.entries = 1;
  entry.rejects = 0;
  entry.modeUs = 4000000000;  // wraps after ~71.6 min of accumulated sleep
  SleepSample later;
  later.entries = 2;
  later.rejects = 0;
  later.modeUs = entry.modeUs + 6000;
  WindowDelta delta;
  EXPECT_TRUE(companiongatepower::windowDelta(entry, later, delta));
  EXPECT_EQ(delta.entries, 1);
  EXPECT_EQ(delta.modeUs, 6000);
  SleepSample wrapped;
  wrapped.entries = 3;
  wrapped.rejects = 0;
  wrapped.modeUs = 1000;
  EXPECT_TRUE(companiongatepower::windowDelta(entry, wrapped, delta));
  EXPECT_EQ(delta.modeUs, 0);
  EXPECT_EQ(delta.entries, 2);
}

TEST(GatePowerPoll, CadenceFollowsTheWindow) {
  EXPECT_EQ(companiongatepower::gatePollMs(true), 15u);
  EXPECT_EQ(companiongatepower::gatePollMs(false), 2u);
}
