#include <gtest/gtest.h>

#include "util/SlideshowPolicy.h"

// The slideshow policy contracts: interval cadence (exact timer values,
// clamped reads, cyclic stepping), the append-only sleep-screen value, the
// fixed sleep source directories and the per-mode render-profile policy.

namespace {

TEST(SlideshowInterval, ExactCadenceValues) {
  EXPECT_EQ(slideshow::intervalMinutes(0), 1);
  EXPECT_EQ(slideshow::intervalMinutes(1), 5);
  EXPECT_EQ(slideshow::intervalMinutes(2), 10);
  EXPECT_EQ(slideshow::intervalMinutes(3), 30);

  EXPECT_EQ(slideshow::intervalMicros(0), 60ULL * 1000 * 1000);
  EXPECT_EQ(slideshow::intervalMicros(1), 300ULL * 1000 * 1000);
  EXPECT_EQ(slideshow::intervalMicros(2), 600ULL * 1000 * 1000);
  EXPECT_EQ(slideshow::intervalMicros(3), 1800ULL * 1000 * 1000);
}

// A corrupt/out-of-range persisted byte resolves to the default cadence —
// never to a 0 or absurd timer value.
TEST(SlideshowInterval, CorruptIndexClampsToDefault) {
  EXPECT_EQ(slideshow::intervalMinutes(4), 5);
  EXPECT_EQ(slideshow::intervalMinutes(255), 5);
  EXPECT_EQ(slideshow::intervalMicros(255), 300ULL * 1000 * 1000);
  EXPECT_EQ(slideshow::intervalIndexClamped(4), slideshow::INTERVAL_DEFAULT_INDEX);
  EXPECT_EQ(slideshow::intervalIndexClamped(255), slideshow::INTERVAL_DEFAULT_INDEX);
  EXPECT_EQ(slideshow::INTERVAL_DEFAULT_INDEX, 1);  // 5 min default
}

TEST(SlideshowInterval, StepsCycleInBothDirections) {
  EXPECT_EQ(slideshow::intervalIndexStepped(0, 1), 1);
  EXPECT_EQ(slideshow::intervalIndexStepped(1, 1), 2);
  EXPECT_EQ(slideshow::intervalIndexStepped(2, 1), 3);
  EXPECT_EQ(slideshow::intervalIndexStepped(3, 1), 0);  // wrap forward
  EXPECT_EQ(slideshow::intervalIndexStepped(0, -1), 3);  // wrap backward
  EXPECT_EQ(slideshow::intervalIndexStepped(1, -1), 0);
  // A corrupt stored value steps from the clamped default.
  EXPECT_EQ(slideshow::intervalIndexStepped(200, 1), 2);
}

// SLEEP_SCREEN_MODE::SLIDESHOW is append-only: appended after the last
// historical value so existing numeric values never move.
TEST(SleepScreenModeCompatibility, SlideshowValueIsAppendOnly) {
  EXPECT_EQ(slideshow::SLEEP_SCREEN_SLIDESHOW_VALUE, 8);
}

// Fixed production storage contract: canonical /.sleep first, legacy /sleep
// fallback — the fallback order is part of the contract.
TEST(SleepSlideshowSource, DirectoryFallbackOrder) {
  EXPECT_STREQ(slideshow::SLEEP_SLIDESHOW_DIR, "/.sleep");
  EXPECT_STREQ(slideshow::SLEEP_SLIDESHOW_DIR_LEGACY, "/sleep");
}

// The retained mode selects the render profile: the viewer slideshow keeps
// the viewer's active session tone, the sleep slideshow always uses the
// persisted sleep profile.
TEST(SlideshowRenderProfilePolicy, ModeSelectsProfile) {
  ToneProfile viewerActive{};
  viewerActive.brightnessPct = 80;
  ToneProfile sleepProfile{};
  sleepProfile.brightnessPct = 120;

  EXPECT_EQ(&slideshow::renderProfileFor(slideshow::Mode::Viewer, viewerActive, sleepProfile), &viewerActive);
  EXPECT_EQ(&slideshow::renderProfileFor(slideshow::Mode::Sleep, viewerActive, sleepProfile), &sleepProfile);
}

// The retained mode discriminates the two use cases.
TEST(SlideshowMode, ValuesAreDistinct) {
  EXPECT_NE(slideshow::Mode::Viewer, slideshow::Mode::Sleep);
}

}  // namespace
