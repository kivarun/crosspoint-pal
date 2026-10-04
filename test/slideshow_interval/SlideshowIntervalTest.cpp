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
  EXPECT_EQ(slideshow::orderIndexClamped(3), slideshow::ORDER_DEFAULT_INDEX);    // out of range -> Forward
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
  EXPECT_EQ(slideshow::indexAfterAdvance(2, 3, slideshow::Order::Forward, 0), 0);   // wrap
  EXPECT_EQ(slideshow::indexAfterAdvance(-1, 3, slideshow::Order::Forward, 7), 0);  // missing -> first
  EXPECT_EQ(slideshow::indexAfterAdvance(0, 1, slideshow::Order::Forward, 5), 0);   // single item
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

// Slideshow page rows (pure policy): Start / Interval / Order with ONE
// activation contract — Confirm (button) starts from ANY row; touch taps
// start on the Start row and are select-only on value rows.
TEST(SlideshowPageRows, ActionsSelectableCountAndHints) {
  EXPECT_EQ(slideshow::SLIDESHOW_PAGE_ROWS, 3);
  EXPECT_EQ(slideshow::pageRowAction(0), slideshow::PageRowAction::Start);
  EXPECT_EQ(slideshow::pageRowAction(1), slideshow::PageRowAction::Start);
  EXPECT_EQ(slideshow::pageRowAction(2), slideshow::PageRowAction::Start);
  EXPECT_EQ(slideshow::pageRowAction(3), slideshow::PageRowAction::None);
  EXPECT_EQ(slideshow::pageRowAction(-1), slideshow::PageRowAction::None);
  EXPECT_EQ(slideshow::pageRowTapAction(0), slideshow::PageRowAction::Start);
  EXPECT_EQ(slideshow::pageRowTapAction(1), slideshow::PageRowAction::SelectOnly);
  EXPECT_EQ(slideshow::pageRowTapAction(2), slideshow::PageRowAction::SelectOnly);
  EXPECT_EQ(slideshow::pageRowTapAction(3), slideshow::PageRowAction::None);
  EXPECT_EQ(slideshow::INTERVAL_PAGE_ROW, 1);
  EXPECT_EQ(slideshow::ORDER_PAGE_ROW, 2);

  // Hint slots: EVERY row's Confirm slot shows Start; the editable rows also
  // show the stepper keys. Every row erases unused slots (checked at the
  // caller: eraseUnused is always true for this page).
  for (const int row : {0, 1, 2}) {
    const auto slots = slideshow::pageHintSlots(row);
    EXPECT_TRUE(slots.confirmStart) << "row=" << row;
  }
  const auto start = slideshow::pageHintSlots(0);
  EXPECT_FALSE(start.stepperSlots);
  const auto interval = slideshow::pageHintSlots(1);
  EXPECT_TRUE(interval.stepperSlots);
  const auto order = slideshow::pageHintSlots(2);
  EXPECT_TRUE(order.stepperSlots);
}

// ---- Sleep-screen settings applicability (pure policy): the two
// sleep-cover presentation rows are inert while Sleep Screen = Slideshow
// (the slideshow pipeline never consults them); Slideshow Interval/Order
// stay enabled on every sleep screen. ----

// Applicability table over the append-only sleep-screen byte (8 = Slideshow,
// pinned by SLEEP_SCREEN_SLIDESHOW_VALUE).
TEST(SleepScreenSettings, CoverRowsDisabledOnlyUnderSlideshow) {
  using slideshow::SleepScreenRow;
  // Slideshow: the two cover rows disabled, everything else enabled.
  for (const auto row : {SleepScreenRow::CoverMode, SleepScreenRow::CoverFilter}) {
    EXPECT_FALSE(slideshow::sleepScreenRowEnabled(8, row)) << "row=" << static_cast<int>(row);
  }
  EXPECT_TRUE(slideshow::sleepScreenRowEnabled(8, SleepScreenRow::Interval));
  EXPECT_TRUE(slideshow::sleepScreenRowEnabled(8, SleepScreenRow::Order));
  EXPECT_TRUE(slideshow::sleepScreenRowEnabled(8, SleepScreenRow::Other));

  // Every other sleep screen (cover/custom/dark/quick-resume and the
  // post-Slideshow append space): the cover rows are back, enabled.
  for (const uint8_t mode : {0, 1, 2, 3, 4, 5, 6, 7, 9, 255}) {
    EXPECT_TRUE(slideshow::sleepScreenRowEnabled(mode, SleepScreenRow::CoverMode)) << "mode=" << mode;
    EXPECT_TRUE(slideshow::sleepScreenRowEnabled(mode, SleepScreenRow::CoverFilter)) << "mode=" << mode;
  }
}

// Activation guard semantics: a disabled row cannot mutate its value or open
// its picker — the activity dispatches Apply/Inert from the SAME
// applicability predicate that greys the row, so the two can never disagree.
TEST(SleepScreenSettings, DisabledRowIsInert) {
  using slideshow::SleepScreenRow;
  // The guard mirrors sleepScreenRowEnabled: disabled only for the cover
  // rows under Slideshow (mode 8); the stored values are never touched by
  // the guard (it is a pure predicate over mode + row).
  EXPECT_FALSE(slideshow::sleepScreenRowEnabled(slideshow::SLEEP_SCREEN_SLIDESHOW_VALUE, SleepScreenRow::CoverMode));
  EXPECT_TRUE(slideshow::sleepScreenRowEnabled(0, SleepScreenRow::CoverMode));
  // Re-enabling keeps the value domain: the predicate reads no value, so
  // switching Slideshow -> Cover cannot have reset anything.
  EXPECT_EQ(slideshow::SLEEP_SCREEN_SLIDESHOW_VALUE, 8);
}

// ---- Randomized exhaustive cycle (LCG + cycle walking, adapted from
// CrossPoint upstream PR #3841 by @gkaindl). All RNG values are fixed, so
// these tests carry no statistical dependence. ----

// Helper: runs one full cycle from currentIndex with the given odd increment
// and records how often each index appears — the pre-shown current frame is
// the cycle's first element.
std::vector<int> runCycle(const int count, const uint16_t increment, const int currentIndex) {
  std::vector<int> seen(count, 0);
  seen[currentIndex] = 1;
  slideshow::RandomCycleState state{};
  state.increment = increment;
  state.remaining = count - 1;
  state.total = count;
  int current = currentIndex;
  for (int picks = 0; picks < count - 1; ++picks) {
    const auto step = slideshow::randomCycleNext(current, count, state, 0);
    EXPECT_GE(step.index, 0);
    EXPECT_LT(step.index, count);
    if (step.index < 0 || step.index >= count) return seen;
    seen[step.index]++;
    state = step.state;
    current = step.index;
  }
  return seen;
}

// Each index appears exactly once per cycle, for power-of-two and
// non-power-of-two counts, small and large, and for different odd
// increments. N=1 repeats unavoidably.
TEST(RandomCycle, ExhaustsEveryIndexExactlyOnce) {
  const auto single = slideshow::randomCycleNext(0, 1, slideshow::RandomCycleState{}, 7);
  EXPECT_EQ(single.index, 0);

  for (const int count : {2, 3, 5, 8, 1000}) {
    for (const uint16_t increment : {1u, 3u, 5u, 7u, 65535u}) {
      if (increment >= slideshow::randomCycleModulus(count)) continue;
      const auto seen = runCycle(count, increment, 0);
      for (int idx = 0; idx < count; ++idx) {
        ASSERT_EQ(seen[idx], 1) << "count=" << count << " increment=" << increment << " idx=" << idx;
      }
    }
  }
}

// Supported count range 1..UINT16_MAX, at its boundary shapes: 32768 (the
// last power-of-two modulus equal to the count), 32769 (modulus doubles to
// 65536), 65535 (the uint16 cap, increment up to 65535) — all still exhaust
// exactly once. Anything above the cap fails closed (-1) instead of
// building a cycle the uint16 metadata cannot represent.
TEST(RandomCycle, SupportedCountBounds) {
  for (const auto& [count, increment] : {std::pair<int, uint16_t>{32768, 1}, {32769, 3}, {65535, 65535}}) {
    const auto seen = runCycle(count, increment, 0);
    for (int idx = 0; idx < count; ++idx) {
      ASSERT_EQ(seen[idx], 1) << "count=" << count << " increment=" << increment << " idx=" << idx;
    }
  }
  // A derived increment at the cap: modulus 65536, every odd value fits and
  // stays odd (mod an even power of two preserves oddness).
  EXPECT_EQ(slideshow::randomCycleIncrement(4294967295u, 65535), 65535);
  EXPECT_EQ(slideshow::randomCycleIncrement(32768u, 65535), 1);

  // Beyond the cap: fail closed, no cycle is built. The state below is what
  // a 65536-image directory would leave in the truncated metadata.
  slideshow::RandomCycleState state{};
  state.increment = 1;
  state.remaining = 65535;
  state.total = 0;
  for (const int count : {65536, 65537, 100000}) {
    EXPECT_EQ(slideshow::randomCycleNext(0, count, state, 42).index, -1) << "count=" << count;
    EXPECT_FALSE(slideshow::randomCycleValid(state, count));
  }
}

// No immediate repeat within a cycle, and none across the cycle boundary
// either (first pick of the new cycle differs from the last frame).
TEST(RandomCycle, NoImmediateRepeatWithinOrAcrossCycles) {
  for (const int count : {2, 3, 5, 8}) {
    slideshow::RandomCycleState state{};
    state.increment = 5;
    state.remaining = count - 1;
    state.total = count;
    int current = 0;
    for (int picks = 0; picks < count - 1; ++picks) {
      const auto step = slideshow::randomCycleNext(current, count, state, 0);
      ASSERT_NE(step.index, current) << "count=" << count << " pick=" << picks;
      state = step.state;
      current = step.index;
    }
    const auto step = slideshow::randomCycleNext(current, count, state, 42);
    EXPECT_NE(step.index, current) << "count=" << count;
  }
}

// Corrupt retained metadata, directory changes and a missing current index
// all fail safe to a fresh cycle; empty directories fail closed.
TEST(RandomCycle, ResetSemantics) {
  slideshow::RandomCycleState ok{};
  ok.increment = 5;
  ok.remaining = 1;
  ok.total = 3;

  slideshow::RandomCycleState corrupt{};
  corrupt.increment = 4;  // even
  corrupt.remaining = 2;
  corrupt.total = 3;
  const auto even = slideshow::randomCycleNext(0, 3, corrupt, 7);
  EXPECT_GE(even.index, 0);
  EXPECT_LT(even.index, 3);
  EXPECT_TRUE(slideshow::randomCycleValid(even.state, 3));

  corrupt.increment = 5;
  corrupt.total = 9;  // directory changed
  const auto changed = slideshow::randomCycleNext(0, 3, corrupt, 7);
  EXPECT_GE(changed.index, 0);
  EXPECT_TRUE(slideshow::randomCycleValid(changed.state, 3));

  corrupt.total = 3;
  corrupt.remaining = 99;  // out of range
  const auto stale = slideshow::randomCycleNext(0, 3, corrupt, 7);
  EXPECT_TRUE(slideshow::randomCycleValid(stale.state, 3));

  // Missing current index: a FRESH cycle, proven by the increment change —
  // the valid old cycle carried increment 7; randomValue 42 derives 1.
  EXPECT_EQ(slideshow::randomCycleIncrement(42, 3), 1);
  const auto missing = slideshow::randomCycleNext(-1, 3, ok, 42);
  EXPECT_GE(missing.index, 0);
  EXPECT_LT(missing.index, 3);
  EXPECT_EQ(missing.state.increment, 1);  // reset used the caller's RNG value
  EXPECT_TRUE(slideshow::randomCycleValid(missing.state, 3));
  // An out-of-range current index behaves the same (fresh cycle).
  const auto outOfRange = slideshow::randomCycleNext(3, 3, ok, 42);
  EXPECT_EQ(outOfRange.state.increment, 1);
  EXPECT_TRUE(slideshow::randomCycleValid(outOfRange.state, 3));

  // Empty directory: fail closed.
  EXPECT_EQ(slideshow::randomCycleNext(0, 0, ok, 7).index, -1);
  EXPECT_EQ(slideshow::randomCycleNext(0, -3, ok, 7).index, -1);
}

// The caller's randomValue owns the new-cycle increment: different values
// derive different odd increments deterministically.
TEST(RandomCycle, CallerOwnedRandomValue) {
  const slideshow::RandomCycleState fresh{};
  EXPECT_EQ(slideshow::randomCycleNext(0, 3, fresh, 42).state.increment, 1);
  EXPECT_EQ(slideshow::randomCycleNext(0, 3, fresh, 43).state.increment, 3);
  // Mid-cycle the value is ignored: the increment stays the cycle's own.
  slideshow::RandomCycleState mid{};
  mid.increment = 3;  // valid odd increment for modulus 4
  mid.remaining = 1;
  mid.total = 3;
  EXPECT_EQ(slideshow::randomCycleNext(0, 3, mid, 4294967295u).state.increment, 3);
}

// LCG shapes: power-of-two modulus and the odd increment derivation.
TEST(RandomCycle, HullDobellShapes) {
  EXPECT_EQ(slideshow::randomCycleModulus(1), 1);
  EXPECT_EQ(slideshow::randomCycleModulus(2), 2);
  EXPECT_EQ(slideshow::randomCycleModulus(3), 4);
  EXPECT_EQ(slideshow::randomCycleModulus(5), 8);
  EXPECT_EQ(slideshow::randomCycleModulus(8), 8);
  EXPECT_EQ(slideshow::randomCycleModulus(9), 16);
  EXPECT_EQ(slideshow::randomCycleIncrement(0, 3), 1);
  EXPECT_EQ(slideshow::randomCycleIncrement(1, 3), 3);
  EXPECT_EQ(slideshow::randomCycleIncrement(2, 3), 1);  // wraps inside the modulus
  EXPECT_EQ(slideshow::randomCycleIncrement(4294967295u, 5), 7);
}

// Forward/Reverse regression: their wrap-advance semantics are untouched.
TEST(RandomCycle, ForwardReverseUnchanged) {
  EXPECT_EQ(slideshow::indexAfterAdvance(0, 3, slideshow::Order::Forward, 0), 1);
  EXPECT_EQ(slideshow::indexAfterAdvance(2, 3, slideshow::Order::Forward, 0), 0);
  EXPECT_EQ(slideshow::indexAfterAdvance(0, 3, slideshow::Order::Reverse, 0), 2);
  EXPECT_EQ(slideshow::indexAfterAdvance(2, 3, slideshow::Order::Reverse, 0), 1);
  EXPECT_EQ(slideshow::indexAfterAdvance(-1, 3, slideshow::Order::Forward, 7), 0);
  EXPECT_EQ(slideshow::indexAfterAdvance(-1, 3, slideshow::Order::Reverse, 7), 2);
}

}  // namespace
