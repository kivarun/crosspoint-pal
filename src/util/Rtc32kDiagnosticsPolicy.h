#pragma once

// Pure classification policy for the X4C RTC 32 kHz diagnostics screen
// (temporary solder-joint verification firmware, experiment branch only).
// Host-testable: no Arduino/ESP includes.

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace rtc32k {

// Wide acceptance window for a kHz-class 32 kHz reading (not a precision
// gate). Shared by the raw digital observation and the EXT_OSC calibration.
inline constexpr uint32_t RAW_WINDOW_MIN_HZ = 30000;
inline constexpr uint32_t RAW_WINDOW_MAX_HZ = 35000;

// Verdict of the full diagnostic pass.
enum class Verdict : uint8_t { Pass = 0, Partial = 1, Fail = 2 };

// PCNT edge count for one 100 ms window on the 32k input pin.
inline constexpr uint32_t EDGE_WINDOW_MS = 100;

inline constexpr bool rawWindowOk(const uint32_t hz) {
  return hz >= RAW_WINDOW_MIN_HZ && hz <= RAW_WINDOW_MAX_HZ;
}

// 100 ms window count -> Hz estimate.
inline constexpr uint32_t rawEstimateHz(const uint32_t medianEdges) {
  return medianEdges * (1000 / EDGE_WINDOW_MS);
}

// --- 1 Hz CLKOUT electrical probe (ADC2 on GPIO15) ---
// Informational observation only: calibrated-mV levels of the CLKOUT square
// wave at 1 Hz. These values NEVER feed classify() — no "safe voltage",
// common-mode or electrical-compliance threshold is encoded here; acceptance
// stays owned by the EXT_OSC calibration.

// Sampling plan: ~4 s at ~100 samples/s = 400 samples; a 1 Hz level is held
// ~0.5 s, so each plateau gets ~50 samples.
inline constexpr size_t ADC_SAMPLE_COUNT = 400;
inline constexpr uint32_t ADC_SAMPLE_INTERVAL_MS = 10;

// Robust levels: P10/P90 of the calibrated-mV samples instead of absolute
// min/max, so transition samples and outliers do not distort the levels.
inline constexpr uint8_t LOW_PERCENTILE = 10;
inline constexpr uint8_t HIGH_PERCENTILE = 90;

// Nearest-rank percentile over an ASCENDING-SORTED sample list:
// k = ceil(pct/100 * n), 1-indexed -> 0-indexed position k-1, clamped to [1,n].
inline constexpr uint16_t percentileFromSorted(const uint16_t* sorted, const size_t n,
                                               const uint8_t pct) {
  if (n == 0) return 0;
  size_t k = (static_cast<size_t>(pct) * n + 99) / 100;
  if (k < 1) k = 1;
  if (k > n) k = n;
  return sorted[k - 1];
}

// Level swing, clamped so a degenerate (high <= low) reading stays >= 0.
inline constexpr uint16_t swingMv(const uint16_t lowMv, const uint16_t highMv) {
  return highMv > lowMv ? static_cast<uint16_t>(highMv - lowMv) : 0;
}

// --- Controlled pull-up link probe (BM8563 CLKOUT -> GPIO15) ---
// GPIO15 gets an explicit input + internal pull-up configuration and counts
// rising edges while the RTC is commanded OFF -> 32 Hz -> 1024 Hz -> 32768 Hz
// -> OFF. Reproducible frequency-following is strong functional evidence of
// an existing electrical link CLKOUT -> GPIO15; it is NOT metrological PCB
// continuity (the final physical check is a multimeter) and it does NOT feed
// the main verdict.
enum class LinkProbeResult : uint8_t { Followed = 0, NotFollowed = 1, Unavailable = 2 };

struct LinkCounts {
  uint32_t off1 = 0;
  uint32_t hz32 = 0;
  uint32_t hz1024 = 0;
  uint32_t hz32768 = 0;
  uint32_t off2 = 0;
};

// OFF-phase noise ceiling per 1 s window (~half the smallest commanded
// count): environmental glitch noise tolerated, no sustained front stream.
inline constexpr uint32_t LINK_OFF_MAX_COUNT = 16;
// Commanded windows: 1 s for 32/1024 Hz, 100 ms for 32768 Hz; nominal rising
// edges ~32 / ~1024 / ~3277. Accepted band: half..double the nominal —
// wide diagnostic windows, deliberately not tuned to a specific instance.
inline constexpr uint32_t LINK_32HZ_MIN = 16;
inline constexpr uint32_t LINK_32HZ_MAX = 64;
inline constexpr uint32_t LINK_1024HZ_MIN = 512;
inline constexpr uint32_t LINK_1024HZ_MAX = 2048;
inline constexpr uint32_t LINK_32KHZ_MIN = 1638;
inline constexpr uint32_t LINK_32KHZ_MAX = 6554;
// Settle time after every CLKOUT command (write + readback) before its
// measurement window starts.
inline constexpr uint32_t LINK_SETTLE_MS = 20;

inline constexpr LinkProbeResult linkFollows(const LinkCounts& c) {
  if (c.off1 > LINK_OFF_MAX_COUNT || c.off2 > LINK_OFF_MAX_COUNT) return LinkProbeResult::NotFollowed;
  if (c.hz32 < LINK_32HZ_MIN || c.hz32 > LINK_32HZ_MAX) return LinkProbeResult::NotFollowed;
  if (c.hz1024 < LINK_1024HZ_MIN || c.hz1024 > LINK_1024HZ_MAX) return LinkProbeResult::NotFollowed;
  if (c.hz32768 < LINK_32KHZ_MIN || c.hz32768 > LINK_32KHZ_MAX) return LinkProbeResult::NotFollowed;
  return LinkProbeResult::Followed;
}

// Median of the ten stored window counts (intentionally exact integer math).
inline uint32_t medianOf10(const uint32_t (&counts)[10]) {
  uint32_t sorted[10];
  for (int i = 0; i < 10; i++) sorted[i] = counts[i];
  std::sort(sorted, sorted + 10);
  return (sorted[4] + sorted[5]) / 2;
}

// The verdict contract (EXT_OSC owns the acceptance):
//   Pass    — RTC reachable, CLKOUT control verified, and the ESP32-S3
//             EXT_OSC calibration measured a clock inside the window,
//             regardless of what the ordinary digital GPIO observes. PASS
//             claims only that the external-clock path accepted and
//             calibrated the signal in this firmware configuration — never
//             electrical compliance, waveform/common-mode conformance,
//             long-term safety, or that the RTC_SLOW_CLK was switched over
//             (deep sleep still runs off the configured slow-clock source);
//   Partial — the EXT_OSC path rejected the signal, but the ordinary
//             digital GPIO (PCNT) still sees a ~32 kHz clock on the pad;
//   Fail    — RTC/CLKOUT unavailable, or no usable 32 kHz anywhere.
//
// PCNT counts through the ordinary digital GPIO path; the EXT_OSC calibration
// is the actual acceptance test of the XTAL_32K_P input path — a signal
// valid enough for that specialized path may not cross the digital GPIO
// threshold reliably, so only EXT_OSC owns PASS.
inline constexpr Verdict classify(const bool rtcOk, const bool clkoutOk, const uint32_t rawHz,
                                  const uint32_t calHz) {
  if (!rtcOk || !clkoutOk) return Verdict::Fail;
  const bool calOk = calHz != 0 && rawWindowOk(calHz);
  if (calOk) return Verdict::Pass;
  if (rawWindowOk(rawHz)) return Verdict::Partial;
  return Verdict::Fail;
}

// Per-run measurement bundle. Every diagnostic pass begins with reset() so a
// run that fails at its first transaction classifies and displays THIS run's
// empty state, never the previous run's readings. The captured original
// CLKOUT register value is session state and is intentionally NOT here.
//
// calHz is the EXT_OSC calibration under the run's explicit pull condition
// (internal pull-up enabled: BM8563 CLKOUT is open-drain, so without a
// pull-up the square wave cannot reach high) and is the verdict input;
// calNoPullHz is the informational no-pull calibration (detects whether the
// board provides its own pull-up).
struct RunState {
  bool rtcOk = false;
  bool clkoutOk = false;
  uint32_t edgeCounts[10] = {};
  uint32_t calHz = 0;
  uint32_t calNoPullHz = 0;
  LinkCounts link{};
  LinkProbeResult linkResult = LinkProbeResult::Unavailable;
  bool adcAvailable = false;
  uint16_t adcLowMv = 0;
  uint16_t adcHighMv = 0;
  uint16_t adcSwingMv = 0;

  void reset() {
    rtcOk = false;
    clkoutOk = false;
    for (int i = 0; i < 10; i++) edgeCounts[i] = 0;
    calHz = 0;
    calNoPullHz = 0;
    link = {};
    linkResult = LinkProbeResult::Unavailable;
    adcAvailable = false;
    adcLowMv = 0;
    adcHighMv = 0;
    adcSwingMv = 0;
  }
};

}  // namespace rtc32k
