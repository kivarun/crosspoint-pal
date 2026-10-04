#include <gtest/gtest.h>

#include <cstdint>

#include "util/Rtc32kDiagnosticsPolicy.h"

using rtc32k::classify;
using rtc32k::rawEstimateHz;
using rtc32k::rawWindowOk;
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
