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
// sleep across that window: esp_pm_configure(240/80) at window open — with
// light sleep enabled in the experimental build, disabled in the control twin
// (CROSSPOINT_COMPANION_GATE_NO_LIGHT_SLEEP) — and
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

// P1-B3 control twin: the gate window keeps the identical flow, cadence and
// GATESTAT, but esp_pm_configure runs with light sleep disabled, so the BLE
// connection test isolates the single light-sleep variable. Defined only by
// the x4c-bleautols-control environment; the experimental build leaves it
// unset.
#ifdef CROSSPOINT_COMPANION_GATE_NO_LIGHT_SLEEP
inline constexpr bool kGateLightSleepEnabled = false;
#else
inline constexpr bool kGateLightSleepEnabled = true;
#endif
inline bool autoLightSleepEnabled() { return kGateLightSleepEnabled; }

// Whether one window accumulates into the sleep totals. A control window
// (light sleep off by design) counts nothing — its dump carries no Sleep
// stats block at all, so it must not be mistaken for the experimental build's
// metrics failure — and a metrics failure (missing samples or an invalid
// delta) records only the end state.
inline bool windowCounts(const bool lightSleepEnabled, const bool entrySampled, const bool exitSampled,
                         const bool deltaValid) {
  return lightSleepEnabled && entrySampled && exitSampled && deltaValid;
}

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
// False when the Sleep stats line is missing entirely (profiling disabled, a
// truncated capture, or light sleep off — the control twin's dumps never
// carry the block); the SLEEP mode row is optional and reads as modeUs<0.
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

// The bounded capture buffer for one esp_pm_dump_locks pass. The parser
// reads the buffer with strstr/sscanf, so the buffer is always NUL-terminated
// within its own bounds: no stale tail of a previous, longer capture is ever
// visible, and the scan can never run past the buffer.
inline constexpr size_t kCaptureBufSize = 2048;
// One byte of the buffer is permanently reserved for the terminator.
inline constexpr size_t kCaptureCapacity = kCaptureBufSize - 1;

struct Capture {
  char buf[kCaptureBufSize];
  size_t len = 0;
  bool truncated = false;
};

inline void captureReset(Capture& capture) {
  capture.len = 0;
  capture.truncated = false;
  capture.buf[0] = '\0';
}

// Appends one write() chunk. An exact-capacity capture is not truncated: the
// terminator takes the reserved byte. A chunk that does not fit marks the
// capture truncated but still leaves the buffer terminated.
inline void captureAppend(Capture& capture, const char* data, const size_t len) {
  const size_t room = capture.len < kCaptureCapacity ? kCaptureCapacity - capture.len : 0;
  if (len > room) capture.truncated = true;
  if (room > 0) {
    const size_t copied = len < room ? len : room;
    memcpy(capture.buf + capture.len, data, copied);
    capture.len += copied;
  }
  capture.buf[capture.len] = '\0';
}

// The persistent accounting record (RTC_NOINIT, SlideshowState pattern: a
// magic written last is the only validity proof). RTC stores accumulated
// results only — the per-window baseline lives in the Guard's RAM, so the
// boot-relative esp_pm counters of different boots are never subtracted.
//
// The magic versions the layout: 'GAP2' is the 28-byte accumulated-results
// record. The retired 'GAPW' record (32 bytes) held the cross-boot baseline
// at offsets 16-27, so reinterpreting it would seed noSleepRuns and sleepUs
// with garbage that no commit ever corrects; such records are reset instead.
struct GatePowerRecord {
  uint32_t magic;        // kGatePowerMagic, written LAST
  uint32_t windows;      // completed sleep-capable gate windows counted
  uint32_t entries;      // accumulated light-sleep entries
  uint32_t rejects;      // accumulated rejected sleep attempts
  uint32_t noSleepRuns;  // windows that counted but produced neither entry nor reject
  uint32_t sleepUs;      // accumulated LIGHT_SLEEP mode time across counted windows
  uint8_t lastBleState;  // blecompanion::RadioState at the window's last poll
  uint8_t lastOutcome;   // WindowOutcome
};
static_assert(sizeof(GatePowerRecord) == 28, "rtc_slow_seg headroom");
static_assert(offsetof(GatePowerRecord, magic) == 0, "record layout: magic first");
static_assert(offsetof(GatePowerRecord, lastBleState) == 24, "record layout: byte tail");

inline constexpr uint32_t kGatePowerMagicV1 = 0x47415057u;  // 'GAPW', retired with the pre-B2h layout
inline constexpr uint32_t kGatePowerMagic = 0x47415032u;    // 'GAP2'

inline bool recordValid(const GatePowerRecord& record) { return record.magic == kGatePowerMagic; }

// The delta of one window against the sample captured at window entry — both
// samples are boot-relative counters from the SAME boot, so the subtraction
// never crosses a deep-sleep reset. Returns false when a counter went
// backwards within the boot (wrap or garbage): the caller then does not count
// the window. Sample failures (a dump that did not parse) never reach this
// function — the caller excludes them through its counted condition.
struct WindowDelta {
  long entries = 0;
  long rejects = 0;
  long long modeUs = 0;
};

inline bool windowDelta(const SleepSample& start, const SleepSample& end, WindowDelta& out) {
  out = WindowDelta{};
  if (end.entries < start.entries || end.rejects < start.rejects) return false;
  out.entries = end.entries - start.entries;
  out.rejects = end.rejects - start.rejects;
  if (start.modeUs >= 0 && end.modeUs >= start.modeUs) {
    out.modeUs = end.modeUs - start.modeUs;
  }
  return true;
}

// How a window commits: a counted window adds its delta to the accumulated
// totals; an uncounted one (failed samples or backwards counters) records only
// the end state. Either way the window ends the record's lastBleState /
// lastOutcome — an accounting failure is visible, not silent.
inline void commitWindow(GatePowerRecord& record, const WindowDelta& delta, const bool counted, const uint8_t bleState,
                         const uint8_t outcome) {
  if (counted) {
    record.windows = record.windows + 1;
    record.entries = record.entries + static_cast<uint32_t>(delta.entries);
    record.rejects = record.rejects + static_cast<uint32_t>(delta.rejects);
    if (delta.entries == 0 && delta.rejects == 0) record.noSleepRuns = record.noSleepRuns + 1;
    if (delta.modeUs > 0) record.sleepUs = record.sleepUs + static_cast<uint32_t>(delta.modeUs);
  }
  record.lastBleState = bleState;
  record.lastOutcome = outcome;
}

// --- device API ---

#ifdef CROSSPOINT_COMPANION_GATE_AUTOLS

// Configures 240/80 MHz for the gate window with the build's light-sleep
// mode: enabled in the experimental build, disabled in the control twin
// (kGateLightSleepEnabled). False when esp_pm_configure failed: the caller
// then keeps the stock poll cadence and the window records nothing.
bool beginWindow();

// One exit of the gate window. finish() is the finalization point: it reads
// the gate's live radio state (before the caller stops the radio), captures
// the exit sample while the window's esp_pm mode still holds (the profiling
// dump carries the Sleep stats block only while light sleep is enabled; the
// control twin's capture reads as a failure by design), commits the window's
// delta against the entry sample, and restores the OFF state — so a window
// that ends in the non-returning deep-sleep handoff is accounted before the
// sleep. The destructor only finalizes windows whose exit path never called
// finish() (returning abort paths); finalization is idempotent.
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
  void finalize();
  blecompanion::BleCompanion* gate_;
  bool configured_ = false;
  bool finished_ = false;
  bool finalized_ = false;
  bool entrySampled_ = false;
  uint8_t outcome_ = WindowTimeout;
  uint8_t bleState_ = 0;
  SleepSample start_{};
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
