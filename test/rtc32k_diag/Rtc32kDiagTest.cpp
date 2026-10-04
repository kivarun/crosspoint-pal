#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>

#include "util/Rtc32kDiagnosticsPolicy.h"

using rtc32k::classify;
using rtc32k::LinkCounts;
using rtc32k::LinkProbeResult;
using rtc32k::percentileFromSorted;
using rtc32k::rawEstimateHz;
using rtc32k::rawWindowOk;
using rtc32k::RunState;
using rtc32k::swingMv;
using rtc32k::Verdict;

TEST(Rtc32kRawWindow, WideAcceptanceWindow) {
  EXPECT_FALSE(rawWindowOk(0));
  EXPECT_FALSE(rawWindowOk(29999));
  EXPECT_TRUE(rawWindowOk(30000));
  EXPECT_TRUE(rawWindowOk(32768));
  EXPECT_TRUE(rawWindowOk(35000));
  EXPECT_FALSE(rawWindowOk(35001));
}

TEST(Rtc32kRawEstimate, WindowCountScalesToHz) {
  EXPECT_EQ(rawEstimateHz(0), 0u);
  EXPECT_EQ(rawEstimateHz(3277), 32770u);
  EXPECT_EQ(rawEstimateHz(100), 1000u);
}

TEST(Rtc32kMedian, MedianOfTenCounts) {
  uint32_t nominal[10] = {3275, 3278, 3276, 3277, 3279, 3276, 3278, 3277, 3275, 3278};
  EXPECT_EQ(rtc32k::medianOf10(nominal), 3277u);  // (3277+3277)/2
  uint32_t dropout[10] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 3};
  EXPECT_EQ(rtc32k::medianOf10(dropout), 0u);  // old unsoldered trace: units of edges
  uint32_t symmetric[10] = {5, 4, 3, 2, 1, 10, 9, 8, 7, 6};
  EXPECT_EQ(rtc32k::medianOf10(symmetric), 5u);  // (5+6)/2
}

TEST(Rtc32kClassify, ExtOscOwnsVerdict) {
  // PCNT counts through the ordinary digital GPIO path (informational); the
  // EXT_OSC calibration is the actual acceptance test of the XTAL_32K_P input
  // path, so only EXT_OSC owns PASS. The ten cases pin the exact contract.
  // 1: RTC unavailable -> FAIL whatever the readings say.
  EXPECT_EQ(classify(false, true, 32768, 32768), Verdict::Fail);
  // 2: CLKOUT control failed -> FAIL.
  EXPECT_EQ(classify(true, false, 32768, 32768), Verdict::Fail);
  // 3: both readings in window -> PASS.
  EXPECT_EQ(classify(true, true, 32768, 32768), Verdict::Pass);
  // 4 (KEY): digital GPIO sees nothing, EXT_OSC accepts -> PASS.
  EXPECT_EQ(classify(true, true, 0, 32768), Verdict::Pass);
  // 5: a few Hz of digital noise, EXT_OSC accepts -> PASS.
  EXPECT_EQ(classify(true, true, 25, 32766), Verdict::Pass);
  // 6: raw in window, calibration timed out (0) -> PARTIAL.
  EXPECT_EQ(classify(true, true, 32768, 0), Verdict::Partial);
  // 7: raw in window, calibration measured out of window -> PARTIAL.
  EXPECT_EQ(classify(true, true, 32768, 25000), Verdict::Partial);
  // 8: nothing anywhere -> FAIL.
  EXPECT_EQ(classify(true, true, 0, 0), Verdict::Fail);
  // 9: digital noise only, no calibration -> FAIL.
  EXPECT_EQ(classify(true, true, 5, 0), Verdict::Fail);
  // 10: both readings out of window -> FAIL.
  EXPECT_EQ(classify(true, true, 25000, 29000), Verdict::Fail);
}

// Mirrors the device wiring of runDiagnostics(): every pass starts with
// RunState::reset() before any transaction, so a run that fails at its first
// I2C operation classifies from an empty state and can never display the
// previous run's samples or calibration.
TEST(Rtc32kRunLifecycle, FailedRunNeverShowsStaleMeasurements) {
  RunState run;

  // Fresh success: enable ok, ten full-count windows, calibration accepted.
  run.reset();
  run.rtcOk = true;
  run.clkoutOk = true;
  for (auto& c : run.edgeCounts) c = 3277u;
  run.calHz = 32766u;
  uint32_t rawHz = rtc32k::rawEstimateHz(rtc32k::medianOf10(run.edgeCounts));
  EXPECT_EQ(classify(run.rtcOk, run.clkoutOk, rawHz, run.calHz), Verdict::Pass);

  // Run again -> success again: reset must not prevent a repeat PASS.
  run.reset();
  run.rtcOk = true;
  run.clkoutOk = true;
  for (auto& c : run.edgeCounts) c = 3276u;
  run.calHz = 32768u;
  rawHz = rtc32k::rawEstimateHz(rtc32k::medianOf10(run.edgeCounts));
  EXPECT_EQ(classify(run.rtcOk, run.clkoutOk, rawHz, run.calHz), Verdict::Pass);

  // Run again -> RTC/I2C failure on this pass: reset() zeroes everything
  // first, the failure leaves both flags down, and classify FAILs with no
  // stale samples, calibration, link counts or link verdict anywhere.
  run.reset();
  EXPECT_FALSE(run.rtcOk);
  EXPECT_FALSE(run.clkoutOk);
  for (const auto& c : run.edgeCounts) {
    EXPECT_EQ(c, 0u);
  }
  EXPECT_EQ(run.calHz, 0u);
  EXPECT_EQ(run.calNoPullHz, 0u);
  EXPECT_EQ(run.link.off1, 0u);
  EXPECT_EQ(run.link.hz32, 0u);
  EXPECT_EQ(run.link.hz1024, 0u);
  EXPECT_EQ(run.link.hz32768, 0u);
  EXPECT_EQ(run.link.off2, 0u);
  EXPECT_EQ(run.linkResult, LinkProbeResult::Unavailable);
  rawHz = rtc32k::rawEstimateHz(rtc32k::medianOf10(run.edgeCounts));
  EXPECT_EQ(classify(run.rtcOk, run.clkoutOk, rawHz, run.calHz), Verdict::Fail);

  // Run again -> CLKOUT control failure: RTC still answers (flag up), output
  // cannot be re-enabled (flag down), measurements stay empty -> FAIL.
  run.reset();
  run.rtcOk = true;
  EXPECT_EQ(classify(run.rtcOk, run.clkoutOk, 0u, 0u), Verdict::Fail);
}

TEST(Rtc32kLinkProbe, NominalSequenceFollowed) {
  // Nominal follow with realistic imperfections and tolerated OFF noise.
  LinkCounts c;
  c.off1 = 0;
  c.hz32 = 31;
  c.hz1024 = 1026;
  c.hz32768 = 3275;
  c.off2 = 2;
  EXPECT_EQ(rtc32k::linkFollows(c), LinkProbeResult::Followed);
  // OFF noise exactly at the tolerated ceiling still follows.
  c.off1 = 16;
  c.off2 = 16;
  EXPECT_EQ(rtc32k::linkFollows(c), LinkProbeResult::Followed);
}

TEST(Rtc32kLinkProbe, NotFollowedCases) {
  // All-zero counts (the reset/unavailable state) can never be Followed.
  EXPECT_EQ(rtc32k::linkFollows(LinkCounts{}), LinkProbeResult::NotFollowed);
  // Floating-pin noise: no commanded frequency reproduced.
  LinkCounts floating{2, 3, 5, 4, 1};
  EXPECT_EQ(rtc32k::linkFollows(floating), LinkProbeResult::NotFollowed);
  // Only one random frequency point matches -> not a link.
  LinkCounts single{0, 0, 1020, 0, 0};
  EXPECT_EQ(rtc32k::linkFollows(single), LinkProbeResult::NotFollowed);
  // Wrong frequency mapping (32 Hz reading at the 1024 Hz stage).
  LinkCounts crossed{0, 1026, 31, 3275, 0};
  EXPECT_EQ(rtc32k::linkFollows(crossed), LinkProbeResult::NotFollowed);
  // 32 kHz stage counting at full rate (out of band above).
  LinkCounts fullRate{0, 31, 1024, 32770, 0};
  EXPECT_EQ(rtc32k::linkFollows(fullRate), LinkProbeResult::NotFollowed);
  // Sustained front stream during an OFF phase.
  LinkCounts noisy{17, 31, 1026, 3275, 0};
  EXPECT_EQ(rtc32k::linkFollows(noisy), LinkProbeResult::NotFollowed);
  // In-band upper edges stay Followed.
  LinkCounts bandEdges{0, 64, 2048, 6554, 0};
  EXPECT_EQ(rtc32k::linkFollows(bandEdges), LinkProbeResult::Followed);
}

TEST(Rtc32kRunState, ResetClearsEveryPerRunValue) {
  RunState run;
  run.rtcOk = true;
  run.clkoutOk = true;
  for (int i = 0; i < 10; i++) run.edgeCounts[i] = 3277u;
  run.calHz = 32766u;
  run.calNoPullHz = 32760u;
  run.link = {2, 31, 1026, 3275, 1};
  run.linkResult = LinkProbeResult::Followed;
  run.adcAvailable = true;
  run.adcLowMv = 42;
  run.adcHighMv = 2760;
  run.adcSwingMv = 2718;
  run.reset();
  EXPECT_FALSE(run.rtcOk);
  EXPECT_FALSE(run.clkoutOk);
  for (int i = 0; i < 10; i++) {
    EXPECT_EQ(run.edgeCounts[i], 0u);
  }
  EXPECT_EQ(run.calHz, 0u);
  EXPECT_EQ(run.calNoPullHz, 0u);
  EXPECT_EQ(run.link.off1, 0u);
  EXPECT_EQ(run.link.hz32, 0u);
  EXPECT_EQ(run.link.hz1024, 0u);
  EXPECT_EQ(run.link.hz32768, 0u);
  EXPECT_EQ(run.link.off2, 0u);
  EXPECT_EQ(run.linkResult, LinkProbeResult::Unavailable);
  EXPECT_FALSE(run.adcAvailable);
  EXPECT_EQ(run.adcLowMv, 0u);
  EXPECT_EQ(run.adcHighMv, 0u);
  EXPECT_EQ(run.adcSwingMv, 0u);
}

// Synthetic 1 Hz probe: 400 samples at ~10 ms over 4 s (four full periods) —
// clean square wave, ~200 samples per plateau.
TEST(Rtc32kLevels, Synthetic1HzPlateaus) {
  uint16_t samples[400];
  for (int i = 0; i < 400; i++) samples[i] = (i % 2 == 0) ? 400 : 2800;
  std::sort(samples, samples + 400);
  const uint16_t low = percentileFromSorted(samples, 400, rtc32k::LOW_PERCENTILE);
  const uint16_t high = percentileFromSorted(samples, 400, rtc32k::HIGH_PERCENTILE);
  EXPECT_EQ(low, 400);
  EXPECT_EQ(high, 2800);
  EXPECT_EQ(swingMv(low, high), 2400);
}

// Transition samples and rare outliers must not move the P10/P90 levels:
// the nearest-rank percentile sits well inside each plateau.
TEST(Rtc32kLevels, TransitionsAndOutliersDoNotMoveLevels) {
  uint16_t samples[400];
  int idx = 0;
  for (int i = 0; i < 3; i++) samples[idx++] = 0;        // glitch spikes
  for (int i = 0; i < 196; i++) samples[idx++] = 400;    // low plateau
  for (int i = 0; i < 4; i++) samples[idx++] = 1500;     // transitions
  for (int i = 0; i < 196; i++) samples[idx++] = 2800;   // high plateau
  samples[idx++] = 4095;                                 // spike above swing
  std::sort(samples, samples + 400);
  const uint16_t low = percentileFromSorted(samples, 400, rtc32k::LOW_PERCENTILE);
  const uint16_t high = percentileFromSorted(samples, 400, rtc32k::HIGH_PERCENTILE);
  EXPECT_EQ(low, 400);
  EXPECT_EQ(high, 2800);
  EXPECT_EQ(swingMv(low, high), 2400);

  // Nearest-rank index policy: k = ceil(pct*n/100), 1-indexed -> k-1.
  uint16_t ten[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  EXPECT_EQ(percentileFromSorted(ten, 10, 10), 1);   // k=1 -> idx 0
  EXPECT_EQ(percentileFromSorted(ten, 10, 50), 5);   // k=5 -> idx 4
  EXPECT_EQ(percentileFromSorted(ten, 10, 90), 9);   // k=9 -> idx 8
  EXPECT_EQ(percentileFromSorted(ten, 10, 100), 10); // k=10 -> idx 9
  uint16_t one[1] = {77};
  EXPECT_EQ(percentileFromSorted(one, 1, 10), 77);
  EXPECT_EQ(percentileFromSorted(one, 1, 90), 77);

  // Degenerate reading clamps to a non-negative swing.
  EXPECT_EQ(swingMv(2800, 400), 0);
}

// Run 1 succeeds with ADC levels; run 2 has the ADC unavailable. Every
// voltage field must be cleared by reset() (screen shows N/A), while the
// PCNT/EXT_OSC measurements of the new run keep working on their own.
TEST(Rtc32kRunLifecycle, AdcFailureLeavesNoStaleVoltage) {
  RunState run;

  // Run 1: full success including ADC levels.
  run.reset();
  run.rtcOk = true;
  run.clkoutOk = true;
  for (auto& c : run.edgeCounts) c = 3277u;
  run.calHz = 32766u;
  run.adcAvailable = true;
  run.adcLowMv = 42;
  run.adcHighMv = 2760;
  run.adcSwingMv = 2718;
  uint32_t rawHz = rtc32k::rawEstimateHz(rtc32k::medianOf10(run.edgeCounts));
  EXPECT_EQ(classify(run.rtcOk, run.clkoutOk, rawHz, run.calHz), Verdict::Pass);

  // Run 2: ADC unavailable. The reset clears every voltage field; the new
  // run's PCNT/EXT_OSC measurements (filled again below) still classify.
  run.reset();
  EXPECT_FALSE(run.adcAvailable);
  EXPECT_EQ(run.adcLowMv, 0u);
  EXPECT_EQ(run.adcHighMv, 0u);
  EXPECT_EQ(run.adcSwingMv, 0u);
  run.rtcOk = true;
  run.clkoutOk = true;
  for (auto& c : run.edgeCounts) c = 3277u;
  run.calHz = 32768u;
  rawHz = rtc32k::rawEstimateHz(rtc32k::medianOf10(run.edgeCounts));
  EXPECT_EQ(classify(run.rtcOk, run.clkoutOk, rawHz, run.calHz), Verdict::Pass);
}
