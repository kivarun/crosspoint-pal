#pragma once

#include <ToneLut.h>

#include <cstddef>
#include <cstdint>

// Slideshow policy (pure, host-tested): the persisted interval cadence, the
// persisted frame order, the retained-state mode, the append-only sleep-screen
// value, the timeout quick-resume precedence, the low-battery cutoff and the
// fixed sleep source directories. Every consumer (viewer, main, SleepActivity)
// resolves intervals, orders and profiles through this header — conversion
// constants live in exactly one place.
namespace slideshow {

// Which use case armed the retained state. Decides the render profile, the
// power-button exit semantics and the continuation activity.
enum class Mode : uint8_t { Viewer = 0, Sleep = 1 };

// Append-only SLEEP_SCREEN_MODE value for SLIDESHOW: appended immediately
// before SLEEP_SCREEN_MODE_COUNT so existing numeric values never move. The
// device enum aliases this constant and the host test pins the number.
inline constexpr uint8_t SLEEP_SCREEN_SLIDESHOW_VALUE = 8;
// Historical sleep-screen values the timeout precedence policy needs. The
// device enum aliases them and CrossPointSettings.h pins the numbers.
inline constexpr uint8_t SLEEP_SCREEN_QUICK_RESUME_VALUE = 6;
// QUICK_RESUME_SLEEP_SCREEN: 0 = never, 1 = after timeout.
inline constexpr uint8_t QUICK_RESUME_AFTER_TIMEOUT_VALUE = 1;

// Timeout quick-resume precedence (pure, host-tested): the explicit
// QUICK_RESUME sleep screen always renders Quick Resume; the
// QUICK_RESUME_AFTER_TIMEOUT option upgrades the auto-sleep timeout to Quick
// Resume for every OTHER screen. Sleep Screen = Slideshow keeps priority over
// it — a timeout with Slideshow selected starts the slideshow instead of the
// quick-resume moon frame.
inline constexpr bool timeoutQuickResume(const uint8_t sleepScreen, const bool fromTimeout,
                                         const uint8_t quickResumeSleepScreen) {
  return sleepScreen == SLEEP_SCREEN_QUICK_RESUME_VALUE ||
         (fromTimeout && sleepScreen != SLEEP_SCREEN_SLIDESHOW_VALUE &&
          quickResumeSleepScreen == QUICK_RESUME_AFTER_TIMEOUT_VALUE);
}

// Sleep-slideshow low-battery cutoff (pure, host-tested): the timer wake must
// not run a low battery down. External power (charging or USB) bypasses the
// floor; on battery the slideshow is blocked once the charge reaches it.
inline constexpr uint8_t BATTERY_CUTOFF_PERCENT = 10;
inline constexpr bool allowedByBattery(const uint16_t percent, const bool externalPowered) {
  return externalPowered || percent > BATTERY_CUTOFF_PERCENT;
}

// Which DISPLAY settings row is being judged (pure, host-tested). The
// Slideshow sleep screen renders its frames through imageonly::renderImageFile()
// and never consults the sleep-cover presentation settings, so those two rows
// are inert while Slideshow is selected. Slideshow Interval/Order stay enabled
// on every sleep screen: the Image Viewer slideshow consumes them regardless.
enum class SleepScreenRow : uint8_t { Other = 0, CoverMode, CoverFilter, Interval, Order };
inline constexpr bool sleepScreenRowEnabled(const uint8_t sleepScreenMode, const SleepScreenRow row) {
  if (row != SleepScreenRow::CoverMode && row != SleepScreenRow::CoverFilter) return true;
  return sleepScreenMode != SLEEP_SCREEN_SLIDESHOW_VALUE;
}

// Persisted slideshow order: an INDEX into this fixed table (append-safe —
// new values are appended, existing indices never move). Shared by the
// Image Viewer slideshow and Sleep Screen = Slideshow. A corrupt/out-of-range
// persisted byte clamps to the default (Forward).
inline constexpr uint8_t ORDER_COUNT = 3;
inline constexpr uint8_t ORDER_DEFAULT_INDEX = 0;  // Forward

enum class Order : uint8_t { Forward = 0, Reverse = 1, Random = 2 };

inline constexpr uint8_t orderIndexClamped(const uint8_t stored) {
  return stored < ORDER_COUNT ? stored : ORDER_DEFAULT_INDEX;
}
inline constexpr Order orderClamped(const uint8_t stored) {
  return static_cast<Order>(orderIndexClamped(stored));
}
// Cyclic order step (wrap in both directions), starting from the clamped
// stored value.
inline constexpr uint8_t orderIndexStepped(const uint8_t current, const int delta) {
  const int stepped = static_cast<int>(orderIndexClamped(current)) + (delta >= 0 ? 1 : ORDER_COUNT - 1);
  return static_cast<uint8_t>(stepped % ORDER_COUNT);
}

// Index-selection policy (pure, host-tested) over a sorted image list. The
// random value comes from the caller (the device RNG at runtime; a fixed
// value in tests), so the policy itself stays deterministic.
//
// Forward: wrap-around advance (A -> B -> C -> A); a frame missing from the
// scan restarts from the first entry.
// Reverse: backward walk (C -> B -> A -> C); a missing frame restarts from
// the last entry.
// Random: an independent uniform pick. With a current frame on screen and
// more than one candidate, the immediate repeat is EXCLUDED from the
// candidate set (uniform over count-1 indices); with none, any index. No
// shuffle history between frames.
inline int indexAfterAdvance(const int currentIndex, const int count, const Order order,
                             const uint32_t randomValue) {
  if (count <= 0) return -1;  // nothing usable; the caller fails closed
  if (count == 1) return 0;   // the only frame
  const bool haveCurrent = currentIndex >= 0 && currentIndex < count;
  switch (order) {
    case Order::Forward:
      return haveCurrent ? (currentIndex + 1) % count : 0;
    case Order::Reverse:
      return haveCurrent ? (currentIndex + count - 1) % count : count - 1;
    case Order::Random:
    default: {
      if (!haveCurrent) return static_cast<int>(randomValue % static_cast<uint32_t>(count));
      // Uniform over the count-1 candidates that are not the current frame.
      const uint32_t reduced = randomValue % static_cast<uint32_t>(count - 1);
      return static_cast<int>(reduced + (reduced >= static_cast<uint32_t>(currentIndex) ? 1 : 0));
    }
  }
}

// Initial selection for a slideshow START (no prior frame shown): Forward
// opens the first sorted entry, Reverse the last, Random an arbitrary one.
// The Image Viewer's Start ignores this — its open image IS the first frame.
inline int initialIndex(const int count, const Order order, const uint32_t randomValue) {
  if (count <= 0) return -1;
  if (count == 1) return 0;
  switch (order) {
    case Order::Forward:
      return 0;
    case Order::Reverse:
      return count - 1;
    case Order::Random:
    default:
      return static_cast<int>(randomValue % static_cast<uint32_t>(count));
  }
}

// ---- Randomized exhaustive cycle (Random order) ----
//
// LCG/cycle-walking approach adapted from CrossPoint upstream PR #3841 by
// @gkaindl (https://github.com/crosspoint-reader/crosspoint-reader/pull/3841):
// a full-period LCG over the smallest power-of-two modulus >= count
// (multiplier 5, odd increment — Hull-Dobell gives full period over 2^k),
// walked with cycle-walking. Unlike the upstream sleep-image use, the walk
// position is the CURRENT image index (derived from the retained path), so
// the cycle follows the LCG order from wherever the user currently is and no
// permutation array or history list is needed.
//
// Every available image is shown exactly once before the first repeat; an
// exhausted cycle starts a new random one (fresh odd increment), and the
// walk skips the frame already on screen — no immediate repeat within a
// cycle or across the boundary (for count > 1).

// Retained (RTC) cycle metadata. The image index itself is NOT stored — it
// is derived from the retained path and the sorted directory scan.
struct RandomCycleState {
  uint16_t increment;  // odd LCG increment of the current cycle
  uint16_t remaining;  // picks left before the current cycle exhausts
  uint16_t total;      // image count the current cycle was built for
};

// Modulus of the LCG: the smallest power of two >= count.
inline constexpr uint32_t randomCycleModulus(const int count) {
  uint32_t m = 1;
  while (m < static_cast<uint32_t>(count)) m <<= 1;
  return m;
}

// Odd LCG increment derived from the caller's device-RNG value (Hull-Dobell:
// multiplier 5 + odd increment = full period over 2^k). Reducing first keeps
// the doubling overflow-safe.
inline constexpr uint16_t randomCycleIncrement(const uint32_t randomValue, const int count) {
  const uint32_t m = randomCycleModulus(count);
  return static_cast<uint16_t>(((randomValue % m) * 2u + 1u) % m);
}

// A freshly started cycle: the current frame is its first element.
inline constexpr RandomCycleState randomCycleReset(const uint32_t randomValue, const int count) {
  RandomCycleState cycle{};
  cycle.total = static_cast<uint16_t>(count);
  cycle.remaining = count > 1 ? static_cast<uint16_t>(count - 1) : 0;
  cycle.increment = randomCycleIncrement(randomValue, count);
  return cycle;
}

// Mid-cycle state validity against the CURRENT directory: the supported
// image count is 1..UINT16_MAX (the retained metadata is uint16), matching
// count, an odd increment inside the modulus, remaining in [1, count-1].
// Anything else (corrupt metadata, directory changed) resets the cycle at
// use time.
inline constexpr bool randomCycleValid(const RandomCycleState& cycle, const int count) {
  if (count <= 1 || count > static_cast<int>(UINT16_MAX)) return false;
  if (cycle.total != static_cast<uint16_t>(count)) return false;
  if (cycle.remaining == 0 || cycle.remaining >= static_cast<uint16_t>(count)) return false;
  if (cycle.increment % 2u == 0) return false;
  return static_cast<uint32_t>(cycle.increment) < randomCycleModulus(count);
}

struct RandomCycleStep {
  int index;              // next image index (-1: no usable frame)
  RandomCycleState state; // updated retained metadata
};

// Pure next-frame policy of the Random order. randomValue feeds a NEW
// cycle's increment. A fresh cycle is started when the current one is
// exhausted or invalid (corrupt metadata / directory changed) — or when the
// current index is out of range (the retained path is missing from the
// scan): a cycle always continues from a real position on the LCG walk, so
// a missing current never rides a formally valid old cycle. The supported
// image count is 1..UINT16_MAX; anything beyond fails closed (-1) instead
// of building a cycle the uint16 metadata cannot represent.
inline RandomCycleStep randomCycleNext(const int currentIndex, const int count,
                                       const RandomCycleState& cycle, const uint32_t randomValue) {
  RandomCycleStep step{};
  step.index = -1;
  // No frames, or beyond the uint16 metadata range: the caller fails closed.
  if (count <= 0 || count > static_cast<int>(UINT16_MAX)) return step;

  if (count == 1) {
    // The only image, unavoidably repeated; the cycle is trivially exhausted.
    step.index = 0;
    step.state = randomCycleReset(randomValue, count);
    return step;
  }

  const bool haveCurrent = currentIndex >= 0 && currentIndex < count;
  // Exhausted, invalid, or no current frame to continue from: a new cycle
  // starts at the current frame, with a fresh odd increment from the RNG.
  step.state = (cycle.remaining == 0 || !haveCurrent || !randomCycleValid(cycle, count))
                   ? randomCycleReset(randomValue, count)
                   : cycle;
  --step.state.remaining;

  // Cycle-walk the LCG from the current position: skip out-of-range states
  // and the frame already on screen. Terminates within the modulus: for
  // count > 1 at least one valid frame other than the current one is on the
  // full-period cycle. A reset with a missing current walks deterministically
  // from the first entry.
  uint32_t state = haveCurrent ? static_cast<uint32_t>(currentIndex) : 0u;
  const uint32_t m = randomCycleModulus(count);
  do {
    state = (5u * state + step.state.increment) % m;
  } while (state >= static_cast<uint32_t>(count) || state == static_cast<uint32_t>(currentIndex));
  step.index = static_cast<int>(state);
  return step;
}

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

// Sleep-screen picker/value-list sizing (pure): a board advertises exactly
// the modes it supports — the full SLEEP_SCREEN_MODE table on a
// sleep-slideshow-capable board, the table minus the appended SLIDESHOW entry
// elsewhere. Sizing the label list this way keeps the picker free of a
// phantom entry AND makes CrossPointSettings::fromJson()'s enum clamp
// (enumLabels().size()) fold a stored SLIDESHOW value back to the default on
// incapable boards — without renumbering anything.
inline constexpr size_t advertisedSleepScreenValueCount(const bool sleepSlideshowCapable, const size_t fullCount) {
  return sleepSlideshowCapable ? fullCount : fullCount - 1;
}

// Image Viewer Slideshow page rows (pure policy): row 0 = Start slideshow;
// row 1 = Interval (the shared persisted cadence); row 2 = Order (the shared
// persisted frame order). ONE activation contract: Confirm on ANY
// slideshow-menu row STARTS the slideshow (the open image is the first
// frame); Confirm is never a value-edit operation — the editable value rows
// are changed by the explicit axes only (Left/Right and the touch -/+
// steppers). Touch taps on a value row are select-only (focus move), and
// taps on the Start row start. Anything else activates nothing.
inline constexpr int SLIDESHOW_PAGE_ROWS = 3;
// The page's editable value rows.
inline constexpr int INTERVAL_PAGE_ROW = 1;
inline constexpr int ORDER_PAGE_ROW = 2;

enum class PageRowAction : uint8_t { None = 0, Start, SelectOnly };
// Button Confirm dispatch.
inline constexpr PageRowAction pageRowAction(const int row) {
  if (row >= 0 && row < SLIDESHOW_PAGE_ROWS) return PageRowAction::Start;
  return PageRowAction::None;
}
// Touch row-tap dispatch: the Start row starts, value rows are select-only.
inline PageRowAction pageRowTapAction(const int row) {
  if (row == 0) return PageRowAction::Start;
  if (row == INTERVAL_PAGE_ROW || row == ORDER_PAGE_ROW) return PageRowAction::SelectOnly;
  return PageRowAction::None;
}

// Slideshow-page button-hint slots (pure, host-tested): EVERY row's Confirm
// slot shows Start (Confirm starts from any row); the editable value rows
// also show the stepper keys (Back | Start | - | + in mapped-label terms),
// the Start row shows only Back | Start. The whole page draws with
// eraseUnused so a row or page switch physically removes stale hint frames
// (drawButtonHints skips empty slots by design — other callers rely on that;
// the opt-in erase is modal-repaint-only).
struct PageHintSlots {
  bool confirmStart;   // Confirm slot shows Start
  bool stepperSlots;   // Left/Right slots carry "-"/"+"
};
inline constexpr PageHintSlots pageHintSlots(const int row) {
  return {pageRowAction(row) == PageRowAction::Start, row == INTERVAL_PAGE_ROW || row == ORDER_PAGE_ROW};
}

}  // namespace slideshow
