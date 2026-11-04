#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Pure support core for the dev-only RtcClockDiag (p0-rtc-clock): the
// cycle-interval math and an RTC-retained log ring. No Arduino or ESP
// headers, so the host tests compile this directly.
namespace rtcclockdiag {

// Q13.19 period format shared with IDF's rtc_clk_cal / rtc_time_*.
inline constexpr uint32_t kCalFract = 19;

struct CycleSample {
  uint64_t requestedUs;   // interval the alarm was armed for
  uint32_t sleepCal;      // Q13.19 period stamped at sleep entry
  uint64_t elapsedTicks;  // raw RTC counter delta across the sleep
};

// The period the alarm conversion effectively used: requested*2^19/ticks.
// Near the 32k period the alarm matched the sleep clock; near the RC period
// (about 4.26x larger on this board) it is the long-sleep signature.
inline uint64_t effectiveCalPeriod(const CycleSample& s) {
  if (s.elapsedTicks == 0) return 0;
  return (s.requestedUs << kCalFract) / s.elapsedTicks;
}

// Sleep duration in microseconds, converted from the raw tick delta with the
// calibration that was active while the counter was running.
inline int64_t measuredUs(const CycleSample& s) {
  if (s.sleepCal == 0) return 0;
  return static_cast<int64_t>((s.elapsedTicks * s.sleepCal) >> kCalFract);
}

inline int64_t cycleErrorUs(const CycleSample& s) { return measuredUs(s) - static_cast<int64_t>(s.requestedUs); }

inline int64_t cycleErrorPpm(const CycleSample& s) {
  if (s.requestedUs == 0) return 0;
  return (cycleErrorUs(s) * 1000000LL) / static_cast<int64_t>(s.requestedUs);
}

// Fixed-capacity log ring for RTC_NOINIT storage: magic-guarded like the
// CompanionGate arm record, oldest lines overwritten first.
template <size_t kLines, size_t kLineLen>
class DiagLogRing {
 public:
  void reset(uint32_t stamp) {
    for (size_t i = 0; i < kLines; ++i) lines_[i][0] = '\0';
    head_ = 0;
    stamp_ = stamp;
  }

  bool ok() const { return stamp_ != 0 && head_ < kLines; }

  void push(uint32_t stamp, const char* line) {
    if (stamp == 0 || !ok() || stamp_ != stamp) {
      reset(stamp);
    }
    strncpy(lines_[head_], line, kLineLen - 1);
    lines_[head_][kLineLen - 1] = '\0';
    head_ = (head_ + 1) % kLines;
  }

  // Oldest-first walk over live lines; fn returns false to stop.
  template <typename Fn>
  void forEach(Fn fn) const {
    if (!ok()) return;
    for (size_t i = 0; i < kLines; ++i) {
      const char* line = lines_[(head_ + i) % kLines];
      if (line[0] != '\0' && !fn(line)) return;
    }
  }

 private:
  uint32_t stamp_ = 0;
  size_t head_ = 0;
  char lines_[kLines][kLineLen] = {};
};

}  // namespace rtcclockdiag
