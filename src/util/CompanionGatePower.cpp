#include "CompanionGatePower.h"

#ifdef CROSSPOINT_COMPANION_GATE_AUTOLS

#include <BleCompanion.h>
#include <Logging.h>
#include <esp_attr.h>
#include <esp_pm.h>
#include <sys/types.h>

#include <cstdio>
#include <cstring>

#include "CompanionGateState.h"

namespace companiongatepower {
namespace {

constexpr const char* TAG = "GATEPWR";

// RTC_NOINIT: survives deep sleep, garbage on a cold boot — the magic guards
// every read (the SlideshowState pattern).
RTC_NOINIT_ATTR GatePowerRecord record;

// esp_pm_dump_locks writes to a stdio FILE*, and this core's stdio routes to
// the primary console (UART0), not the USJ monitor the UAT reads. The capture
// cookie appends into the fixed bounded buffer (always NUL-terminated within
// its bounds); the Sleep stats and SLEEP mode rows sit at the dump tail, so a
// truncation fails the sample instead of reporting a partial one.
Capture capture;
FILE* captureFile = nullptr;

ssize_t captureWrite(void* cookie, const char* buf, size_t len) {
  captureAppend(*static_cast<Capture*>(cookie), buf, len);
  return static_cast<ssize_t>(len);
}

FILE* captureStream() {
  if (captureFile == nullptr) {
    static const cookie_io_functions_t io = {nullptr, captureWrite, nullptr, nullptr};
    captureFile = fopencookie(&capture, "w", io);
  }
  return captureFile;
}

ssize_t logCookieWrite(void*, const char* buf, size_t len) {
  logSerial.write(reinterpret_cast<const uint8_t*>(buf), len);
  return static_cast<ssize_t>(len);
}

FILE* logFile() {
  static FILE* file = nullptr;
  if (file == nullptr) {
    static const cookie_io_functions_t io = {nullptr, logCookieWrite, nullptr, nullptr};
    file = fopencookie(nullptr, "w", io);
  }
  return file;
}

const char* stateName(const uint8_t state) {
  switch (state) {
    case static_cast<uint8_t>(blecompanion::RadioState::Advertising):
      return "advertising";
    case static_cast<uint8_t>(blecompanion::RadioState::AwaitingAuthentication):
      return "awaiting-auth";
    case static_cast<uint8_t>(blecompanion::RadioState::Connected):
      return "connected";
    default:
      return "stopped";
  }
}

const char* outcomeName(const uint8_t outcome) { return outcome == WindowPowerAbort ? "power-abort" : "timeout"; }

void ensureValidRecord() {
  if (recordValid(record)) return;
  record = GatePowerRecord{};
  record.magic = kGatePowerMagic;  // LAST: the record is valid only now
}

}  // namespace

bool beginWindow() {
  ensureValidRecord();
  esp_pm_config_t pm = {};
  pm.max_freq_mhz = kGateMaxFreqMhz;
  pm.min_freq_mhz = kGateMinFreqMhz;
  pm.light_sleep_enable = kGateLightSleepEnabled;
  const esp_err_t err = esp_pm_configure(&pm);
  if (err != ESP_OK) {
    LOG_ERR(TAG, "esp_pm_configure(240/80) failed: %s — the window keeps the stock cadence", esp_err_to_name(err));
    return false;
  }
  if (!kGateLightSleepEnabled) LOG_INF(TAG, "Gate automatic light sleep OFF");
  return true;
}

namespace {

SleepSample captureSample(bool& ok) {
  ok = false;
  SleepSample sample{};
  captureReset(capture);
  FILE* file = captureStream();
  if (file != nullptr) {
    esp_pm_dump_locks(file);
    fflush(file);
    if (!capture.truncated) ok = parseSleepStats(capture.buf, sample);
  }
  return sample;
}

}  // namespace

bool captureSleepSample(SleepSample& out) {
  bool ok = false;
  out = captureSample(ok);
  return ok;
}

Guard::Guard(blecompanion::BleCompanion& gate) : gate_(&gate), configured_(beginWindow()) {
  if (!configured_) return;
  // The entry sample anchors the window's delta in THIS boot: the esp_pm
  // counters are boot-relative, so the window must be measured against a
  // sample taken at window entry, never against a previous boot's record.
  start_ = captureSample(entrySampled_);
}

void Guard::finish(const uint8_t outcome) {
  outcome_ = outcome;
  bleState_ = static_cast<uint8_t>(gate_->status().state);
  finished_ = true;
  finalize();
}

Guard::~Guard() { finalize(); }

void Guard::finalize() {
  if (!configured_ || finalized_) return;
  finalized_ = true;
  if (!finished_) {
    // An exit path that never called finish(): count it as an abort so the
    // accounting still runs exactly once.
    outcome_ = WindowPowerAbort;
    bleState_ = static_cast<uint8_t>(blecompanion::RadioState::Stopped);
  }
  // The exit sample must be captured while automatic light sleep is still
  // enabled: the profiling dump prints the SLEEP mode row and the Sleep stats
  // block only then. In the control twin the dump hides the Sleep stats block
  // entirely, so the sample reads as a failure — that is the design, and
  // windowCounts keeps such windows out of the totals.
  bool sampled = false;
  const SleepSample end = captureSample(sampled);
  ensureValidRecord();
  WindowDelta delta{};
  const bool deltaValid = entrySampled_ && sampled && windowDelta(start_, end, delta);
  const bool counted = windowCounts(kGateLightSleepEnabled, entrySampled_, sampled, deltaValid);
  commitWindow(record, delta, counted, bleState_, outcome_);
  // P1-B9 zero-console rule: the standby env compiles the Guard's routine
  // exit lines out (the diagnostic buffer records the exit); error paths and
  // the other envs are unchanged.
#ifndef CROSSPOINT_COMPANION_GATE_STANDBY
  if (!kGateLightSleepEnabled) {
    LOG_INF(TAG, "window ended: ble=%s end=%s (light sleep off — counters not accumulated)", stateName(bleState_),
            outcomeName(outcome_));
  } else if (counted && delta.entries == 0 && delta.rejects == 0) {
    LOG_INF(TAG, "window %u: no light sleep entry (rejects 0) — sleep never engaged",
            static_cast<unsigned>(record.windows));
  }
#endif
  esp_pm_config_t pm = {};
  pm.max_freq_mhz = kGateMaxFreqMhz;
  pm.min_freq_mhz = kGateMinFreqMhz;
  pm.light_sleep_enable = false;
  const esp_err_t err = esp_pm_configure(&pm);
  if (err != ESP_OK) LOG_ERR(TAG, "esp_pm_configure(OFF) failed: %s", esp_err_to_name(err));
}

void handleCommand(const char* command) {
  if (command == nullptr || strcmp(command, "GATESTAT") != 0) return;
  ensureValidRecord();
  if (kGateLightSleepEnabled) {
    LOG_INF(TAG, "windows=%u entries=%u rejects=%u noSleep=%u sleepUs=%u", static_cast<unsigned>(record.windows),
            static_cast<unsigned>(record.entries), static_cast<unsigned>(record.rejects),
            static_cast<unsigned>(record.noSleepRuns), static_cast<unsigned>(record.sleepUs));
  } else {
    // Control build: the counters describe earlier experimental flashes only —
    // a window without light sleep is the design here, not a metrics failure.
    LOG_INF(TAG, "Gate automatic light sleep OFF (control build: windows=%u is not accumulated here)",
            static_cast<unsigned>(record.windows));
  }
  LOG_INF(TAG, "last window: ble=%s end=%s", stateName(record.lastBleState), outcomeName(record.lastOutcome));
  // The persisted dispatch verdict of the last timer wake: the inputs the
  // policy saw, its decision, and (for Run) why the window attempt ended.
  // Bond presence is derived, never stored: a completed window or a power
  // abort implies the bond held, the no-bond outcome implies it did not.
  companiongate::DispatchVerdict verdict{};
  if (companiongate::readDispatchVerdict(verdict)) {
    const char* bond = "n/a";
    if (verdict.decision == companiongate::Action::Run) {
      bond = verdict.outcome == companiongate::RunOutcome::NoBond ? "no"
             : (verdict.outcome == companiongate::RunOutcome::WindowTimeout ||
                verdict.outcome == companiongate::RunOutcome::PowerAbort)
                 ? "yes"
                 : "n/a";
    }
    // M3 (the established BLE session) is only meaningful for a window that
    // actually ran; every other decision reads n/a.
    const char* m3 =
        verdict.decision == companiongate::Action::Run ? (verdict.sessionEstablished ? "yes" : "no") : "n/a";
    LOG_INF(TAG,
            "last timer-wake dispatch: wake=%s armed=%d diag=%d recovery=%d usb=%d slideshow=%d identity=%d "
            "battery=%d -> %s run=%s bond=%s m3=%s",
            verdict.wakeTimer ? "timer" : "other", verdict.armValid, verdict.diagnostic, verdict.recovery, verdict.usb,
            verdict.slideshow, verdict.identity, verdict.battery, companiongate::gateActionName(verdict.decision),
            companiongate::runOutcomeName(verdict.outcome), bond, m3);
    if (!verdict.armValid) {
      LOG_INF(TAG, "  identity/battery not sampled: the wake carried no arm");
    }
  } else {
    LOG_INF(TAG, "last timer-wake dispatch: none (cold boot, GATEARM pending, or never armed) m3=n/a");
  }
  // The live PM-lock snapshot: with automatic light sleep off at boot the
  // dump carries the lock table (the SLEEP mode row stays hidden), which is
  // the "why would the next window sleep" answer on demand.
  FILE* file = logFile();
  if (file == nullptr) {
    LOG_ERR(TAG, "GATESTAT: could not create the serial dump stream");
    return;
  }
  esp_pm_dump_locks(file);
  fflush(file);
}

}  // namespace companiongatepower
#endif  // CROSSPOINT_COMPANION_GATE_AUTOLS
