#include "RtcClockDiag.h"

#ifdef CROSSPOINT_RTC_CLK_DIAG

#include <Arduino.h>
#include <Logging.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <soc/rtc.h>
#include <soc/rtc_cntl_reg.h>

#include <cstdarg>
#include <cstdio>

#include "RtcClockDiagCore.h"
#include "esp_private/esp_clk.h"

namespace rtcclockdiag {
namespace {

constexpr uint32_t SCRATCH_MAGIC = 0x4B4C4352U;  // "RCLK" little-endian
constexpr uint32_t kSettleMs = 500;              // cold 32k crystal startup window
constexpr uint32_t kCalCycles32k = 2048;         // ~62.5 ms against the 40 MHz XTAL
constexpr uint32_t kCalCyclesLiveness = 128;     // ~4 ms gate for a cached calibration
constexpr uint32_t kCalCyclesRc = 576;           // matches CONFIG_RTC_CLK_CAL_CYCLES
// Sanity band around 32768 Hz for the cached calibration (~1250 ppm total).
constexpr uint32_t kCal32kMin = 15980000;
constexpr uint32_t kCal32kMax = 16020000;
constexpr uint32_t kMaxLoopCycles = 10000;
constexpr uint32_t kMinIntervalMs = 100;
constexpr uint32_t kMaxIntervalMs = 3600000;

enum class Selection : uint8_t { Production = 0, ForceRc = 1, ForceXtal32k = 2 };

// Survives deep sleep, garbage on a cold boot — the magic guards every read
// (CompanionGateState pattern). All 32-bit fields: no alignment surprises.
struct Scratch {
  uint32_t magic;     // written LAST; 0/garbage on a cold boot
  uint8_t selection;  // Selection
  uint8_t loopArmed;
  uint16_t reserved;
  uint32_t good32kCal;  // cached 32k calibration; 0 forces a fresh one
  uint32_t cyclesRemaining;
  uint32_t intervalMs;
  uint32_t cycleIndex;
  uint32_t t0TicksLo;  // raw RTC counter at sleep entry
  uint32_t t0TicksHi;
  uint32_t sleepCal;  // calibration value that scaled the sleep
};

RTC_NOINIT_ATTR Scratch scratch;

static_assert(sizeof(Scratch) == 36, "Scratch layout is pinned by the writer/reader symmetry");

// Retained history for the early-boot events the USB console cannot see
// (the CDC link is down across every deep sleep). 8 x 128 B in RTC slow
// memory, on top of the ~6.1 KB the firmware already reserves there.
constexpr size_t kRingLines = 8;
constexpr size_t kRingLineLen = 128;
RTC_NOINIT_ATTR DiagLogRing<kRingLines, kRingLineLen> diagRing;

void diagLog(const char* format, ...) {
  char line[kRingLineLen];
  va_list args;
  va_start(args, format);
  vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  diagRing.push(SCRATCH_MAGIC, line);
  LOG_INF("RTCCLK", "%s", line);
}

const char* srcName() {
  switch (rtc_clk_slow_freq_get()) {
    case RTC_SLOW_FREQ_RTC:
      return "RC_SLOW";
    case RTC_SLOW_FREQ_32K_XTAL:
      return "XTAL32K";
    case RTC_SLOW_FREQ_8MD256:
      return "RC_FAST_D256";
    default:
      return "UNKNOWN";
  }
}

// cal is the Q13.19 period in us; exactly 32768 Hz reads 16,000,000.
uint32_t calToHz(const uint32_t cal) { return cal ? static_cast<uint32_t>((1000000ULL << 19) / cal) : 0; }

uint64_t rtcCounterTicks() {
  REG_WRITE(RTC_CNTL_TIME_UPDATE_REG, RTC_CNTL_TIME_UPDATE);
  const uint32_t lo = REG_READ(RTC_CNTL_TIME_LOW0_REG);
  const uint32_t hi = REG_READ(RTC_CNTL_TIME_HIGH0_REG);
  return (static_cast<uint64_t>(hi) << 32) | lo;
}

uint64_t unpack32(const uint32_t lo, const uint32_t hi) { return (static_cast<uint64_t>(hi) << 32) | lo; }

// Enable the 32k crystal and select it. A cached calibration is first gated
// by a short liveness calibration — a dead or stalled crystal reads 0 and
// falls through to the full settle-and-measure path, so a wake boot never
// switches onto an unverified clock. False means rtc_clk_cal timed out or
// rejected the clock — its documented failed-startup / invalid-clock
// contract — and the caller stays on RC.
bool applyXtal32k(uint32_t& calCache) {
  rtc_clk_32k_enable(true);
  uint32_t cal = calCache;
  if (cal != 0 && (cal < kCal32kMin || cal > kCal32kMax)) cal = 0;
  if (cal != 0 && rtc_clk_cal(RTC_CAL_32K_XTAL, kCalCyclesLiveness) == 0) cal = 0;
  if (cal == 0) {
    delay(kSettleMs);
    cal = rtc_clk_cal(RTC_CAL_32K_XTAL, kCalCycles32k);
  }
  if (cal == 0) {
    rtc_clk_32k_enable(false);
    calCache = 0;
    return false;
  }
  calCache = cal;
  rtc_clk_slow_freq_set(RTC_SLOW_FREQ_32K_XTAL);
  esp_clk_slowclk_cal_set(cal);
  return true;
}

// Back to the boot default, restoring its calibration.
void applyRc() {
  rtc_clk_slow_freq_set(RTC_SLOW_FREQ_RTC);
  const uint32_t cal = rtc_clk_cal(RTC_CAL_RTC_MUX, kCalCyclesRc);
  if (cal != 0) esp_clk_slowclk_cal_set(cal);
}

// Apply the scratch selection; a failed XTAL32K apply clears it (fail closed:
// every later boot stays on the RC boot default).
void applySelection() {
  if (scratch.selection == static_cast<uint8_t>(Selection::ForceXtal32k)) {
    if (applyXtal32k(scratch.good32kCal)) {
      const uint32_t cal = esp_clk_slowclk_cal_get();
      diagLog("XTAL32K applied (cal=%u, %u Hz)", cal, calToHz(cal));
    } else {
      scratch.selection = static_cast<uint8_t>(Selection::Production);
      diagLog("XTAL32K failed to start; staying on the boot clock");
    }
  } else if (scratch.selection == static_cast<uint8_t>(Selection::ForceRc)) {
    applyRc();
  }
}

// Stamp the entry snapshot, then sleep intervalMs on the selected clock.
// startDeepSleep never returns in normal operation (it also arms the power
// button as the wake source); esp_restart() mirrors the abort contract of
// PowerManager::deepSleep().
[[noreturn]] void enterSleepCycle(HalPowerManager& power, HalGPIO& gpio) {
  const uint64_t t0Ticks = rtcCounterTicks();
  scratch.magic = 0;
  scratch.t0TicksLo = static_cast<uint32_t>(t0Ticks);
  scratch.t0TicksHi = static_cast<uint32_t>(t0Ticks >> 32);
  scratch.sleepCal = esp_clk_slowclk_cal_get();
  scratch.magic = SCRATCH_MAGIC;
  diagLog("cycle %u: sleeping %u ms on %s (cal=%u clkConf=0x%08x reqUs=%llu t0=%llu)", scratch.cycleIndex,
          scratch.intervalMs, srcName(), scratch.sleepCal, REG_READ(RTC_CNTL_CLK_CONF_REG),
          static_cast<unsigned long long>(scratch.intervalMs) * 1000ULL, static_cast<unsigned long long>(t0Ticks));
  power.startDeepSleep(gpio, false, static_cast<uint64_t>(scratch.intervalMs) * 1000ULL);
  esp_restart();
}

}  // namespace

void begin(HalPowerManager& power, HalGPIO& gpio, const HalGPIO::WakeupReason wake) {
  const uint32_t bootCal = esp_clk_slowclk_cal_get();
  diagLog("boot: wake=%d cause=%d reset=%d bootSrc=%s bootCal=%u (%u Hz)", static_cast<int>(wake),
          static_cast<int>(esp_sleep_get_wakeup_cause()), static_cast<int>(esp_reset_reason()), srcName(), bootCal,
          calToHz(bootCal));

  if (scratch.magic != SCRATCH_MAGIC) return;  // cold boot: RC default

  // Snapshot BEFORE any re-apply: the reset left the boot default (RC) active.
  const uint64_t t1Ticks = rtcCounterTicks();
  const uint64_t t0Ticks = unpack32(scratch.t0TicksLo, scratch.t0TicksHi);

  scratch.magic = 0;  // consumed; every exit path below re-stamps or disarms
  applySelection();
  const uint32_t calNow = esp_clk_slowclk_cal_get();

  if (!scratch.loopArmed) {
    scratch.magic = SCRATCH_MAGIC;  // keep the selection for later cycles
    flushBufferedLog();
    return;
  }

  if (wake != HalGPIO::WakeupReason::Timer) {
    scratch.loopArmed = 0;
    diagLog("LOOP_EXIT cycle=%u wake=%d (scratch was valid)", scratch.cycleIndex, static_cast<int>(wake));
    scratch.magic = SCRATCH_MAGIC;  // keep the selection; the loop is over
    flushBufferedLog();
    return;
  }

  const CycleSample sample{static_cast<uint64_t>(scratch.intervalMs) * 1000ULL, scratch.sleepCal, t1Ticks - t0Ticks};
  diagLog("CYCLE %u: ticks=%llu periodEff=%llu errUs=%lld (%lld ppm) src=%s", scratch.cycleIndex,
          static_cast<unsigned long long>(sample.elapsedTicks),
          static_cast<unsigned long long>(effectiveCalPeriod(sample)), static_cast<long long>(cycleErrorUs(sample)),
          static_cast<long long>(cycleErrorPpm(sample)), srcName());

  if (scratch.cyclesRemaining <= 1) {
    scratch.loopArmed = 0;
    diagLog("LOOP_DONE (src=%s cal=%u, %u Hz)", srcName(), calNow, calToHz(calNow));
    scratch.magic = SCRATCH_MAGIC;  // keep the selection; the loop is over
    flushBufferedLog();
    return;
  }
  scratch.cyclesRemaining--;
  scratch.cycleIndex++;
  enterSleepCycle(power, gpio);
}

void flushBufferedLog() {
  static bool flushedThisBoot = false;
  if (flushedThisBoot || !logSerial) return;
  flushedThisBoot = true;
  diagRing.forEach([](const char* line) {
    logSerial.printf("RTCDIAG| %s\n", line);
    return true;
  });
}

void handleCommand(HalPowerManager& power, HalGPIO& gpio, const char* command) {
  String cmd(command);
  if (cmd == "RTCCLK" || cmd.startsWith("RTCCLK ")) {
    String arg = cmd.length() > 6 ? cmd.substring(7) : String();
    arg.trim();
    if (arg == "RC" || arg == "XTAL") {
      scratch.magic = 0;
      scratch.selection = static_cast<uint8_t>(arg == "RC" ? Selection::ForceRc : Selection::ForceXtal32k);
      scratch.loopArmed = 0;
      scratch.cyclesRemaining = 0;
      scratch.cycleIndex = 0;
      scratch.good32kCal = 0;  // a fresh RTCCLK XTAL always re-measures
      if (arg == "RC") {
        applyRc();
      } else if (!applyXtal32k(scratch.good32kCal)) {
        scratch.selection = static_cast<uint8_t>(Selection::Production);
        logSerial.printf("RTCCLK ERR: XTAL32K failed to start; staying on the boot clock\n");
      }
      scratch.magic = SCRATCH_MAGIC;
    }
    const uint32_t cal = esp_clk_slowclk_cal_get();
    logSerial.printf("RTCCLK: src=%s cal=%u (%u Hz) sel=%u loop=%u\n", srcName(), cal, calToHz(cal), scratch.selection,
                     scratch.loopArmed);
    return;
  }
  if (cmd.startsWith("RTCLOOP")) {
    // Usage: RTCLOOP <cycles in [1,10000]> <intervalMs in [100,3600000]>
    uint32_t cycles = 0;
    uint32_t intervalMs = 0;
    if (sscanf(cmd.c_str() + 7, "%u %u", &cycles, &intervalMs) != 2 || cycles == 0 || cycles > kMaxLoopCycles ||
        intervalMs < kMinIntervalMs || intervalMs > kMaxIntervalMs) {
      logSerial.printf("RTCLOOP ERR: usage RTCLOOP <cycles 1..10000> <intervalMs 100..3600000>\n");
      return;
    }
    scratch.magic = 0;
    scratch.loopArmed = 1;
    scratch.cyclesRemaining = cycles;
    scratch.intervalMs = intervalMs;
    scratch.cycleIndex = 1;
    scratch.magic = SCRATCH_MAGIC;
    logSerial.printf("RTCLOOP OK: %u cycles x %u ms\n", cycles, intervalMs);
    enterSleepCycle(power, gpio);
  }
  if (cmd.startsWith("RTCCAL")) {
    // Usage: RTCCAL [cycles in [32,32768]]; measures both clocks, changes none.
    uint32_t cycles = kCalCycles32k;
    sscanf(cmd.c_str() + 6, "%u", &cycles);
    if (cycles < 32 || cycles > 32768) cycles = kCalCycles32k;
    const uint32_t calRc = rtc_clk_cal(RTC_CAL_INTERNAL_OSC, cycles);
    const bool oscWasEnabled = rtc_clk_32k_enabled();
    rtc_clk_32k_enable(true);
    delay(kSettleMs);
    const uint32_t cal32k = rtc_clk_cal(RTC_CAL_32K_XTAL, cycles);
    if (!oscWasEnabled) rtc_clk_32k_enable(false);
    logSerial.printf("RTCCAL: RC=%u Hz 32K=%u Hz (cal %u: RC=%u 32K=%u)\n", calToHz(calRc), calToHz(cal32k), cycles,
                     calRc, cal32k);
  }
}

}  // namespace rtcclockdiag
#endif
