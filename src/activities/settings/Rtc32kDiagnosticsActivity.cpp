#include "Rtc32kDiagnosticsActivity.h"

#include <BoardConfig.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <driver/gpio.h>
#include <driver/pulse_cnt.h>
#include <soc/rtc.h>

#include <FreeRTOS.h>
#include <task.h>
#include <Wire.h>

#include "components/UITheme.h"
#include "util/Rtc32kDiagnosticsPolicy.h"

namespace fui = freeink::ui;

// ---------------------------------------------------------------------------
// TEMPORARY X4C hardware-diagnostic I2C access to the BM8563 (PCF8563-
// compatible) CLKOUT control register. The permanent freeink Rtc driver
// deliberately keeps CLKOUT disabled and exposes no register API; this helper
// exists only for the solder-joint diagnostics screen. It rides the board's
// sensor bus (BoardConfig::ACTIVE.sensors — no hardcoded pins) from the same
// task that owns all other I2C users (Rtc / BatteryMonitor / Imu).
// ---------------------------------------------------------------------------
namespace {

constexpr uint8_t PCF8563_REG_CLKOUT = 0x0D;
// FE=1 (output enabled), FD=00 (32768 Hz).
constexpr uint8_t PCF8563_CLKOUT_32768HZ = 0x80;

TwoWire& diagWire() {
#if SOC_I2C_NUM > 1
  return BoardConfig::ACTIVE.sensors.i2cBus == 1 ? Wire1 : Wire;
#else
  return Wire;
#endif
}

// The bus is already up (Rtc::begin ran at boot; all bus users run on this
// task). A transaction that fails simply yields a FAIL verdict.
bool diagReadReg(const uint8_t reg, uint8_t& out) {
  auto& wire = diagWire();
  wire.beginTransmission(BoardConfig::ACTIVE.sensors.rtcAddr);
  wire.write(reg);
  if (wire.endTransmission(false) != 0) return false;
  if (wire.requestFrom(BoardConfig::ACTIVE.sensors.rtcAddr, 1u) < 1) return false;
  out = wire.read();
  return true;
}

bool diagWriteReg(const uint8_t reg, const uint8_t value) {
  auto& wire = diagWire();
  wire.beginTransmission(BoardConfig::ACTIVE.sensors.rtcAddr);
  wire.write(reg);
  wire.write(value);
  return wire.endTransmission() == 0;
}

// XTAL_32K_P pad (GPIO15 on ESP32-S3): plain input, NO pulls, before the PCNT
// channel attaches (and re-asserted after attach in case the driver touched
// the pad config).
void configGpio15PlainInput() {
  gpio_config_t cfg = {};
  cfg.pin_bit_mask = 1ULL << GPIO_NUM_15;
  cfg.mode = GPIO_MODE_INPUT;
  cfg.pull_up_en = GPIO_PULLUP_DISABLE;
  cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
  cfg.intr_type = GPIO_INTR_DISABLE;
  gpio_config(&cfg);
}

}  // namespace

// ---------------------------------------------------------------------------

void Rtc32kDiagnosticsActivity::onEnter() {
  UiListActivity::onEnter();
  runDiagnostics();
}

void Rtc32kDiagnosticsActivity::onExit() {
  // Restore exactly the original CLKOUT register and drop every temporary
  // pad/counter configuration so normal firmware operation resumes.
  if (clkoutOriginalValid_) {
    if (!diagWriteReg(PCF8563_REG_CLKOUT, clkoutOriginal_)) {
      LOG_ERR("RTC32K", "Failed to restore CLKOUT register");
    }
    clkoutOriginalValid_ = false;
  }
  rtc_clk_32k_disable_external();
  gpio_reset_pin(GPIO_NUM_15);
  gpio_reset_pin(GPIO_NUM_16);  // XTAL_32K_N — also part of the ext-osc pair
  UiListActivity::onExit();
}

void Rtc32kDiagnosticsActivity::enableClkout() {
  // Read the ORIGINAL register once per activity lifetime: it is restored
  // verbatim on exit, so re-runs never overwrite it with the enabled value.
  if (clkoutOriginalValid_) {
    // Already captured earlier in this session; just re-enable the output.
    if (diagWriteReg(PCF8563_REG_CLKOUT, PCF8563_CLKOUT_32768HZ) &&
        diagReadReg(PCF8563_REG_CLKOUT, clkoutReadback_) && clkoutReadback_ == PCF8563_CLKOUT_32768HZ) {
      clkoutOk_ = true;
    }
    return;
  }
  uint8_t original = 0;
  if (!diagReadReg(PCF8563_REG_CLKOUT, original)) {
    LOG_ERR("RTC32K", "RTC/CLKOUT not readable");
    rtcOk_ = false;
    return;
  }
  rtcOk_ = true;
  clkoutOriginal_ = original;
  clkoutOriginalValid_ = true;
  if (!diagWriteReg(PCF8563_REG_CLKOUT, PCF8563_CLKOUT_32768HZ) ||
      !diagReadReg(PCF8563_REG_CLKOUT, clkoutReadback_) || clkoutReadback_ != PCF8563_CLKOUT_32768HZ) {
    LOG_ERR("RTC32K", "CLKOUT 32 kHz enable failed");
    return;
  }
  clkoutOk_ = true;
  // CLKOUT stays ON for as long as the activity is open so the technician can
  // also probe the RTC pin directly with a meter.
}

void Rtc32kDiagnosticsActivity::runDiagnostics() {
  rtcOk_ = false;
  clkoutOk_ = false;
  if (BoardConfig::isX4Classic() && BoardConfig::ACTIVE.sensors.rtcAddr != 0) {
    enableClkout();
    if (clkoutOk_) {
      runTestARawEdges();
      runTestBExtOscCal();
    }
  }

  const uint32_t rawHz = rtc32k::rawEstimateHz(rtc32k::medianOf10(edgeCounts_));
  snprintf(samplesBuf_, sizeof(samplesBuf_), "%u %u %u %u %u %u %u %u %u %u", edgeCounts_[0], edgeCounts_[1],
           edgeCounts_[2], edgeCounts_[3], edgeCounts_[4], edgeCounts_[5], edgeCounts_[6], edgeCounts_[7],
           edgeCounts_[8], edgeCounts_[9]);
  snprintf(estimateBuf_, sizeof(estimateBuf_), "%u Hz", static_cast<unsigned>(rawHz));
  snprintf(calBuf_, sizeof(calBuf_), "%u Hz", static_cast<unsigned>(calHz_));
  verdict_ = rtc32k::classify(rtcOk_, clkoutOk_, rawHz, calHz_);
  ran_ = true;
  requestUpdate();
}

bool Rtc32kDiagnosticsActivity::runTestARawEdges() {
  for (int i = 0; i < 10; i++) edgeCounts_[i] = 0;
  calHz_ = 0;

  configGpio15PlainInput();

  pcnt_unit_handle_t unit = nullptr;
  pcnt_channel_handle_t chan = nullptr;
  bool started = false, enabled = false, channelAttached = false;

  // One fail-closed cleanup path: unwind whatever reached its setup step
  // (started -> enabled -> channel -> unit -> pin), on every error and on
  // normal completion.
  struct Cleanup {
    pcnt_unit_handle_t unit;
    pcnt_channel_handle_t& chan;
    bool& started;
    bool& enabled;
    bool& channelAttached;
    void run() {
      if (started) pcnt_unit_stop(unit);
      if (enabled) pcnt_unit_disable(unit);
      if (channelAttached) pcnt_del_channel(chan);
      if (unit) pcnt_del_unit(unit);
      gpio_reset_pin(GPIO_NUM_15);
    }
  } cleanup{unit, chan, started, enabled, channelAttached};

  pcnt_unit_config_t ucfg = {
      .low_limit = -32768,
      .high_limit = 32767,
      .intr_priority = 0,
      .flags = {.accum_count = 1},
  };
  esp_err_t rc = pcnt_new_unit(&ucfg, &unit);
  cleanup.unit = unit;
  if (rc != ESP_OK || !unit) {
    LOG_ERR("RTC32K", "PCNT unit unavailable");
    cleanup.run();
    return false;
  }
  pcnt_glitch_filter_config_t fcfg = {.max_glitch_ns = 1000};
  if (pcnt_unit_set_glitch_filter(unit, &fcfg) != ESP_OK) {
    LOG_ERR("RTC32K", "PCNT glitch filter failed");
    cleanup.run();
    return false;
  }
  pcnt_chan_config_t ccfg = {
      .edge_gpio_num = GPIO_NUM_15,
      .level_gpio_num = -1,
      .flags = {},
  };
  rc = pcnt_new_channel(unit, &ccfg, &chan);
  cleanup.channelAttached = (rc == ESP_OK && chan != nullptr);
  if (rc != ESP_OK || !chan) {
    LOG_ERR("RTC32K", "PCNT channel failed");
    cleanup.run();
    return false;
  }
  // Counting contract: count ONLY rising edges (posedge -> INCREASE, negedge
  // -> HOLD). At 32.768 kHz a 100 ms window then holds 32768 * 0.100 ~= 3277
  // counts; counting both edges would double the raw estimate.
  if (pcnt_channel_set_edge_action(chan, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_HOLD) != ESP_OK) {
    LOG_ERR("RTC32K", "PCNT edge action failed");
    cleanup.run();
    return false;
  }
  // Re-assert the no-pull plain input after the channel attach.
  configGpio15PlainInput();

  if (pcnt_unit_enable(unit) != ESP_OK) {
    LOG_ERR("RTC32K", "PCNT enable failed");
    cleanup.run();
    return false;
  }
  enabled = true;
  if (pcnt_unit_start(unit) != ESP_OK) {
    LOG_ERR("RTC32K", "PCNT start failed");
    cleanup.run();
    return false;
  }
  started = true;

  for (int i = 0; i < 10; i++) {
    pcnt_unit_clear_count(unit);
    vTaskDelay(pdMS_TO_TICKS(rtc32k::EDGE_WINDOW_MS));
    int v = 0;
    pcnt_unit_get_count(unit, &v);
    edgeCounts_[i] = v > 0 ? static_cast<uint32_t>(v) : 0;
  }

  // Free the counter and pin completely before the ext-osc test takes the pad.
  cleanup.run();
  return true;
}

bool Rtc32kDiagnosticsActivity::runTestBExtOscCal() {
  // The ESP32-S3 external-32k oscillator path: the pad pair GPIO15/16
  // (XTAL_32K_P/N) feeds the 32k oscillator block. The one-shot RTC
  // calibration peripheral measures the ext-osc source without switching the
  // firmware's slow clock over; rtc_clk_cal times out and returns 0 on its own
  // (bounded), so no infinite waits are possible.
  rtc_clk_32k_enable_external();
  vTaskDelay(pdMS_TO_TICKS(20));
  const uint32_t period = rtc_clk_cal(RTC_CAL_32K_XTAL, 1024);  // ~31 ms + 2x timeout bound
  calHz_ = period ? static_cast<uint32_t>(1000000ULL * (1ULL << RTC_CLK_CAL_FRACT) / period) : 0;
  if (calHz_ == 0) {
    LOG_DBG("RTC32K", "EXT_OSC calibration timed out (period=0)");
  }
  // Back to the original pad/clock state.
  rtc_clk_32k_disable_external();
  gpio_reset_pin(GPIO_NUM_15);
  gpio_reset_pin(GPIO_NUM_16);
  return calHz_ != 0;
}

int Rtc32kDiagnosticsActivity::listCount() const { return ITEM_COUNT; }

const char* Rtc32kDiagnosticsActivity::headerTitle() const {
  // Deliberately hardcoded English (AboutActivity precedent): the screen is
  // read by the hardware technician, not the user, in every device language.
  return "RTC 32 kHz diagnostics";
}

void Rtc32kDiagnosticsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  // Diagnostic labels/values are support text — hardcoded English (About
  // precedent), showing RAW measurements, never only the verdict.
  rowItems_[ITEM_RTC].label = "RTC";
  rowItems_[ITEM_CLKOUT].label = "CLKOUT";
  rowItems_[ITEM_SAMPLES].label = "GPIO15 samples";
  rowItems_[ITEM_ESTIMATE].label = "GPIO estimate";
  rowItems_[ITEM_CAL].label = "EXT_OSC cal";
  rowItems_[ITEM_RESULT].label = "Result";
  rowItems_[ITEM_RUN].label = "Run again";

  rowItems_[ITEM_RTC].value = !ran_ ? "..." : rtcOk_ ? "OK" : "FAIL";
  rowItems_[ITEM_CLKOUT].value = !ran_ ? "..."
      : clkoutOk_ ? "ON / 32768 Hz" : "FAIL (reg 0x0D)";
  rowItems_[ITEM_SAMPLES].value = samplesBuf_;
  rowItems_[ITEM_ESTIMATE].value = estimateBuf_;
  rowItems_[ITEM_CAL].value = calBuf_;
  rowItems_[ITEM_RESULT].value =
      !ran_ ? "..."
            : verdict_ == rtc32k::Verdict::Pass ? "PASS — external 32 kHz present"
            : verdict_ == rtc32k::Verdict::Partial ? "SIGNAL PRESENT — EXT_OSC calibration failed"
            : rtcOk_ && clkoutOk_ ? "FAIL — no usable 32 kHz on GPIO15"
                                  : "FAIL — RTC/CLKOUT unavailable";
  rowItems_[ITEM_RUN].value = "";

  fui::ListProps props;
  props.items = rowItems_;
  props.count = ITEM_COUNT;
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 2;
  syncListViewport(screen, props);
  screen.list(props);
}

void Rtc32kDiagnosticsActivity::activateIndex(const int index) {
  if (index != ITEM_RUN) return;
  app.clearTapFlash();
  runDiagnostics();
}

bool Rtc32kDiagnosticsActivity::handleButtons() {
  // Footer says Back | Run again, so Confirm must run the pass from EVERY
  // row, not only from the selected one (the default list activation would
  // require navigating to the Run row first). Up/Down keep the base list
  // navigation.
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onBackButton();
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    app.clearTapFlash();
    runDiagnostics();
    return true;
  }
  return false;
}

void Rtc32kDiagnosticsActivity::drawFooter() {
  const auto hint = mappedInput.mapLabels(tr(STR_BACK), "Run again", "", "");
  GUI.drawButtonHints(renderer, hint.btn1, hint.btn2, hint.btn3, hint.btn4);
}
