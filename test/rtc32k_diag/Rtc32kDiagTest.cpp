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

CalRun allFailed() {
  CalRun run;
  for (size_t i = 0; i < rtc32k::CAL_SAMPLE_COUNT; i++) run.record(false, 0);
  return run;
}

}  // namespace

// --- period conversion -------------------------------------------------------

TEST(Xtal32kPeriod, ZeroPeriodStaysZero) {
  // rtc_clk_cal() returns 0 on hardware timeout AND on validity-gate
  // rejection; that sentinel must never turn into a plausible-looking
  // frequency.
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

// --- ownership: entry state, Enable, Disable, exit (cases A, B, C, D, I) -----

// A (corrective). Initial hardware ON, not owned, slow source RC: Disable must
// not touch the hardware, and the exit path owes nothing.
TEST(Xtal32kOwnership, InitialOnDisableIsBlockedAndExitDoesNothing) {
  DiagState s = DiagState::initial(true);
  EXPECT_FALSE(s.ownedByActivity);
  EXPECT_FALSE(rtc32k::mayDisable(s, false));
  EXPECT_STREQ(rtc32k::disableRowValue(s, false), "N/A - PRE-ENABLED");
  EXPECT_FALSE(rtc32k::shouldDisableOnExit(s, false));
}

// B (corrective). Initial OFF, activity enables with confirmed readback,
// slow source RC: Disable is allowed.
TEST(Xtal32kOwnership, OwnedEnableAllowsDisable) {
  DiagState s = DiagState::initial(false);
  s.applyEnableResult(true);
  EXPECT_TRUE(s.ownedByActivity);
  EXPECT_TRUE(rtc32k::mayDisable(s, false));
  EXPECT_STREQ(rtc32k::disableRowValue(s, false), "");
}

// C (corrective). Owned enable, but the slow clock source is (or moved to)
// XTAL32K: Disable must not touch the hardware.
TEST(Xtal32kOwnership, OwnedEnableStillBlockedBySystemSlowClock) {
  DiagState s = DiagState::initial(false);
  s.applyEnableResult(true);
  EXPECT_TRUE(s.ownedByActivity);
  EXPECT_FALSE(rtc32k::mayDisable(s, true));
  EXPECT_STREQ(rtc32k::disableRowValue(s, true), "N/A - SYSTEM CLOCK");
  EXPECT_FALSE(rtc32k::shouldDisableOnExit(s, true));
}

// D (corrective). Entry snapshot OFF, but the live readback taken before the
// Enable request is already ON: no enable hardware call happens, the observed
// state is adopted, and ownership stays empty.
TEST(Xtal32kOwnership, LiveReadbackOnBeforeEnableAdoptsWithoutOwnership) {
  DiagState s = DiagState::initial(false);
  s.observeEnabled(true);  // what doEnable() does instead of the hardware call
  EXPECT_TRUE(s.xtalEnabled);
  EXPECT_FALSE(s.ownedByActivity);
  EXPECT_FALSE(rtc32k::mayDisable(s, false));
  EXPECT_STREQ(rtc32k::disableRowValue(s, false), "N/A - PRE-ENABLED");
  EXPECT_FALSE(rtc32k::shouldDisableOnExit(s, false));
}

// Initial OFF -> Enable -> owned -> exit requires disable.
TEST(Xtal32kOwnership, InitialOffEnableClaimsOwnershipAndExitUndoes) {
  DiagState s = DiagState::initial(false);
  EXPECT_FALSE(s.xtalEnabled);
  // Enable action: hardware call, then the rtc_clk_32k_enabled() readback.
  s.applyEnableResult(true);
  EXPECT_TRUE(s.xtalEnabled);
  EXPECT_TRUE(s.ownedByActivity);
  EXPECT_TRUE(rtc32k::shouldDisableOnExit(s, false));
  // Still owed at exit even after a calibration ran in between.
  s.cal.record(true, 32768);
  EXPECT_TRUE(rtc32k::shouldDisableOnExit(s, false));
}

// Initial hardware ON -> Enable is a no-op and claims NOTHING; exit must not
// disable.
TEST(Xtal32kOwnership, InitialOnEnableIsNoOpAndNeverOwned) {
  DiagState s = DiagState::initial(true);
  EXPECT_TRUE(s.xtalEnabled);
  // The live readback before the action reports ENABLED: the request stops
  // there, the state is re-adopted, ownership stays unclaimed.
  s.observeEnabled(true);
  EXPECT_TRUE(s.xtalEnabled);
  EXPECT_FALSE(s.ownedByActivity);
  EXPECT_FALSE(rtc32k::mayDisable(s, false));
  EXPECT_FALSE(rtc32k::shouldDisableOnExit(s, false));
}

// Generator feeding the system slow clock: no disable may run, neither from
// the button nor from the exit path — ownership alone is not sufficient.
TEST(Xtal32kOwnership, SystemSlowClockBlocksDisableAndExit) {
  DiagState s = DiagState::initial(true);  // generator feeds the system slow clock
  EXPECT_FALSE(rtc32k::mayDisable(s, true));
  EXPECT_FALSE(rtc32k::shouldDisableOnExit(s, true));
  EXPECT_STREQ(rtc32k::disableRowValue(s, true), "N/A - SYSTEM CLOCK");
}

// Slow source changed to XTAL32K after an owned enable -> exit must not
// disable (live re-check, not the entry snapshot).
TEST(Xtal32kOwnership, SlowClockMovedToXtal32kBeforeExitBlocksDisable) {
  DiagState s = DiagState::initial(false);
  s.applyEnableResult(true);
  EXPECT_TRUE(rtc32k::shouldDisableOnExit(s, false));  // guard would allow...
  EXPECT_FALSE(rtc32k::shouldDisableOnExit(s, true));  // ...until the source changed
}

// I. Enable/Disable state tracks the hardware readback, not the request.
TEST(Xtal32kOwnership, StateTracksHardwareReadbackNotRequest) {
  DiagState s = DiagState::initial(false);
  // Enable requested, readback still DISABLED: nothing enabled, nothing owned.
  s.applyEnableResult(false);
  EXPECT_FALSE(s.xtalEnabled);
  EXPECT_FALSE(s.ownedByActivity);
  EXPECT_FALSE(rtc32k::shouldDisableOnExit(s, false));
  // Enable requested, readback ENABLED: owned.
  s.applyEnableResult(true);
  EXPECT_TRUE(s.xtalEnabled);
  EXPECT_TRUE(s.ownedByActivity);
  // Disable requested, readback DISABLED: state and ownership released.
  s.applyDisableResult(false);
  EXPECT_FALSE(s.xtalEnabled);
  EXPECT_FALSE(s.ownedByActivity);
  EXPECT_FALSE(rtc32k::shouldDisableOnExit(s, false));
  // Disable requested, readback still ENABLED (failed disable): the state
  // stays honest and ownership persists so the exit path retries the undo.
  s.applyEnableResult(true);
  s.applyDisableResult(true);
  EXPECT_TRUE(s.xtalEnabled);
  EXPECT_TRUE(s.ownedByActivity);
  EXPECT_TRUE(rtc32k::shouldDisableOnExit(s, false));
}

TEST(Xtal32kOwnership, DisableNeverStripsOwnershipWhileHardwareStillEnabled) {
  DiagState s = DiagState::initial(false);
  s.applyEnableResult(true);
  s.applyDisableResult(true);  // hardware kept running
  EXPECT_TRUE(rtc32k::shouldDisableOnExit(s, false));
}

// An observation of DISABLED (whatever caused it) also releases ownership:
// there is nothing left to undo.
TEST(Xtal32kOwnership, ObservedDisabledReleasesOwnership) {
  DiagState s = DiagState::initial(false);
  s.applyEnableResult(true);
  s.observeEnabled(false);
  EXPECT_FALSE(s.xtalEnabled);
  EXPECT_FALSE(s.ownedByActivity);
  EXPECT_FALSE(rtc32k::shouldDisableOnExit(s, false));
}

// --- calibration runs --------------------------------------------------------

// E. 5/5 valid, median in window, samples agree -> STABLE.
TEST(Xtal32kCalibration, AllValidAndAgreeingIsStable) {
  const uint32_t nominal[rtc32k::CAL_SAMPLE_COUNT] = {32768, 32767, 32768, 32769, 32768};
  const CalRun run = allOk(nominal);
  EXPECT_TRUE(run.complete());
  EXPECT_EQ(run.okCount(), 5u);
  EXPECT_EQ(calStatus(run), CalStatus::Done);
  EXPECT_EQ(calMedianHz(run), 32768u);
  EXPECT_EQ(calQuality(run), CalQuality::Stable);
  EXPECT_STREQ(rtc32k::resultLabel(run), "STABLE");
}

TEST(Xtal32kCalibration, RecordPastCapacityIsIgnored) {
  CalRun run;
  for (size_t i = 0; i < rtc32k::CAL_SAMPLE_COUNT; i++) run.record(true, 32768);
  run.record(true, 1);
  EXPECT_EQ(run.count, 5u);
}

// H. 0/5 valid -> NO VALID 32K (never a frequency, never a "no clock" claim —
// rtc_clk_cal() does not distinguish timeout from validity rejection).
TEST(Xtal32kCalibration, AllFailedRunIsNoValid32k) {
  const CalRun run = allFailed();
  EXPECT_TRUE(run.complete());
  EXPECT_EQ(run.okCount(), 0u);
  EXPECT_EQ(calStatus(run), CalStatus::NoValid);
  EXPECT_EQ(calQuality(run), CalQuality::Unknown);
  EXPECT_EQ(calMedianHz(run), 0u);
  EXPECT_STREQ(rtc32k::resultLabel(run), "NO VALID 32K");
}

// F. 4/5 valid + 1 fail -> NOT STABLE (PARTIAL).
TEST(Xtal32kCalibration, FourOfFiveValidIsNeverStable) {
  CalRun run;
  run.record(true, 32768);
  run.record(true, 32768);
  run.record(true, 32768);
  run.record(true, 32768);
  run.record(false, 0);
  EXPECT_EQ(calStatus(run), CalStatus::Done);
  EXPECT_EQ(run.okCount(), 4u);
  EXPECT_EQ(calQuality(run), CalQuality::Partial);
  EXPECT_NE(calQuality(run), CalQuality::Stable);
  EXPECT_STREQ(rtc32k::resultLabel(run), "PARTIAL");
}

// G. 1/5 valid + 4 fail -> NOT STABLE (PARTIAL).
TEST(Xtal32kCalibration, OneOfFiveValidIsNeverStable) {
  CalRun run;
  run.record(true, 32768);
  for (size_t i = 0; i < 4; i++) run.record(false, 0);
  EXPECT_EQ(calStatus(run), CalStatus::Done);
  EXPECT_EQ(run.okCount(), 1u);
  EXPECT_EQ(calQuality(run), CalQuality::Partial);
  EXPECT_NE(calQuality(run), CalQuality::Stable);
  EXPECT_STREQ(rtc32k::resultLabel(run), "PARTIAL");
  // The median still comes from the single good sample.
  EXPECT_EQ(calMedianHz(run), 32768u);
}

TEST(Xtal32kCalibration, TwoValidSamplesMedianSplitsThePair) {
  CalRun run;
  run.record(true, 32766);
  run.record(false, 0);
  run.record(true, 32770);
  for (size_t i = 0; i < 2; i++) run.record(false, 0);
  EXPECT_EQ(run.okCount(), 2u);
  EXPECT_EQ(calQuality(run), CalQuality::Partial);
  EXPECT_EQ(calMedianHz(run), 32768u);  // (32766+32770)/2
}

// All 5 valid but readings wander far beyond crystal tolerance -> UNSTABLE,
// never STABLE.
TEST(Xtal32kCalibration, WanderingSamplesAreUnstable) {
  const uint32_t wandering[rtc32k::CAL_SAMPLE_COUNT] = {32752, 32784, 32768, 32768, 32768};
  const CalRun run = allOk(wandering);
  EXPECT_EQ(calStatus(run), CalStatus::Done);
  EXPECT_EQ(calMedianHz(run), 32768u);
  EXPECT_EQ(calQuality(run), CalQuality::Unstable);
  EXPECT_STREQ(rtc32k::resultLabel(run), "UNSTABLE");
}

// Defense in depth: the RTC peripheral already rejects wild readings, but the
// policy still refuses to bless a median outside the window.
TEST(Xtal32kCalibration, OutOfWindowMedianIsOutOfRange) {
  const uint32_t slow[rtc32k::CAL_SAMPLE_COUNT] = {25000, 25000, 25000, 25000, 25000};
  const CalRun run = allOk(slow);
  EXPECT_EQ(calStatus(run), CalStatus::Done);
  EXPECT_EQ(calQuality(run), CalQuality::OutOfRange);
  EXPECT_STREQ(rtc32k::resultLabel(run), "OUT OF RANGE");
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
  EXPECT_STREQ(buf, "FAIL");  // not "TIMEOUT": the API cannot tell why it failed
  rtc32k::formatCalSample(buf, sizeof(buf), run, 2);
  EXPECT_STREQ(buf, "-");
  rtc32k::formatCalSample(buf, sizeof(buf), run, 99);
  EXPECT_STREQ(buf, "-");
}

TEST(Xtal32kFormatting, MedianLineCoversAllStates) {
  char buf[24];
  rtc32k::formatCalMedian(buf, sizeof(buf), CalRun{});
  EXPECT_STREQ(buf, "NOT RUN");
  rtc32k::formatCalMedian(buf, sizeof(buf), allFailed());
  EXPECT_STREQ(buf, "NO VALID 32K");
  const uint32_t nominal[rtc32k::CAL_SAMPLE_COUNT] = {32768, 32767, 32768, 32769, 32768};
  rtc32k::formatCalMedian(buf, sizeof(buf), allOk(nominal));
  EXPECT_STREQ(buf, "32768 Hz");
}

TEST(Xtal32kFormatting, ResultLabelsNeverClaimAPassOnFailure) {
  EXPECT_STREQ(rtc32k::resultLabel(CalRun{}), "NOT RUN");
  EXPECT_STREQ(rtc32k::resultLabel(allFailed()), "NO VALID 32K");
  const uint32_t nominal[rtc32k::CAL_SAMPLE_COUNT] = {32768, 32768, 32768, 32768, 32768};
  EXPECT_STREQ(rtc32k::resultLabel(allOk(nominal)), "STABLE");
  const uint32_t wandering[rtc32k::CAL_SAMPLE_COUNT] = {32752, 32784, 32768, 32768, 32768};
  EXPECT_STREQ(rtc32k::resultLabel(allOk(wandering)), "UNSTABLE");
}

TEST(Xtal32kFormatting, StateLabelOnlyAssertsTheEnableState) {
  EXPECT_STREQ(rtc32k::stateLabel(true), "ENABLED");
  EXPECT_STREQ(rtc32k::stateLabel(false), "DISABLED");
}

TEST(Xtal32kFormatting, SlowClockSourceLabels) {
  EXPECT_STREQ(rtc32k::slowClockSourceName(SlowClkSource::RcSlow), "RC SLOW");
  EXPECT_STREQ(rtc32k::slowClockSourceName(SlowClkSource::Xtal32k), "XTAL32K");
  EXPECT_STREQ(rtc32k::slowClockSourceName(SlowClkSource::RcFastD256), "RC FAST D256");
  EXPECT_STREQ(rtc32k::slowClockSourceName(SlowClkSource::Invalid), "UNKNOWN");
}

// --- session state bookkeeping -----------------------------------------------

TEST(Xtal32kSession, CalibrationHistorySurvivesDisable) {
  // Disable changes the oscillator state only; the last calibration stays
  // on screen until the next Calibrate action resets the run.
  DiagState s = DiagState::initial(false);
  s.applyEnableResult(true);
  s.cal.record(true, 32768);
  s.applyDisableResult(false);
  EXPECT_EQ(calStatus(s.cal), CalStatus::Done);
}

TEST(Xtal32kSession, FreshCalibrationRunClearsPreviousSamples) {
  DiagState s = DiagState::initial(true);
  s.cal.record(true, 32768);
  s.cal.record(true, 32768);
  s.cal.beginRun();
  EXPECT_EQ(s.cal.count, 0u);
  EXPECT_EQ(calStatus(s.cal), CalStatus::NotRun);
}

// --- expected technician flow ------------------------------------------------

TEST(Xtal32kSession, TechnicianFlowOnFreshHardwareEndsClean) {
  DiagState s = DiagState::initial(false);  // power-on: generator disabled
  // Enable -> readback ENABLED, exit owns the undo.
  s.applyEnableResult(true);
  EXPECT_TRUE(rtc32k::shouldDisableOnExit(s, false));
  // Calibrate -> five good samples, stable.
  const uint32_t nominal[rtc32k::CAL_SAMPLE_COUNT] = {32768, 32768, 32767, 32768, 32768};
  for (size_t i = 0; i < rtc32k::CAL_SAMPLE_COUNT; i++) s.cal.record(true, nominal[i]);
  EXPECT_EQ(calQuality(s.cal), CalQuality::Stable);
  // Disable -> readback DISABLED, exit owes nothing anymore.
  s.applyDisableResult(false);
  EXPECT_FALSE(s.xtalEnabled);
  EXPECT_FALSE(rtc32k::shouldDisableOnExit(s, false));
}
