#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "util/RtcClockDiagCore.h"

namespace {

using rtcclockdiag::CycleSample;
using rtcclockdiag::DiagLogRing;

// Hardware-measured RC and 32k calibrations (X4C, p0-rtc-clock run 1):
// Q13.19 periods of 139521 Hz and 32772 Hz.
constexpr uint32_t kCalRc = 3756929;    // 1e6 * 2^19 / 139521
constexpr uint32_t kCal32k = 15997444;  // 1e6 * 2^19 / 32772

TEST(RtcClockDiagCore, ConsistentCycleHasNearZeroError) {
  // A 10 s request whose alarm matched the sleep clock: 327,700 ticks.
  const CycleSample s{10000000ULL, kCal32k, 327700ULL};
  EXPECT_NEAR(rtcclockdiag::measuredUs(s), 10000000, 1000);
  EXPECT_NEAR(rtcclockdiag::cycleErrorUs(s), 0, 1000);
  EXPECT_NEAR(rtcclockdiag::cycleErrorPpm(s), 0, 100);
}

TEST(RtcClockDiagCore, LongSleepSignatureMatchesRcPeriod) {
  // The reported hardware failure: a 10 s request that slept ~42 s. The tick
  // count of such a sleep (10 s worth of 139521 Hz) must back-solve to the
  // RC calibration period, pinning the diagnostic interpretation.
  const CycleSample s{10000000ULL, kCal32k, 1395208ULL};
  const uint64_t periodEff = rtcclockdiag::effectiveCalPeriod(s);
  EXPECT_GT(periodEff, 3750000);
  EXPECT_LT(periodEff, 3765000);
  EXPECT_GT(rtcclockdiag::cycleErrorUs(s), 30000000);
}

TEST(RtcClockDiagCore, ZeroGuardsReturnZero) {
  const CycleSample noTicks{10000000ULL, kCal32k, 0};
  EXPECT_EQ(rtcclockdiag::effectiveCalPeriod(noTicks), 0);
  const CycleSample noCal{10000000ULL, 0, 327700};
  EXPECT_EQ(rtcclockdiag::measuredUs(noCal), 0);
  const CycleSample noReq{0, kCal32k, 327700};
  EXPECT_EQ(rtcclockdiag::cycleErrorPpm(noReq), 0);
}

TEST(RtcClockDiagRing, PushAndIterateOldestFirst) {
  DiagLogRing<4, 32> ring;
  ring.push(7, "one");
  ring.push(7, "two");
  ring.push(7, "three");
  std::vector<std::string> seen;
  ring.forEach([&](const char* line) {
    seen.emplace_back(line);
    return true;
  });
  ASSERT_EQ(seen.size(), 3u);
  EXPECT_EQ(seen[0], "one");
  EXPECT_EQ(seen[2], "three");
}

TEST(RtcClockDiagRing, WrapsAndOverwritesOldest) {
  DiagLogRing<3, 32> ring;
  for (int i = 0; i < 5; ++i) {
    char line[32];
    snprintf(line, sizeof(line), "line%d", i);
    ring.push(9, line);
  }
  std::vector<std::string> seen;
  ring.forEach([&](const char* line) {
    seen.emplace_back(line);
    return true;
  });
  ASSERT_EQ(seen.size(), 3u);
  EXPECT_EQ(seen[0], "line2");
  EXPECT_EQ(seen[2], "line4");
}

TEST(RtcClockDiagRing, GarbageStampRecoversOnPush) {
  DiagLogRing<4, 32> ring;
  ring.push(0, "ignored");  // stamp 0 never seeds the ring
  ring.push(5, "kept");
  std::vector<std::string> seen;
  ring.forEach([&](const char* line) {
    seen.emplace_back(line);
    return true;
  });
  ASSERT_EQ(seen.size(), 1u);
  EXPECT_EQ(seen[0], "kept");
}

TEST(RtcClockDiagRing, StampChangeResetsHistory) {
  // A new session (different stamp) must not read the previous one's lines.
  DiagLogRing<4, 32> ring;
  ring.push(5, "old");
  ring.push(6, "new");
  std::vector<std::string> seen;
  ring.forEach([&](const char* line) {
    seen.emplace_back(line);
    return true;
  });
  ASSERT_EQ(seen.size(), 1u);
  EXPECT_EQ(seen[0], "new");
}

TEST(RtcClockDiagRing, LinesAreTruncatedToCapacity) {
  DiagLogRing<2, 8> ring;
  ring.push(3, "0123456789");
  std::vector<std::string> seen;
  ring.forEach([&](const char* line) {
    seen.emplace_back(line);
    return true;
  });
  ASSERT_EQ(seen.size(), 1u);
  EXPECT_EQ(seen[0], "0123456");
}

}  // namespace
