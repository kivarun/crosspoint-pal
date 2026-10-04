#pragma once

// Pure classification policy for the X4C RTC 32 kHz diagnostics screen
// (temporary solder-joint verification firmware, experiment branch only).
// Host-testable: no Arduino/ESP includes.

#include <algorithm>
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
//             regardless of what the ordinary digital GPIO observes;
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

}  // namespace rtc32k
