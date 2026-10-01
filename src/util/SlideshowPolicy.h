#pragma once

#include <ToneLut.h>

#include <cstdint>

// Slideshow policy (pure, host-tested): the persisted interval cadence, the
// retained-state mode, the append-only sleep-screen value and the fixed sleep
// source directories. Every consumer (viewer, main, SleepActivity) resolves
// intervals and profiles through this header — conversion constants live in
// exactly one place.
namespace slideshow {

// Which use case armed the retained state. Decides the render profile, the
// power-button exit semantics and the continuation activity.
enum class Mode : uint8_t { Viewer = 0, Sleep = 1 };

// Append-only SLEEP_SCREEN_MODE value for SLIDESHOW: appended immediately
// before SLEEP_SCREEN_MODE_COUNT so existing numeric values never move. The
// device enum aliases this constant and the host test pins the number.
inline constexpr uint8_t SLEEP_SCREEN_SLIDESHOW_VALUE = 8;

// Persisted slideshow interval: an INDEX into this fixed cadence (append-safe
// — new values are appended, existing indices never move). Shared by the
// Image Viewer slideshow and Sleep Screen = Slideshow.
inline constexpr uint8_t INTERVAL_COUNT = 4;
inline constexpr uint8_t INTERVAL_DEFAULT_INDEX = 1;  // 5 min

// Canonical conversion — the ONLY interval conversion. A corrupt/out-of-range
// persisted byte clamps to the default cadence, so a timer-armed sleep can
// never get a 0 or absurd interval.
inline constexpr uint8_t intervalMinutes(const uint8_t index) {
  switch (index) {
    case 0:
      return 1;
    case 1:
      return 5;
    case 2:
      return 10;
    case 3:
      return 30;
    default:
      return intervalMinutes(INTERVAL_DEFAULT_INDEX);
  }
}
inline constexpr uint64_t intervalMicros(const uint8_t index) {
  return static_cast<uint64_t>(intervalMinutes(index)) * 60ULL * 1000 * 1000;
}

// Cyclic interval step (wrap in both directions), starting from the clamped
// stored value.
inline constexpr uint8_t intervalIndexClamped(const uint8_t stored) {
  return stored < INTERVAL_COUNT ? stored : INTERVAL_DEFAULT_INDEX;
}
inline constexpr uint8_t intervalIndexStepped(const uint8_t current, const int delta) {
  const int stepped = static_cast<int>(intervalIndexClamped(current)) + (delta >= 0 ? 1 : INTERVAL_COUNT - 1);
  return static_cast<uint8_t>(stepped % INTERVAL_COUNT);
}

// Render-profile policy (pure): the viewer slideshow renders with the
// viewer's ACTIVE session tone, the sleep slideshow with the persisted sleep
// render profile — a sleep frame can never silently pick up the viewer
// profile after a timer wake.
inline const ToneProfile& renderProfileFor(const Mode mode, const ToneProfile& viewerActive,
                                           const ToneProfile& sleepProfile) {
  return mode == Mode::Sleep ? sleepProfile : viewerActive;
}

// Fixed sleep-slideshow storage contract (production increment 1): the
// canonical directory is scanned first, the pre-rename legacy directory is
// the fallback. No configurable directory yet; /sleep.bmp is NOT part of this
// contract (it stays the legacy single-custom-image).
inline constexpr const char* SLEEP_SLIDESHOW_DIR = "/.sleep";
inline constexpr const char* SLEEP_SLIDESHOW_DIR_LEGACY = "/sleep";

// Image Viewer Slideshow page rows (pure policy): row 0 = Start slideshow;
// row 1 = Interval (the shared persisted cadence — Left/Right and the touch
// stepper step it in both directions, Confirm/row-tap steps forward).
// Anything else activates nothing.
enum class PageRowAction : uint8_t { None = 0, Start, IntervalStepForward };
inline PageRowAction pageRowAction(const int row) {
  if (row == 0) return PageRowAction::Start;
  if (row == 1) return PageRowAction::IntervalStepForward;
  return PageRowAction::None;
}
inline constexpr int SLIDESHOW_PAGE_ROWS = 2;
// The Interval row is the page's only editable row.
inline constexpr int INTERVAL_PAGE_ROW = 1;

}  // namespace slideshow
