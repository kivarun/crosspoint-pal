#include <gtest/gtest.h>

#include <cstdint>

#include "util/Rtc32kDiagnosticsPolicy.h"

using rtc32k::CalQuality;
using rtc32k::CalRun;
using rtc32k::CalStatus;
using rtc32k::DiagState;
using rtc32k::SlowClkSource;

namespace {

CalRun allOk(const uint32_t (&hz)[rtc32k::CAL_SAMPLE_COUNT]) {
  CalRun run;
  for (size_t i = 0; i < rtc32k::CAL_SAMPLE_COUNT; i++) run.record(true, hz[i]);
  return run;
}

CalRun allTimeout() {
  CalRun run;
  for (size_t i = 0; i < rtc32k::CAL_SAMPLE_COUNT; i++) run.record(false, 0);
  return run;
}

}  // namespace

// --- period conversion -------------------------------------------------------

TEST(Xtal32kPeriod, ZeroPeriodStaysZero) {
  // rtc_clk_cal() returns 0 on hardware timeout / failed validity check; that
  // sentinel must never turn into a plausible-looking frequency.
  EXPECT_EQ(rtc32k::periodToHz(0), 0u);
}

TEST(Xtal32kPeriod, Nominal32768PeriodConvertsExactly) {
  // Q19.13 period of 1/32768 s = 30.517578125 us -> 16000000 counts.
  EXPECT_EQ(rtc32k::periodToHz(16000000), 32768u);
  EXPECT_EQ(rtc32k::periodToHz(32000000), 16384u);
  // A few hardware steps (~12.8 period-counts per 40 MHz XTAL cycle at 1024
  // slow-clock cycles) land on the neighbouring integer Hz readings.
  EXPECT_EQ(rtc32k::periodToHz(15999500), 32769u);
  EXPECT_EQ(rtc32k::periodToHz(16000500), 32766u);
}

// --- session state -----------------------------------------------------------

TEST(Xtal32kSession, InitialStateIsOffAndUntouched) {
  DiagState s;
  EXPECT_FALSE(s.xtalOn);
  EXPECT_FALSE(s.enabledByThisActivity);
  EXPECT_EQ(calStatus(s.cal), CalStatus::NotRun);
  EXPECT_EQ(calQuality(s.cal), CalQuality::Unknown);
  EXPECT_EQ(calMedianHz(s.cal), 0u);
  EXPECT_FALSE(rtc32k::shouldDisableOnExit(s));
}

TEST(Xtal32kSession, EnableMarksStateAndExitOwnership) {
  DiagState s;
  s.markEnabled();
  EXPECT_TRUE(s.xtalOn);
  EXPECT_TRUE(s.enabledByThisActivity);
  EXPECT_TRUE(rtc32k::shouldDisableOnExit(s));
  // Repeated enable is idempotent at the state level.
  s.markEnabled();
  EXPECT_TRUE(s.xtalOn);
  EXPECT_TRUE(rtc32k::shouldDisableOnExit(s));
}

TEST(Xtal32kSession, DisableClearsStateAndExitOwnership) {
  DiagState s;
  s.markEnabled();
  s.markDisabled();
  EXPECT_FALSE(s.xtalOn);
  EXPECT_FALSE(s.enabledByThisActivity);
  EXPECT_FALSE(rtc32k::shouldDisableOnExit(s));
}

TEST(Xtal32kSession, ExitNeverTouchesAGeneratorThisActivityDidNotEnable) {
  // Entered and left without any action: the exit path must not run
  // rtc_clk_32k_enable(false) — the state change belongs to nobody.
  DiagState s;
  EXPECT_FALSE(rtc32k::shouldDisableOnExit(s));
}

TEST(Xtal32kSession, CalibrationHistorySurvivesDisable) {
  // Disable changes the oscillator state only; the last calibration stays
  // on screen until the next Calibrate action resets the run.
  DiagState s;
  s.markEnabled();
  s.cal.record(true, 32768);
  s.markDisabled();
  EXPECT_EQ(calStatus(s.cal), CalStatus::Done);
}

TEST(Xtal32kSession, FreshCalibrationRunClearsPreviousSamples) {
  DiagState s;
  s.cal.record(true, 32768);
  s.cal.record(true, 32768);
  s.cal.beginRun();
  EXPECT_EQ(s.cal.count, 0u);
  EXPECT_EQ(calStatus(s.cal), CalStatus::NotRun);
}

// --- calibration runs --------------------------------------------------------

TEST(Xtal32kCalibration, SuccessfulRunRecordsEverySample) {
  const uint32_t nominal[rtc32k::CAL_SAMPLE_COUNT] = {32768, 32767, 32768, 32769, 32768};
  const CalRun run = allOk(nominal);
  EXPECT_TRUE(run.complete());
  EXPECT_EQ(run.okCount(), 5u);
  EXPECT_EQ(calStatus(run), CalStatus::Done);
  EXPECT_EQ(calMedianHz(run), 32768u);
  EXPECT_EQ(calQuality(run), CalQuality::Stable);
}

TEST(Xtal32kCalibration, RecordPastCapacityIsIgnored) {
  CalRun run;
  for (size_t i = 0; i < rtc32k::CAL_SAMPLE_COUNT; i++) run.record(true, 32768);
  run.record(true, 1);
  EXPECT_EQ(run.count, 5u);
}

TEST(Xtal32kCalibration, TimeoutRunShowsTimeoutNotZeroHz) {
  const CalRun run = allTimeout();
  EXPECT_TRUE(run.complete());
  EXPECT_EQ(run.okCount(), 0u);
  EXPECT_EQ(calStatus(run), CalStatus::Timeout);
  EXPECT_EQ(calQuality(run), CalQuality::Unknown);
  EXPECT_EQ(calMedianHz(run), 0u);
}

TEST(Xtal32kCalibration, PartialRunMediansTheGoodSamples) {
  CalRun run;
  run.record(true, 32768);
  run.record(false, 0);
  run.record(true, 32770);
  run.record(true, 32767);
  run.record(false, 0);
  EXPECT_EQ(calStatus(run), CalStatus::Done);
  EXPECT_EQ(run.okCount(), 3u);
  EXPECT_EQ(calMedianHz(run), 32768u);
  EXPECT_EQ(calQuality(run), CalQuality::Stable);
}

TEST(Xtal32kCalibration, WanderingSamplesAreUnstable) {
  // All samples pass the hardware validity gate (so this is a realistic
  // device-side pattern), but the readings wander far beyond crystal
  // tolerance -> the policy refuses the STABLE label.
  const uint32_t wandering[rtc32k::CAL_SAMPLE_COUNT] = {32752, 32784, 32768, 32768, 32768};
  const CalRun run = allOk(wandering);
  EXPECT_EQ(calStatus(run), CalStatus::Done);
  EXPECT_EQ(calMedianHz(run), 32768u);
  EXPECT_EQ(calQuality(run), CalQuality::Unstable);
}

TEST(Xtal32kCalibration, OutOfWindowMedianIsOutOfRange) {
  // Defense in depth: the RTC peripheral already rejects wild readings, but
  // the policy still refuses to bless a median outside the window.
  const uint32_t slow[rtc32k::CAL_SAMPLE_COUNT] = {25000, 25000, 25000, 25000, 25000};
  const CalRun run = allOk(slow);
  EXPECT_EQ(calStatus(run), CalStatus::Done);
  EXPECT_EQ(calQuality(run), CalQuality::OutOfRange);
}

// --- formatting --------------------------------------------------------------

TEST(Xtal32kFormatting, SampleRowsPerIndex) {
  CalRun run;
  run.record(true, 32768);
  run.record(false, 0);
  char buf[20];
  rtc32k::formatCalSample(buf, sizeof(buf), run, 0);
  EXPECT_STREQ(buf, "32768 Hz");
  rtc32k::formatCalSample(buf, sizeof(buf), run, 1);
  EXPECT_STREQ(buf, "TIMEOUT");
  rtc32k::formatCalSample(buf, sizeof(buf), run, 2);
  EXPECT_STREQ(buf, "-");
  rtc32k::formatCalSample(buf, sizeof(buf), run, 99);
  EXPECT_STREQ(buf, "-");
}

TEST(Xtal32kFormatting, MedianLineCoversAllStates) {
  char buf[24];
  rtc32k::formatCalMedian(buf, sizeof(buf), CalRun{});
  EXPECT_STREQ(buf, "NOT RUN");
  rtc32k::formatCalMedian(buf, sizeof(buf), allTimeout());
  EXPECT_STREQ(buf, "TIMEOUT");
  const uint32_t nominal[rtc32k::CAL_SAMPLE_COUNT] = {32768, 32767, 32768, 32769, 32768};
  rtc32k::formatCalMedian(buf, sizeof(buf), allOk(nominal));
  EXPECT_STREQ(buf, "32768 Hz");
}

TEST(Xtal32kFormatting, ResultLabelsNeverClaimAPassOnFailure) {
  EXPECT_STREQ(rtc32k::resultLabel(CalRun{}), "NOT RUN");
  EXPECT_STREQ(rtc32k::resultLabel(allTimeout()), "TIMEOUT - NO CLOCK");
  const uint32_t nominal[rtc32k::CAL_SAMPLE_COUNT] = {32768, 32768, 32768, 32768, 32768};
  EXPECT_STREQ(rtc32k::resultLabel(allOk(nominal)), "STABLE");
  const uint32_t wandering[rtc32k::CAL_SAMPLE_COUNT] = {32752, 32784, 32768, 32768, 32768};
  EXPECT_STREQ(rtc32k::resultLabel(allOk(wandering)), "UNSTABLE");
}

TEST(Xtal32kFormatting, StateAndSourceLabels) {
  EXPECT_STREQ(rtc32k::stateLabel(true), "ON");
  EXPECT_STREQ(rtc32k::stateLabel(false), "OFF");
  EXPECT_STREQ(rtc32k::slowClockSourceName(SlowClkSource::RcSlow), "RC SLOW");
  EXPECT_STREQ(rtc32k::slowClockSourceName(SlowClkSource::Xtal32k), "XTAL32K");
  EXPECT_STREQ(rtc32k::slowClockSourceName(SlowClkSource::RcFastD256), "RC FAST D256");
  EXPECT_STREQ(rtc32k::slowClockSourceName(SlowClkSource::Invalid), "UNKNOWN");
}

// --- expected technician flow ------------------------------------------------

TEST(Xtal32kSession, TechnicianFlowEndsClean) {
  DiagState s;
  // Enable -> the generator runs and the exit path owns the undo.
  s.markEnabled();
  EXPECT_TRUE(rtc32k::shouldDisableOnExit(s));
  // Calibrate -> five good samples, stable.
  const uint32_t nominal[rtc32k::CAL_SAMPLE_COUNT] = {32768, 32768, 32767, 32768, 32768};
  for (size_t i = 0; i < rtc32k::CAL_SAMPLE_COUNT; i++) s.cal.record(true, nominal[i]);
  EXPECT_EQ(calQuality(s.cal), CalQuality::Stable);
  // Disable -> the screen goes OFF and the exit path owes nothing anymore.
  s.markDisabled();
  EXPECT_FALSE(s.xtalOn);
  EXPECT_FALSE(rtc32k::shouldDisableOnExit(s));
}
