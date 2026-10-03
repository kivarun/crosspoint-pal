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

TEST(Rtc32kClassify, FullVerdictContract) {
  // PASS: RTC + CLKOUT + raw in window + calibration accepted.
  EXPECT_EQ(classify(true, true, 32770, 32766), Verdict::Pass);
  // Partial: usable raw signal but the ext-osc calibration failed.
  EXPECT_EQ(classify(true, true, 32770, 0), Verdict::Partial);
  EXPECT_EQ(classify(true, true, 32000, 25000), Verdict::Partial);
  // FAIL: no usable signal on the pin.
  EXPECT_EQ(classify(true, true, 0, 0), Verdict::Fail);
  EXPECT_EQ(classify(true, true, 5, 0), Verdict::Fail);
  // FAIL: RTC or CLKOUT unavailable (dominates any reading).
  EXPECT_EQ(classify(false, true, 32770, 32766), Verdict::Fail);
  EXPECT_EQ(classify(true, false, 32770, 32766), Verdict::Fail);
  EXPECT_EQ(classify(false, false, 0, 0), Verdict::Fail);
}
