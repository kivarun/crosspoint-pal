#pragma once

// Pure classification policy for the X4C RTC 32 kHz diagnostics screen
// (temporary solder-joint verification firmware, experiment branch only).
// Host-testable: no Arduino/ESP includes.

#include <algorithm>
#include <cstdint>

namespace rtc32k {

// Wide raw-signal acceptance window (kHz-class clock, not a precision gate).
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

// Median of the ten stored window counts (intentionally exact integer math).
inline uint32_t medianOf10(const uint32_t (&counts)[10]) {
  uint32_t sorted[10];
  for (int i = 0; i < 10; i++) sorted[i] = counts[i];
  std::sort(sorted, sorted + 10);
  return (sorted[4] + sorted[5]) / 2;
}

// The verdict contract:
//   Pass    — RTC reachable, CLKOUT control verified, raw signal inside the
//             window AND the ext-osc calibration accepted the same signal;
//   Partial — a usable ~32 kHz signal is present on the pin but the ESP32
//             ext-osc calibration rejected it (calibration timeout/0 or out
//             of window);
//   Fail    — RTC/CLKOUT unavailable, or no ~32 kHz on the pin.
inline constexpr Verdict classify(const bool rtcOk, const bool clkoutOk, const uint32_t rawHz,
                                  const uint32_t calHz) {
  if (!rtcOk || !clkoutOk) return Verdict::Fail;
  const bool rawOk = rawWindowOk(rawHz);
  const bool calOk = calHz != 0 && rawWindowOk(calHz);
  if (rawOk && calOk) return Verdict::Pass;
  if (rawOk) return Verdict::Partial;
  return Verdict::Fail;
}

}  // namespace rtc32k
