#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "BleCompanion.h"

// BLE-aware automatic light sleep for the Sleep Companion gate window
// (P1-B2a, experimental). The gate window is the one governed "BLE waiting"
// state in the firmware: the radio advertises for the bounded window with the
// identity/bond/battery/USB preconditions already checked by
// companiongate::dispatch. This module adds, per build flag, automatic light
// sleep across that window: esp_pm_configure(240/80, ON) at window open and
// OFF again on every exit, a main-loop poll period that leaves tickless idle
// windows above the 8 ms esp_pm sleep gate, and persistent (RTC) sleep
// accounting read from the esp_pm profiling dump.
//
// No PM lock is created here: the sleep permit comes from the gate window
// itself, and the USB-MSC session lock (HalStorage, CROSSPOINT_USB_SESSION_PM_LOCK)
// stays the sole owner of that policy. Everything that would hold a lock or
// print per idle loop is deliberately absent.
//
// The pure parts below carry no device headers, so host tests include them
// directly; the device API is exercised only in the experimental env.

namespace companiongatepower {

// The gate window keeps CPU and APB on the PLL between BLE events: the 40 MHz
// XTAL floor is the p1-b1l USB-MSC failure mode, so the floor is 80.
inline constexpr int kGateMaxFreqMhz = 240;
inline constexpr int kGateMinFreqMhz = 80;

// Main-loop poll period inside the window: the stock 2 ms cadence never gives
// the idle task the 8 ms window esp_pm's sleep gate needs (pm_impl.c:872), so
// an automatic-light-sleep window polls at 15 ms.
inline constexpr unsigned kStockPollMs = 2;
inline constexpr unsigned kAutolsPollMs = 15;
inline unsigned gatePollMs(const bool autolsWindow) { return autolsWindow ? kAutolsPollMs : kStockPollMs; }

// Why the gate window ended. Values are stable across builds (stored in RTC).
enum WindowOutcome : uint8_t {
  WindowTimeout = 1,     // the full window ran out
  WindowPowerAbort = 2,  // the power button aborted the window
};

// A cumulative sample of the esp_pm profiling counters, parsed from one
// esp_pm_dump_locks capture. `entries`/`rejects` are the boot-relative
// light_sleep_counts / light_sleep_reject_counts; `modeUs` is the
// boot-relative LIGHT_SLEEP mode time, or -1 when the dump carried no SLEEP
// row (automatic light sleep was off at dump time).
struct SleepSample {
  long entries = 0;
  long rejects = 0;
  long long modeUs = -1;
};

// Parses one captured esp_pm_dump_locks body (the profiling build's text).
// False when the Sleep stats line is missing entirely (profiling disabled or
// a truncated capture); the SLEEP mode row is optional and reads as modeUs<0.
inline bool parseSleepStats(const char* text, SleepSample& out) {
  out = SleepSample{};
  if (text == nullptr) return false;
  const char* counts = strstr(text, "light_sleep_counts:");
  if (counts == nullptr) return false;
  if (sscanf(counts, "light_sleep_counts:%ld  light_sleep_reject_counts:%ld", &out.entries, &out.rejects) != 2) {
    return false;
  }
  // The LIGHT_SLEEP mode row is the only line starting with "SLEEP": the
  // mode-stats table is the first dump section with that prefix.
  const char* row = strstr(text, "\nSLEEP ");
  if (row != nullptr) {
    // Row shape: name, freq digits, 'M' (one token when freq >= 100, two
    // below), the mode time, the percent. Parse after the first digit run.
    const char* p = row + 1;
    while (*p != '\0' && (*p < '0' || *p > '9')) ++p;
    if (*p != '\0') {
      char* end = nullptr;
      (void)strtoul(p, &end, 10);  // frequency; layout continues at the 'M'
      while (end != nullptr && *end == ' ') ++end;
      if (end != nullptr && *end == 'M') {
        out.modeUs = strtoll(end + 1, nullptr, 10);
      }
    }
  }
  return true;
}

// The persistent accounting record (RTC_NOINIT, SlideshowState pattern: a
// magic written last is the only validity proof). 32 B — the rtc_slow_seg of
// the rebuilt-core envs has 36 B of headroom (MscDiag.h), so the mode-time
// baseline is a 32-bit microsecond counter whose wrap skips one sample.
struct GatePowerRecord {
  uint32_t magic;        // kGatePowerMagic, written LAST
  uint32_t windows;      // completed sleep-capable gate windows counted
  uint32_t entries;      // accumulated light-sleep entries
  uint32_t rejects;      // accumulated rejected sleep attempts
  uint32_t lastEntries;  // previous cumulative sample (the window baseline)
  uint32_t lastRejects;
  uint32_t lastModeUs;   // previous cumulative LIGHT_SLEEP mode time (wraps ~71.6 min of sleep)
  uint8_t lastBleState;  // blecompanion::RadioState at the window's last poll
  uint8_t lastOutcome;   // WindowOutcome
  uint8_t noSleepRuns;   // windows that counted but produced neither entry nor reject
  uint8_t primed;        // a baseline sample exists
};
static_assert(sizeof(GatePowerRecord) == 32, "rtc_slow_seg headroom");
static_assert(offsetof(GatePowerRecord, magic) == 0, "record layout: magic first");
static_assert(offsetof(GatePowerRecord, lastBleState) == 28, "record layout: byte tail");

inline constexpr uint32_t kGatePowerMagic = 0x47415057u;  // 'GAPW'

inline bool recordValid(const GatePowerRecord& record) { return record.magic == kGatePowerMagic; }

// The delta of one window against the record's baseline. Returns false when
// the sample must not count: the record was never primed (the baseline is
// boot-relative, so the first sample only establishes it), or a counter went
// backwards (wrap or RTC garbage; skipped, p1-b1q semantics — the baseline
// stays as it was).
struct WindowDelta {
  long entries = 0;
  long rejects = 0;
  long long modeUs = 0;
};

inline bool windowDelta(const GatePowerRecord& record, const SleepSample& sample, WindowDelta& out) {
  out = WindowDelta{};
  if (record.primed == 0) return false;
  if (sample.entries < 0 || sample.entries < static_cast<long>(record.lastEntries)) return false;
  if (sample.rejects < static_cast<long>(record.lastRejects)) return false;
  out.entries = sample.entries - static_cast<long>(record.lastEntries);
  out.rejects = sample.rejects - static_cast<long>(record.lastRejects);
  if (sample.modeUs >= 0 && sample.modeUs >= static_cast<long long>(record.lastModeUs)) {
    out.modeUs = sample.modeUs - static_cast<long long>(record.lastModeUs);
  }
  return true;
}

// How a sampled window commits: Prime stores the first baseline without
// counting (the counters are boot-relative), Count applies the delta, Skip
// records only the end state — a dump that failed, or a backwards counter
// (wrap or RTC garbage) whose baseline must stay as it was (p1-b1q
// semantics: excluded from the sums, not summed as a wrap).
enum class SampleRole : uint8_t { Prime, Count, Skip };

inline void commitWindow(GatePowerRecord& record, const SleepSample& sample, const SampleRole role,
                         const WindowDelta& delta, const uint8_t bleState, const uint8_t outcome) {
  if (role == SampleRole::Prime) {
    record.lastEntries = static_cast<uint32_t>(sample.entries);
    record.lastRejects = static_cast<uint32_t>(sample.rejects);
    record.lastModeUs = sample.modeUs >= 0 ? static_cast<uint32_t>(sample.modeUs) : 0xFFFFFFFFu;
    record.primed = 1;
  } else if (role == SampleRole::Count) {
    record.windows = record.windows + 1;
    record.entries = record.entries + static_cast<uint32_t>(delta.entries);
    record.rejects = record.rejects + static_cast<uint32_t>(delta.rejects);
    if (delta.entries == 0 && delta.rejects == 0 && record.noSleepRuns != 0xFF)
      record.noSleepRuns = record.noSleepRuns + 1;
    record.lastEntries = static_cast<uint32_t>(sample.entries);
    record.lastRejects = static_cast<uint32_t>(sample.rejects);
    if (sample.modeUs >= 0) record.lastModeUs = static_cast<uint32_t>(sample.modeUs);
  }
  record.lastBleState = bleState;
  record.lastOutcome = outcome;
}

// --- device API ---

#ifdef CROSSPOINT_COMPANION_GATE_AUTOLS

// Configures 240/80 MHz with automatic light sleep ON for the gate window.
// False when esp_pm_configure failed: the caller then keeps the stock poll
// cadence and the window records nothing.
bool beginWindow();

// One exit of the gate window. The guard reads the gate's live radio state at
// finish() (before the caller stops the radio) and the destructor records the
// sample — while automatic light sleep is still on, so the profiling dump
// carries the SLEEP row — and restores the OFF state.
class Guard {
 public:
  explicit Guard(blecompanion::BleCompanion& gate);
  ~Guard();
  Guard(const Guard&) = delete;
  Guard& operator=(const Guard&) = delete;
  // False when esp_pm_configure failed at construction: the window keeps the
  // stock poll cadence and records nothing.
  bool configured() const { return configured_; }
  void finish(uint8_t outcome);

 private:
  blecompanion::BleCompanion* gate_;
  bool configured_ = false;
  bool finished_ = false;
  uint8_t outcome_ = WindowTimeout;
  uint8_t bleState_ = 0;
};

// Serial diagnostic ("GATESTAT"): accumulated totals, the last window's BLE
// state and outcome, and a full PM-lock dump snapshot.
void handleCommand(const char* command);

#else  // builds without the flag: the stock window, no accounting

class Guard {
 public:
  explicit Guard(blecompanion::BleCompanion&) {}
  ~Guard() = default;
  Guard(const Guard&) = delete;
  Guard& operator=(const Guard&) = delete;
  bool configured() const { return false; }
  void finish(uint8_t) {}
};

inline void handleCommand(const char*) {}

#endif

}  // namespace companiongatepower
