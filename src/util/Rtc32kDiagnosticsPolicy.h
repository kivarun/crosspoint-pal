#pragma once

// Pure policy for the X4C XTAL32K diagnostics screen (experiment branch only).
// GPIO15/GPIO16 are the ESP32-S3 XTAL_32K_P/N pads feeding the internal 32 kHz
// crystal oscillator; the screen drives that oscillator and reports raw
// RTC-calibration measurements. Host-testable: no Arduino/ESP includes.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace rtc32k {

// Q19.13 fractional bits of the rtc_clk_cal() period result — mirrors
// RTC_CLK_CAL_FRACT from soc/rtc.h (duplicated here so this header stays
// host-testable without ESP-IDF includes).
inline constexpr uint32_t CAL_FRACT = 19;

// rtc_clk_cal() returns the average period of the calibrated clock in
// microseconds, Q19.13 fixed point (soc/rtc.h). 0 is the "no measurement"
// sentinel (hardware timeout or failed 32k validity check) and stays 0 —
// a zero period must never display as a valid frequency.
inline constexpr uint32_t periodToHz(const uint32_t periodQ19) {
  return periodQ19 == 0 ? 0 : static_cast<uint32_t>((1000000ULL << CAL_FRACT) / periodQ19);
}

// Wide acceptance window for a kHz-class 32 kHz reading (not a precision gate).
inline constexpr uint32_t RAW_WINDOW_MIN_HZ = 30000;
inline constexpr uint32_t RAW_WINDOW_MAX_HZ = 35000;

inline constexpr bool rawWindowOk(const uint32_t hz) { return hz >= RAW_WINDOW_MIN_HZ && hz <= RAW_WINDOW_MAX_HZ; }

// --- calibration run ---------------------------------------------------------

// rtc_clk_cal(RTC_CAL_32K_XTAL, slowclk_cycles) per measurement: 1024 cycles
// ≈ 31 ms of counting per measurement at 32768 Hz (~0.03 Hz quantization),
// with a bounded hardware timeout (~105 ms) when no clock is running.
inline constexpr uint32_t CAL_CYCLES = 1024;

// Independent calibration measurements per Calibrate action.
inline constexpr size_t CAL_SAMPLE_COUNT = 5;

// Oscillator startup wait before the first measurement of a run: 32.768 kHz
// tuning-fork crystals start in ~0.1–1 s.
inline constexpr uint32_t XTAL_STARTUP_SETTLE_MS = 500;

// Max spread across the successful samples still reported as stable.
// Generous by design: this catches a wandering or jamming signal, not
// metrology (a healthy 32.768 kHz crystal spreads ~1–2 Hz over five runs).
inline constexpr uint32_t STABLE_SPREAD_HZ = 8;

enum class CalStatus : uint8_t { NotRun = 0, Timeout = 1, Done = 2 };
enum class CalQuality : uint8_t { Unknown = 0, Stable = 1, Unstable = 2, OutOfRange = 3 };

// Result of one Calibrate action: up to CAL_SAMPLE_COUNT independent
// measurements of the XTAL32K clock, each stored individually.
struct CalRun {
  uint32_t hz[CAL_SAMPLE_COUNT] = {};  // measured frequency; 0 = no sample
  bool ok[CAL_SAMPLE_COUNT] = {};      // sample produced a measurement
  size_t count = 0;                    // attempted samples (0..CAL_SAMPLE_COUNT)

  void beginRun() { *this = CalRun{}; }

  void record(const bool measured, const uint32_t sampleHz) {
    if (count >= CAL_SAMPLE_COUNT) return;
    ok[count] = measured;
    hz[count] = measured ? sampleHz : 0;
    ++count;
  }

  size_t okCount() const {
    size_t n = 0;
    for (size_t i = 0; i < count && i < CAL_SAMPLE_COUNT; i++) {
      if (ok[i]) n++;
    }
    return n;
  }

  bool complete() const { return count >= CAL_SAMPLE_COUNT; }
};

// Median of the successful samples; 0 when none succeeded.
inline uint32_t calMedianHz(const CalRun& run) {
  uint32_t sorted[CAL_SAMPLE_COUNT];
  size_t n = 0;
  for (size_t i = 0; i < run.count && i < CAL_SAMPLE_COUNT; i++) {
    if (run.ok[i]) sorted[n++] = run.hz[i];
  }
  if (n == 0) return 0;
  std::sort(sorted, sorted + n);
  return n % 2 == 1 ? sorted[n / 2] : (sorted[n / 2 - 1] + sorted[n / 2]) / 2;
}

// NotRun — no measurements attempted; Timeout — attempted but none succeeded
// (oscillator not running, hardware timeout, or failed validity check);
// Done — at least one sample measured.
inline constexpr CalStatus calStatus(const CalRun& run) {
  if (run.count == 0) return CalStatus::NotRun;
  return run.okCount() > 0 ? CalStatus::Done : CalStatus::Timeout;
}

// Honest quality classification of a Done run (never a PASS claim):
//   Stable     — median inside the window and the samples agree;
//   Unstable   — median inside the window but the samples wander;
//   OutOfRange — median outside the window.
inline CalQuality calQuality(const CalRun& run) {
  if (calStatus(run) != CalStatus::Done) return CalQuality::Unknown;
  const uint32_t median = calMedianHz(run);
  if (!rawWindowOk(median)) return CalQuality::OutOfRange;
  uint32_t lo = UINT32_MAX;
  uint32_t hi = 0;
  for (size_t i = 0; i < run.count && i < CAL_SAMPLE_COUNT; i++) {
    if (!run.ok[i]) continue;
    lo = run.hz[i] < lo ? run.hz[i] : lo;
    hi = run.hz[i] > hi ? run.hz[i] : hi;
  }
  return (hi - lo) <= STABLE_SPREAD_HZ ? CalQuality::Stable : CalQuality::Unstable;
}

// --- session state -----------------------------------------------------------

// Diagnostics session state. The activity is the only device-side mutator and
// calls the hardware API exactly where these transitions are invoked; the
// transitions themselves are the host-tested contract.
struct DiagState {
  bool xtalOn = false;                 // generator state shown on screen
  bool enabledByThisActivity = false;  // exit cleanup owns the undo
  CalRun cal{};

  void reset() { *this = DiagState{}; }

  void markEnabled() {
    xtalOn = true;
    enabledByThisActivity = true;
  }

  void markDisabled() {
    xtalOn = false;
    enabledByThisActivity = false;
  }
};

// Exit-cleanup contract: undo the oscillator enable only when THIS activity
// enabled it — never touch a generator the rest of the firmware runs.
inline constexpr bool shouldDisableOnExit(const DiagState& state) { return state.enabledByThisActivity; }

// --- display formatting ------------------------------------------------------

inline const char* stateLabel(const bool on) { return on ? "ON" : "OFF"; }

// RTC slow clock source (read-only display). Numeric values mirror
// soc_rtc_slow_clk_src_t on the S3.
enum class SlowClkSource : uint8_t { RcSlow = 0, Xtal32k = 1, RcFastD256 = 2, Invalid = 3 };

inline const char* slowClockSourceName(const SlowClkSource src) {
  switch (src) {
    case SlowClkSource::RcSlow:
      return "RC SLOW";
    case SlowClkSource::Xtal32k:
      return "XTAL32K";
    case SlowClkSource::RcFastD256:
      return "RC FAST D256";
    default:
      return "UNKNOWN";
  }
}

// Per-sample row value: "-" before that sample, "TIMEOUT" when the
// calibration produced no measurement, "<n> Hz" otherwise.
inline void formatCalSample(char* buf, const size_t n, const CalRun& run, const size_t index) {
  if (index >= run.count) {
    snprintf(buf, n, "-");
    return;
  }
  if (!run.ok[index]) {
    snprintf(buf, n, "TIMEOUT");
    return;
  }
  snprintf(buf, n, "%lu Hz", static_cast<unsigned long>(run.hz[index]));
}

// Median line: "NOT RUN" / "TIMEOUT" / "<n> Hz" — never "0 Hz".
inline void formatCalMedian(char* buf, const size_t n, const CalRun& run) {
  switch (calStatus(run)) {
    case CalStatus::NotRun:
      snprintf(buf, n, "NOT RUN");
      break;
    case CalStatus::Timeout:
      snprintf(buf, n, "TIMEOUT");
      break;
    case CalStatus::Done:
      snprintf(buf, n, "%lu Hz", static_cast<unsigned long>(calMedianHz(run)));
      break;
  }
}

// Result row: describes what the calibration saw, without claiming the
// crystal is good unless every check agrees.
inline const char* resultLabel(const CalRun& run) {
  switch (calStatus(run)) {
    case CalStatus::NotRun:
      return "NOT RUN";
    case CalStatus::Timeout:
      return "TIMEOUT - NO CLOCK";
    case CalStatus::Done:
      break;
  }
  switch (calQuality(run)) {
    case CalQuality::Stable:
      return "STABLE";
    case CalQuality::Unstable:
      return "UNSTABLE";
    default:
      return "OUT OF RANGE";
  }
}

}  // namespace rtc32k
