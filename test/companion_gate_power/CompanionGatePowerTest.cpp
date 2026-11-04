#include <gtest/gtest.h>

#include "util/CompanionGatePower.h"

// The pure core of CompanionGatePower: the PM-dump parse, the window
// accounting, and the RTC record layout. The device side (esp_pm configure,
// the dump capture, the serial command) runs only in the experimental env.

namespace {

using companiongatepower::GatePowerRecord;
using companiongatepower::kGatePowerMagic;
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
  static_assert(sizeof(GatePowerRecord) == 32, "RTC budget");
  EXPECT_EQ(offsetof(GatePowerRecord, magic), 0u);
  EXPECT_EQ(offsetof(GatePowerRecord, lastBleState), 28u);
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

TEST(GateAccounting, FirstSamplePrimesWithoutCounting) {
  GatePowerRecord record = fresh();
  SleepSample sample;
  ASSERT_TRUE(companiongatepower::parseSleepStats(kDump, sample));
  WindowDelta delta;
  EXPECT_FALSE(companiongatepower::windowDelta(record, sample, delta));
  companiongatepower::commitWindow(record, sample, companiongatepower::SampleRole::Prime, delta, 1,
                                   companiongatepower::WindowTimeout);
  EXPECT_EQ(record.windows, 0u);
  EXPECT_EQ(record.entries, 0u);
  EXPECT_EQ(record.primed, 1u);
  EXPECT_EQ(record.lastEntries, 1u);
  EXPECT_EQ(record.lastModeUs, 5545u);
}

TEST(GateAccounting, SecondSampleCountsTheDelta) {
  GatePowerRecord record = fresh();
  SleepSample first;
  ASSERT_TRUE(companiongatepower::parseSleepStats(kDump, first));
  WindowDelta delta;
  companiongatepower::commitWindow(record, first, companiongatepower::SampleRole::Prime, delta, 1,
                                   companiongatepower::WindowTimeout);

  constexpr const char* kLater =
      "\nSleep stats:\n"
      "light_sleep_counts:9  light_sleep_reject_counts:2\n"
      "\nSLEEP     40 M         12000        0%\n";
  SleepSample second;
  ASSERT_TRUE(companiongatepower::parseSleepStats(kLater, second));
  EXPECT_TRUE(companiongatepower::windowDelta(record, second, delta));
  EXPECT_EQ(delta.entries, 8);
  EXPECT_EQ(delta.rejects, 2);
  EXPECT_EQ(delta.modeUs, 12000 - 5545);
  companiongatepower::commitWindow(record, second, companiongatepower::SampleRole::Count, delta, 3,
                                   companiongatepower::WindowPowerAbort);
  EXPECT_EQ(record.windows, 1u);
  EXPECT_EQ(record.entries, 8u);
  EXPECT_EQ(record.rejects, 2u);
  EXPECT_EQ(record.lastBleState, 3u);
  EXPECT_EQ(record.lastOutcome, companiongatepower::WindowPowerAbort);
  EXPECT_EQ(record.noSleepRuns, 0u);
}

TEST(GateAccounting, BackwardsCounterIsSkipped) {
  GatePowerRecord record = fresh();
  record.primed = 1;
  record.lastEntries = 100;
  record.lastRejects = 1;
  SleepSample backwards;
  backwards.entries = 4;  // RTC garbage or a wrap: less than the baseline
  backwards.rejects = 5;
  backwards.modeUs = 1000;
  WindowDelta delta;
  EXPECT_FALSE(companiongatepower::windowDelta(record, backwards, delta));
  companiongatepower::commitWindow(record, backwards, companiongatepower::SampleRole::Skip, delta, 1,
                                   companiongatepower::WindowTimeout);
  EXPECT_EQ(record.lastEntries, 100u);  // the baseline stays as it was
  EXPECT_EQ(record.windows, 0u);
}

TEST(GateAccounting, NoSleepWindowIsCounted) {
  GatePowerRecord record = fresh();
  record.primed = 1;
  record.lastEntries = 10;
  record.lastRejects = 0;
  SleepSample same;
  same.entries = 10;
  same.rejects = 0;
  same.modeUs = 500;
  WindowDelta delta;
  EXPECT_TRUE(companiongatepower::windowDelta(record, same, delta));
  companiongatepower::commitWindow(record, same, companiongatepower::SampleRole::Count, delta, 1,
                                   companiongatepower::WindowTimeout);
  EXPECT_EQ(record.windows, 1u);
  EXPECT_EQ(record.noSleepRuns, 1u);
  record.noSleepRuns = 0xFF;  // saturates instead of wrapping
  companiongatepower::commitWindow(record, same, companiongatepower::SampleRole::Count, delta, 1,
                                   companiongatepower::WindowTimeout);
  EXPECT_EQ(record.noSleepRuns, 0xFFu);
}

TEST(GateAccounting, ModeTimeBaselineIsThirtyTwoBit) {
  GatePowerRecord record = fresh();
  record.primed = 1;
  record.lastModeUs = 4000000000u;  // wraps after ~71.6 min of accumulated sleep
  SleepSample later;
  later.entries = 1;
  later.rejects = 0;
  later.modeUs = 4000000000u + 6000;
  WindowDelta delta;
  EXPECT_TRUE(companiongatepower::windowDelta(record, later, delta));
  EXPECT_EQ(delta.modeUs, 6000);
  // A wrap-suspected mode delta contributes nothing but still counts entries.
  SleepSample wrapped;
  wrapped.entries = 2;
  wrapped.rejects = 0;
  wrapped.modeUs = 1000;
  EXPECT_TRUE(companiongatepower::windowDelta(record, wrapped, delta));
  EXPECT_EQ(delta.modeUs, 0);
}

TEST(GatePowerPoll, CadenceFollowsTheWindow) {
  EXPECT_EQ(companiongatepower::gatePollMs(true), 15u);
  EXPECT_EQ(companiongatepower::gatePollMs(false), 2u);
}
