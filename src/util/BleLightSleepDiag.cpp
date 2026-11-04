#include "BleLightSleepDiag.h"

#ifdef CROSSPOINT_BLE_LIGHT_SLEEP_DIAG

#include <Logging.h>
#include <esp_pm.h>
#include <soc/rtc.h>
#include <sys/types.h>

#include <cstdio>
#include <cstring>

namespace blelightsleepdiag {
namespace {

constexpr const char* TAG = "BLELP";

// esp_pm_dump_locks writes to a stdio FILE*, and this core's stdio routes to
// the primary console (UART0), not the USJ monitor the UAT reads. Wrap the
// serial log reference in a FILE* so the dump reaches the operator on demand.
ssize_t logCookieWrite(void*, const char* buf, size_t len) {
  logSerial.write(reinterpret_cast<const uint8_t*>(buf), len);
  return static_cast<ssize_t>(len);
}

FILE* dumpFile() {
  static FILE* file = nullptr;
  if (file == nullptr) {
    static const cookie_io_functions_t io = {nullptr, logCookieWrite, nullptr, nullptr};
    file = fopencookie(nullptr, "w", io);
  }
  return file;
}

const char* slowSrcName() {
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

}  // namespace

void begin() {
  // These two are baked by the rebuilt core's sdkconfig and flow into the
  // precompiled BT controller at esp_bt_controller_init() (esp_bt.h fills
  // them into the config struct). sleep_clock 2 = external 32k crystal.
  LOG_INF(TAG, "controller sleepMode=%d lpClock=%d (2 = external 32k crystal)", CONFIG_BT_CTRL_SLEEP_MODE_EFF,
          CONFIG_BT_CTRL_SLEEP_CLOCK_EFF);
  LOG_INF(TAG, "RTC slow clock at boot: %s", slowSrcName());

  esp_pm_config_t pm = {};
  pm.max_freq_mhz = 240;
  pm.min_freq_mhz = CROSSPOINT_BLE_LIGHT_SLEEP_MIN_FREQ;
  pm.light_sleep_enable = CROSSPOINT_BLE_LIGHT_SLEEP_AUTOLS != 0;
  const esp_err_t err = esp_pm_configure(&pm);
  LOG_INF(TAG, "esp_pm_configure(240/%d MHz, automatic light sleep %s): %s", pm.min_freq_mhz,
          pm.light_sleep_enable ? "on" : "OFF (control build)", err == ESP_OK ? "OK" : esp_err_to_name(err));
}

void handleCommand(const char* command) {
  // esp_pm_dump_locks names every active lock and its holder, which is the
  // "why is light sleep not engaging" answer; on-demand only so logging
  // never blocks light sleep by itself. The dump goes through the serial
  // log, not stdio's primary console.
  if (command != nullptr && strcmp(command, "PMSTAT") == 0) {
    FILE* file = dumpFile();
    if (file == nullptr) {
      LOG_ERR(TAG, "PMSTAT: could not create the serial dump stream");
      return;
    }
    LOG_INF(TAG, "PM lock dump follows");
    esp_pm_dump_locks(file);
    fflush(file);
  }
}

}  // namespace blelightsleepdiag
#endif
