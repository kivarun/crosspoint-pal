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
  EXPECT_EQ(slideshow::intervalIndexStepped(3, 1), 0);   // wrap forward
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

// Picker/value-list sizing (SlideshowPolicy seam): a capable board advertises
// the full table (Slideshow at its persisted value 8); an incapable board's
// value list stops BEFORE the appended entry — no phantom picker item, and
// the enum clamp in CrossPointSettings::fromJson() (enumLabels().size())
// folds a stored value 8 back to the default.
TEST(SleepScreenModeCompatibility, AdvertisedCountTracksCapability) {
  EXPECT_EQ(slideshow::advertisedSleepScreenValueCount(true, 9), 9);
  EXPECT_EQ(slideshow::advertisedSleepScreenValueCount(false, 9), 8);
  // The device contract: the full table is SLIDESHOW + the 8 historical
  // values (CrossPointSettings.h static_asserts SLIDESHOW == 8).
  EXPECT_EQ(9, static_cast<size_t>(slideshow::SLEEP_SCREEN_SLIDESHOW_VALUE) + 1);
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
TEST(SlideshowMode, ValuesAreDistinct) { EXPECT_NE(slideshow::Mode::Viewer, slideshow::Mode::Sleep); }

// Timeout quick-resume precedence: Sleep Screen = Slideshow beats the
// QUICK_RESUME_AFTER_TIMEOUT upgrade — a timeout with Slideshow selected
// starts the slideshow, not the moon frame.
TEST(TimeoutQuickResumePrecedence, SlideshowBeatsTimeoutQuickResume) {
  constexpr uint8_t SLIDESHOW = slideshow::SLEEP_SCREEN_SLIDESHOW_VALUE;
  constexpr uint8_t QUICK_RESUME = slideshow::SLEEP_SCREEN_QUICK_RESUME_VALUE;
  constexpr uint8_t DARK = 0;
  constexpr uint8_t AFTER_TIMEOUT = slideshow::QUICK_RESUME_AFTER_TIMEOUT_VALUE;

  // Slideshow + timeout + QuickResumeAfterTimeout -> slideshow wins.
  EXPECT_FALSE(slideshow::timeoutQuickResume(SLIDESHOW, /*fromTimeout=*/true, AFTER_TIMEOUT));
  // non-Slideshow + timeout + QuickResumeAfterTimeout -> Quick Resume.
  EXPECT_TRUE(slideshow::timeoutQuickResume(DARK, true, AFTER_TIMEOUT));
  EXPECT_TRUE(slideshow::timeoutQuickResume(QUICK_RESUME, true, AFTER_TIMEOUT));
  // Explicit Quick Resume -> Quick Resume regardless of the timeout option.
  EXPECT_TRUE(slideshow::timeoutQuickResume(QUICK_RESUME, false, AFTER_TIMEOUT));
  EXPECT_TRUE(slideshow::timeoutQuickResume(QUICK_RESUME, false, /*NEVER=*/0));
  // Manual sleep (not a timeout) only renders Quick Resume when selected.
  EXPECT_FALSE(slideshow::timeoutQuickResume(DARK, false, AFTER_TIMEOUT));
  EXPECT_FALSE(slideshow::timeoutQuickResume(SLIDESHOW, false, AFTER_TIMEOUT));
}

// Sleep-slideshow low-battery cutoff: external power bypasses the floor; on
// battery the slideshow stops once the charge reaches it.
TEST(SlideshowBatteryCutoff, BoundaryAndChargingBypass) {
  EXPECT_TRUE(slideshow::allowedByBattery(11, false));   // one above the floor
  EXPECT_FALSE(slideshow::allowedByBattery(10, false));  // at the floor: stopped
  EXPECT_FALSE(slideshow::allowedByBattery(0, false));   // empty: stopped
  EXPECT_TRUE(slideshow::allowedByBattery(10, true));    // charging bypasses
  EXPECT_TRUE(slideshow::allowedByBattery(0, true));     // external power bypasses
  EXPECT_TRUE(slideshow::allowedByBattery(100, false));  // healthy battery
}

// Persisted order policy: the fixed table, its append-safe clamp and its
// cyclic stepping mirror the interval contract.
TEST(SlideshowOrder, TableAndClamping) {
  EXPECT_EQ(slideshow::ORDER_COUNT, 3);
  EXPECT_EQ(slideshow::ORDER_DEFAULT_INDEX, 0);  // Forward
  EXPECT_EQ(slideshow::orderIndexClamped(0), 0);
  EXPECT_EQ(slideshow::orderIndexClamped(2), 2);
  EXPECT_EQ(slideshow::orderIndexClamped(3), slideshow::ORDER_DEFAULT_INDEX);   // out of range -> Forward
  EXPECT_EQ(slideshow::orderIndexClamped(255), slideshow::ORDER_DEFAULT_INDEX);  // corrupt -> Forward
  EXPECT_EQ(slideshow::orderClamped(1), slideshow::Order::Reverse);
  EXPECT_EQ(slideshow::orderClamped(9), slideshow::Order::Forward);
}

TEST(SlideshowOrder, StepsCycleInBothDirections) {
  EXPECT_EQ(slideshow::orderIndexStepped(0, 1), 1);
  EXPECT_EQ(slideshow::orderIndexStepped(1, 1), 2);
  EXPECT_EQ(slideshow::orderIndexStepped(2, 1), 0);   // wrap forward
  EXPECT_EQ(slideshow::orderIndexStepped(0, -1), 2);  // wrap backward
  EXPECT_EQ(slideshow::orderIndexStepped(1, -1), 0);
  // A corrupt stored value steps from the clamped default.
  EXPECT_EQ(slideshow::orderIndexStepped(200, 1), 1);
}

// Forward: sorted list, wrap-around advance; a missing frame restarts from
// the first entry.
TEST(SlideshowOrderAdvance, Forward) {
  EXPECT_EQ(slideshow::indexAfterAdvance(0, 3, slideshow::Order::Forward, 0), 1);
  EXPECT_EQ(slideshow::indexAfterAdvance(1, 3, slideshow::Order::Forward, 0), 2);
  EXPECT_EQ(slideshow::indexAfterAdvance(2, 3, slideshow::Order::Forward, 0), 0);  // wrap
  EXPECT_EQ(slideshow::indexAfterAdvance(-1, 3, slideshow::Order::Forward, 7), 0);  // missing -> first
  EXPECT_EQ(slideshow::indexAfterAdvance(0, 1, slideshow::Order::Forward, 5), 0);  // single item
  EXPECT_EQ(slideshow::indexAfterAdvance(0, 0, slideshow::Order::Forward, 5), -1);  // empty -> fail closed
}

// Reverse: backward walk; a missing frame restarts from the last entry.
TEST(SlideshowOrderAdvance, Reverse) {
  EXPECT_EQ(slideshow::indexAfterAdvance(0, 3, slideshow::Order::Reverse, 0), 2);
  EXPECT_EQ(slideshow::indexAfterAdvance(2, 3, slideshow::Order::Reverse, 0), 1);
  EXPECT_EQ(slideshow::indexAfterAdvance(1, 3, slideshow::Order::Reverse, 0), 0);
  EXPECT_EQ(slideshow::indexAfterAdvance(-1, 3, slideshow::Order::Reverse, 7), 2);  // missing -> last
  EXPECT_EQ(slideshow::indexAfterAdvance(0, 1, slideshow::Order::Reverse, 5), 0);   // single item
}

// Random: result in range, never the current frame when others exist, and a
// single item stays the only candidate. The raw random value comes from the
// caller (device RNG at runtime; fixed values here).
TEST(SlideshowOrderAdvance, Random) {
  constexpr int count = 3;
  for (const uint32_t seed : {0u, 1u, 2u, 3u, 7u, 1000003u, 4294967295u}) {
    const int current = static_cast<int>(seed % count);
    const int next = slideshow::indexAfterAdvance(current, count, slideshow::Order::Random, seed);
    ASSERT_GE(next, 0);
    ASSERT_LT(next, count);
    EXPECT_NE(next, current) << "seed=" << seed;  // no immediate repeat
  }
  // Any value, no current frame: a valid index.
  const int any = slideshow::indexAfterAdvance(-1, count, slideshow::Order::Random, 4294967295u);
  EXPECT_GE(any, 0);
  EXPECT_LT(any, count);
  // Single item: the only candidate, even with a current frame.
  EXPECT_EQ(slideshow::indexAfterAdvance(0, 1, slideshow::Order::Random, 42), 0);
  // Empty: fail closed.
  EXPECT_EQ(slideshow::indexAfterAdvance(-1, 0, slideshow::Order::Random, 42), -1);
}

// Initial selection: Forward -> first, Reverse -> last, Random -> a valid
// random index.
TEST(SlideshowOrderInitial, PerOrderSemantics) {
  EXPECT_EQ(slideshow::initialIndex(3, slideshow::Order::Forward, 0), 0);
  EXPECT_EQ(slideshow::initialIndex(3, slideshow::Order::Reverse, 0), 2);
  const int randomStart = slideshow::initialIndex(3, slideshow::Order::Random, 4294967295u);
  EXPECT_GE(randomStart, 0);
  EXPECT_LT(randomStart, 3);
  // Single item: index 0 under every order.
  EXPECT_EQ(slideshow::initialIndex(1, slideshow::Order::Forward, 9), 0);
  EXPECT_EQ(slideshow::initialIndex(1, slideshow::Order::Reverse, 9), 0);
  EXPECT_EQ(slideshow::initialIndex(1, slideshow::Order::Random, 9), 0);
  // Empty: fail closed.
  EXPECT_EQ(slideshow::initialIndex(0, slideshow::Order::Random, 9), -1);
}

// Slideshow page rows (pure policy): Start / Interval / Order — their
// actions, the editable rows and the button-hint slot sets.
TEST(SlideshowPageRows, ActionsSelectableCountAndHints) {
  EXPECT_EQ(slideshow::SLIDESHOW_PAGE_ROWS, 3);
  EXPECT_EQ(slideshow::pageRowAction(0), slideshow::PageRowAction::Start);
  EXPECT_EQ(slideshow::pageRowAction(1), slideshow::PageRowAction::IntervalStepForward);
  EXPECT_EQ(slideshow::pageRowAction(2), slideshow::PageRowAction::OrderStepForward);
  EXPECT_EQ(slideshow::pageRowAction(3), slideshow::PageRowAction::None);
  EXPECT_EQ(slideshow::pageRowAction(-1), slideshow::PageRowAction::None);
  EXPECT_EQ(slideshow::INTERVAL_PAGE_ROW, 1);
  EXPECT_EQ(slideshow::ORDER_PAGE_ROW, 2);

  // Hint slots: the Start row offers ONLY its real actions (no +/-); the
  // editable rows also show the stepper keys. Every row erases unused slots.
  const auto start = slideshow::pageHintSlots(0);
  EXPECT_TRUE(start.confirmStart);
  EXPECT_FALSE(start.stepperSlots);
  const auto interval = slideshow::pageHintSlots(1);
  EXPECT_FALSE(interval.confirmStart);
  EXPECT_TRUE(interval.stepperSlots);
  const auto order = slideshow::pageHintSlots(2);
  EXPECT_FALSE(order.confirmStart);
  EXPECT_TRUE(order.stepperSlots);
}

}  // namespace
