#include "Rtc32kDiagnosticsActivity.h"

#include <BoardConfig.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_adc/adc_oneshot.h>
#include <driver/gpio.h>
#include <driver/pulse_cnt.h>
#include <soc/rtc.h>

#include <FreeRTOS.h>
#include <task.h>
#include <Wire.h>

#include <algorithm>

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
// CLKOUT control (BM8563/PCF8563-compatible): bit7 FE enables the output,
// bits1:0 FD select the frequency (00=32768 Hz, 01=1024 Hz, 10=32 Hz, 11=1 Hz).
// CLKOUT is an OPEN-DRAIN output (BM8563 datasheet): it drives LOW actively
// and floats when high — the level observed at GPIO15 depends on whatever
// pull-up/load the board provides, and no pull-up is assumed anywhere in
// this code.
constexpr uint8_t PCF8563_CLKOUT_OFF = 0x00;      // FE=0 -> high-impedance
constexpr uint8_t PCF8563_CLKOUT_32768HZ = 0x80;  // FE=1, FD=00
constexpr uint8_t PCF8563_CLKOUT_1024HZ = 0x81;   // FE=1, FD=01
constexpr uint8_t PCF8563_CLKOUT_32HZ = 0x82;     // FE=1, FD=10
constexpr uint8_t PCF8563_CLKOUT_1HZ = 0x83;      // FE=1, FD=11

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

// Explicit GPIO15 pad configuration — NEVER inherit the state left behind by
// gpio_reset_pin() (installed IDF 5.5.5: selects gpio function, ENABLES the
// pull-up and DISABLES input). Every sub-test states its own condition.
void configureGpio15(const bool pullUp) {
  gpio_config_t cfg = {};
  cfg.pin_bit_mask = 1ULL << GPIO_NUM_15;
  cfg.mode = GPIO_MODE_INPUT;
  cfg.pull_up_en = pullUp ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
  cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
  cfg.intr_type = GPIO_INTR_DISABLE;
  gpio_config(&cfg);
}

// Single owner for all PCNT rising-edge counting: unit, 1 us glitch filter,
// channel, rising=INCREASE/falling=HOLD, enable/start, per-window clear/read
// and the fail-closed unwind. The pad configuration is explicit per session
// (pull mode), and end() always leaves the pin to be reconfigured by the
// next stage.
struct PcntSession {
  pcnt_unit_handle_t unit = nullptr;
  pcnt_channel_handle_t chan = nullptr;
  bool started = false;
  bool enabled = false;
  bool channelAttached = false;
  bool unitCreated = false;

  bool begin(const bool pullUp) {
    configureGpio15(pullUp);
    pcnt_unit_config_t ucfg = {
        .low_limit = -32768,
        .high_limit = 32767,
        .intr_priority = 0,
        .flags = {.accum_count = 1},
    };
    if (pcnt_new_unit(&ucfg, &unit) != ESP_OK || !unit) return false;
    unitCreated = true;
    pcnt_glitch_filter_config_t fcfg = {.max_glitch_ns = 1000};
    if (pcnt_unit_set_glitch_filter(unit, &fcfg) != ESP_OK) return false;
    pcnt_chan_config_t ccfg = {
        .edge_gpio_num = GPIO_NUM_15,
        .level_gpio_num = -1,
        .flags = {},
    };
    esp_err_t rc = pcnt_new_channel(unit, &ccfg, &chan);
    channelAttached = (rc == ESP_OK && chan != nullptr);
    if (!channelAttached) return false;
    // Counting contract: ONLY rising edges (posedge -> INCREASE, negedge ->
    // HOLD); counting both edges would double every estimate.
    if (pcnt_channel_set_edge_action(chan, PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                     PCNT_CHANNEL_EDGE_ACTION_HOLD) != ESP_OK) {
      return false;
    }
    // Re-assert the explicit pad configuration after the channel attach.
    configureGpio15(pullUp);
    if (pcnt_unit_enable(unit) != ESP_OK) return false;
    enabled = true;
    if (pcnt_unit_start(unit) != ESP_OK) return false;
    started = true;
    return true;
  }

  uint32_t window(const uint32_t durationMs) {
    pcnt_unit_clear_count(unit);
    vTaskDelay(pdMS_TO_TICKS(durationMs));
    int v = 0;
    pcnt_unit_get_count(unit, &v);
    return v > 0 ? static_cast<uint32_t>(v) : 0;
  }

  void end() {
    if (started) pcnt_unit_stop(unit);
    if (enabled) pcnt_unit_disable(unit);
    if (channelAttached) pcnt_del_channel(chan);
    if (unitCreated) pcnt_del_unit(unit);
    gpio_reset_pin(GPIO_NUM_15);
  }
};

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

// Switch CLKOUT to `value` and verify by readback; returns true only when the
// readback confirms. A successful write still ACKs the RTC, so rtcOk is set
// independently of the readback result.
bool Rtc32kDiagnosticsActivity::setClkoutReg(const uint8_t value) {
  if (!diagWriteReg(PCF8563_REG_CLKOUT, value)) {
    LOG_ERR("RTC32K", "CLKOUT write failed (reg 0x0D)");
    return false;
  }
  run_.rtcOk = true;
  uint8_t readback = 0;
  if (diagReadReg(PCF8563_REG_CLKOUT, readback) && readback == value) {
    return true;
  }
  LOG_ERR("RTC32K", "CLKOUT readback failed (reg 0x0D)");
  return false;
}

void Rtc32kDiagnosticsActivity::enableClkout() {
  // Read the ORIGINAL register once per activity lifetime: it is restored
  // verbatim on exit, so re-runs never overwrite it with an enabled value.
  if (!clkoutOriginalValid_) {
    uint8_t original = 0;
    if (!diagReadReg(PCF8563_REG_CLKOUT, original)) {
      LOG_ERR("RTC32K", "RTC/CLKOUT not readable");
      return;
    }
    run_.rtcOk = true;
    clkoutOriginal_ = original;
    clkoutOriginalValid_ = true;
  }
  if (!setClkoutReg(PCF8563_CLKOUT_32768HZ)) {
    LOG_ERR("RTC32K", "CLKOUT 32 kHz enable failed");
    return;
  }
  run_.clkoutOk = true;
  // CLKOUT stays ON for as long as the activity is open so the technician can
  // also probe the RTC pin directly with a meter.
}

void Rtc32kDiagnosticsActivity::runDiagnostics() {
  // Fresh per-run state BEFORE any transaction: a run that fails at its very
  // first I2C operation must display THIS run's empty measurements, never
  // the previous run's samples/calibration. The captured original CLKOUT
  // register value is session state and survives across runs.
  run_.reset();

  if (BoardConfig::isX4Classic() && BoardConfig::ACTIVE.sensors.rtcAddr != 0) {
    enableClkout();
    if (run_.clkoutOk) {
      // 1) Controlled pull-up link probe (OFF 32 1024 32768 OFF) — ends with
      //    the PCNT released and the pad at gpio_reset_pin state.
      runLinkProbe();
      // 2) 1 Hz ADC electrical probe (informational): 1 Hz -> ADC -> 32768.
      if (setClkoutReg(PCF8563_CLKOUT_1HZ)) {
        runAdcProbe();
      }
      // 3..5) No-pull raw PCNT, then the two EXT_OSC calibrations under
      // EXPLICIT pull conditions (no-pull informational, pull-up verdict).
      // A failed 32768 restore makes every later reading untrustworthy ->
      // fail closed: no tests, no stale data.
      if (!setClkoutReg(PCF8563_CLKOUT_32768HZ)) {
        LOG_ERR("RTC32K", "CLKOUT restore failed; PCNT/EXT_OSC skipped");
        run_.clkoutOk = false;
      } else {
        runTestARawEdges();
        run_.calNoPullHz = runTestBExtOscCal(false);
        run_.calHz = runTestBExtOscCal(true);
      }
    }
  }

  const uint32_t rawHz = rtc32k::rawEstimateHz(rtc32k::medianOf10(run_.edgeCounts));
  snprintf(samplesBuf_, sizeof(samplesBuf_), "%u %u %u %u %u %u %u %u %u %u", run_.edgeCounts[0], run_.edgeCounts[1],
           run_.edgeCounts[2], run_.edgeCounts[3], run_.edgeCounts[4], run_.edgeCounts[5], run_.edgeCounts[6],
           run_.edgeCounts[7], run_.edgeCounts[8], run_.edgeCounts[9]);
  snprintf(estimateBuf_, sizeof(estimateBuf_), "%u Hz", static_cast<unsigned>(rawHz));
  snprintf(calBuf_, sizeof(calBuf_), "%u Hz", static_cast<unsigned>(run_.calHz));
  snprintf(calNoPullBuf_, sizeof(calNoPullBuf_), "%u Hz", static_cast<unsigned>(run_.calNoPullHz));
  snprintf(linkCountsBuf_, sizeof(linkCountsBuf_), "%u / %u / %u / %u / %u", static_cast<unsigned>(run_.link.off1),
           static_cast<unsigned>(run_.link.hz32), static_cast<unsigned>(run_.link.hz1024),
           static_cast<unsigned>(run_.link.hz32768), static_cast<unsigned>(run_.link.off2));
  if (run_.adcAvailable) {
    snprintf(adcLowBuf_, sizeof(adcLowBuf_), "%u mV", static_cast<unsigned>(run_.adcLowMv));
    snprintf(adcHighBuf_, sizeof(adcHighBuf_), "%u mV", static_cast<unsigned>(run_.adcHighMv));
    snprintf(adcSwingBuf_, sizeof(adcSwingBuf_), "%u mV", static_cast<unsigned>(run_.adcSwingMv));
  }
  verdict_ = rtc32k::classify(run_.rtcOk, run_.clkoutOk, rawHz, run_.calHz);
  ran_ = true;
  requestUpdate();
}

// Controlled pull-up continuity probe: one PCNT session (explicit input +
// internal pull-up pad configuration) watches the open-drain CLKOUT while
// the BM8563 is commanded OFF -> 32 Hz -> 1024 Hz -> 32768 Hz -> OFF, each
// command verified by register readback, each stage measured in its own
// window. A failed command means no frequency-following conclusion
// (Unavailable, no partial counts).
void Rtc32kDiagnosticsActivity::runLinkProbe() {
  PcntSession pcnt;
  if (!pcnt.begin(true)) {
    LOG_ERR("RTC32K", "Link probe PCNT unavailable");
    pcnt.end();
    return;
  }

  struct Stage {
    uint8_t reg;
    uint32_t windowMs;
  };
  constexpr Stage stages[] = {
      {PCF8563_CLKOUT_OFF, 1000},
      {PCF8563_CLKOUT_32HZ, 1000},
      {PCF8563_CLKOUT_1024HZ, 1000},
      {PCF8563_CLKOUT_32768HZ, 100},
      {PCF8563_CLKOUT_OFF, 1000},
  };
  uint32_t* slots[] = {&run_.link.off1, &run_.link.hz32, &run_.link.hz1024, &run_.link.hz32768, &run_.link.off2};
  bool commandsVerified = true;
  for (size_t i = 0; i < 5; i++) {
    if (!setClkoutReg(stages[i].reg)) {
      commandsVerified = false;
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(rtc32k::LINK_SETTLE_MS));
    *slots[i] = pcnt.window(stages[i].windowMs);
  }
  pcnt.end();

  if (!commandsVerified) {
    run_.link = {};
    return;
  }
  run_.linkResult = rtc32k::linkFollows(run_.link);
}

// One-shot ADC2 reading of the CLKOUT 1 Hz level at GPIO15 (ADC2_CH4): 400
// calibrated-mV samples at ~10 ms (4 s, four full periods), P10/P90 levels.
// Informational only — the result never feeds the verdict.
void Rtc32kDiagnosticsActivity::runAdcProbe() {
  adc_oneshot_unit_handle_t unit = nullptr;
  adc_cali_handle_t cali = nullptr;
  bool unitCreated = false;
  bool caliCreated = false;

  // Single fail-closed cleanup path: unwind whatever reached its setup step
  // (calibration handle -> oneshot unit -> pad state), on every error and on
  // normal completion. gpio_reset_pin leaves GPIO15 digital-clean before the
  // PCNT test re-attaches.
  struct Cleanup {
    adc_oneshot_unit_handle_t& unit;
    adc_cali_handle_t& cali;
    bool& unitCreated;
    bool& caliCreated;
    void run() {
      if (caliCreated) adc_cali_delete_scheme_curve_fitting(cali);
      if (unitCreated) adc_oneshot_del_unit(unit);
      gpio_reset_pin(GPIO_NUM_15);
    }
  } cleanup{unit, cali, unitCreated, caliCreated};

  adc_oneshot_unit_init_cfg_t initCfg = {
      .unit_id = ADC_UNIT_2,
      .clk_src = ADC_RTC_CLK_SRC_DEFAULT,
      .ulp_mode = ADC_ULP_MODE_DISABLE,
  };
  if (adc_oneshot_new_unit(&initCfg, &unit) != ESP_OK || !unit) {
    LOG_ERR("RTC32K", "ADC2 oneshot unit unavailable");
    cleanup.run();
    return;
  }
  unitCreated = true;

  adc_oneshot_chan_cfg_t chanCfg = {
      .atten = ADC_ATTEN_DB_12,
      .bitwidth = ADC_BITWIDTH_DEFAULT,
  };
  if (adc_oneshot_config_channel(unit, ADC_CHANNEL_4, &chanCfg) != ESP_OK) {
    LOG_ERR("RTC32K", "ADC2 channel config failed");
    cleanup.run();
    return;
  }

  adc_cali_curve_fitting_config_t caliCfg = {
      .unit_id = ADC_UNIT_2,
      .chan = ADC_CHANNEL_4,
      .atten = ADC_ATTEN_DB_12,
      .bitwidth = ADC_BITWIDTH_DEFAULT,
  };
  if (adc_cali_create_scheme_curve_fitting(&caliCfg, &cali) != ESP_OK || !cali) {
    LOG_ERR("RTC32K", "ADC calibration scheme unavailable");
    cleanup.run();
    return;
  }
  caliCreated = true;

  int raw = 0;
  int mv = 0;
  for (size_t i = 0; i < rtc32k::ADC_SAMPLE_COUNT; i++) {
    if (adc_oneshot_read(unit, ADC_CHANNEL_4, &raw) != ESP_OK ||
        adc_cali_raw_to_voltage(cali, raw, &mv) != ESP_OK) {
      LOG_ERR("RTC32K", "ADC2 read failed");
      cleanup.run();
      return;
    }
    adcSamples_[i] = static_cast<uint16_t>(mv);
    vTaskDelay(pdMS_TO_TICKS(rtc32k::ADC_SAMPLE_INTERVAL_MS));
  }

  std::sort(adcSamples_, adcSamples_ + rtc32k::ADC_SAMPLE_COUNT);
  const uint16_t low = rtc32k::percentileFromSorted(adcSamples_, rtc32k::ADC_SAMPLE_COUNT, rtc32k::LOW_PERCENTILE);
  const uint16_t high = rtc32k::percentileFromSorted(adcSamples_, rtc32k::ADC_SAMPLE_COUNT, rtc32k::HIGH_PERCENTILE);
  run_.adcAvailable = true;
  run_.adcLowMv = low;
  run_.adcHighMv = high;
  run_.adcSwingMv = rtc32k::swingMv(low, high);
  cleanup.run();
}

bool Rtc32kDiagnosticsActivity::runTestARawEdges() {
  // Baseline raw observation: explicit NO-pull pad configuration, ten 100 ms
  // windows through the shared PCNT owner. With the open-drain CLKOUT and no
  // pull-up, fronts are only expected if the board provides its own path.
  // Per-run measurements were reset by runDiagnostics().
  PcntSession pcnt;
  if (!pcnt.begin(false)) {
    LOG_ERR("RTC32K", "Raw PCNT unavailable");
    pcnt.end();
    return false;
  }
  for (int i = 0; i < 10; i++) {
    run_.edgeCounts[i] = pcnt.window(rtc32k::EDGE_WINDOW_MS);
  }
  pcnt.end();
  return true;
}

uint32_t Rtc32kDiagnosticsActivity::runTestBExtOscCal(const bool pullUp) {
  // The ESP32-S3 external-32k oscillator path: the pad pair GPIO15/16
  // (XTAL_32K_P/N) feeds the 32k oscillator block. The test condition is set
  // EXPLICITLY (pull-up or no-pull) before rtc_clk_32k_enable_external(),
  // which enables the GPIO15 input buffer without touching the pull bits.
  // The one-shot RTC calibration peripheral measures the ext-osc source
  // without switching the firmware's slow clock over; rtc_clk_cal times out
  // and returns 0 on its own (bounded), so no infinite waits are possible.
  configureGpio15(pullUp);
  rtc_clk_32k_enable_external();
  vTaskDelay(pdMS_TO_TICKS(20));
  const uint32_t period = rtc_clk_cal(RTC_CAL_32K_XTAL, 1024);  // ~31 ms + 2x timeout bound
  const uint32_t calHz = period ? static_cast<uint32_t>(1000000ULL * (1ULL << RTC_CLK_CAL_FRACT) / period) : 0;
  if (calHz == 0) {
    LOG_DBG("RTC32K", "EXT_OSC calibration timed out (period=0, pullUp=%d)", pullUp ? 1 : 0);
  }
  // Back to the original pad/clock state; the next stage reconfigures the pad
  // explicitly.
  rtc_clk_32k_disable_external();
  gpio_reset_pin(GPIO_NUM_15);
  gpio_reset_pin(GPIO_NUM_16);
  return calHz;
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
  rowItems_[ITEM_LINK].label = "Pull-up link probe";
  rowItems_[ITEM_LINK_COUNTS].label = "PU counts off/32/1k/32k/off";
  rowItems_[ITEM_ADC_LOW].label = "GPIO15 @1Hz low";
  rowItems_[ITEM_ADC_HIGH].label = "GPIO15 @1Hz high";
  rowItems_[ITEM_ADC_SWING].label = "GPIO15 @1Hz swing";
  rowItems_[ITEM_SAMPLES].label = "GPIO15 raw samples";
  rowItems_[ITEM_ESTIMATE].label = "GPIO raw estimate";
  rowItems_[ITEM_CAL_NO_PULL].label = "EXT_OSC no-pull";
  rowItems_[ITEM_CAL].label = "EXT_OSC pull-up";
  rowItems_[ITEM_RESULT].label = "Result";
  rowItems_[ITEM_RUN].label = "Run again";

  rowItems_[ITEM_RTC].value = !ran_ ? "..." : run_.rtcOk ? "OK" : "FAIL";
  rowItems_[ITEM_CLKOUT].value =
      !ran_ ? "..." : run_.clkoutOk ? "ON / 32768 Hz" : "FAIL (reg 0x0D)";
  rowItems_[ITEM_LINK].value =
      !ran_ ? "..."
            : run_.linkResult == rtc32k::LinkProbeResult::Followed ? "FOLLOWED"
            : run_.linkResult == rtc32k::LinkProbeResult::NotFollowed ? "NOT SEEN"
                                                                      : "UNAVAILABLE";
  rowItems_[ITEM_LINK_COUNTS].value = !ran_ ? "..." : linkCountsBuf_;
  rowItems_[ITEM_ADC_LOW].value = !ran_ ? "..." : run_.adcAvailable ? adcLowBuf_ : "N/A";
  rowItems_[ITEM_ADC_HIGH].value = !ran_ ? "..." : run_.adcAvailable ? adcHighBuf_ : "N/A";
  rowItems_[ITEM_ADC_SWING].value = !ran_ ? "..." : run_.adcAvailable ? adcSwingBuf_ : "N/A";
  rowItems_[ITEM_SAMPLES].value = samplesBuf_;
  rowItems_[ITEM_ESTIMATE].value = estimateBuf_;
  rowItems_[ITEM_CAL_NO_PULL].value = !ran_ ? "..." : calNoPullBuf_;
  rowItems_[ITEM_CAL].value = !ran_ ? "..." : calBuf_;
  // PCNT = ordinary digital GPIO observation (the raw rows above stay
  // informational); the EXT_OSC PULL-UP calibration is the verdict input —
  // the explicit reported test condition (internal pull-up enabled, awake),
  // since the open-drain CLKOUT cannot reach high without a pull-up. The
  // no-pull calibration row is informational. PASS claims only that the
  // ESP32-S3 external-clock path accepted and calibrated the clock in this
  // firmware configuration — never electrical compliance or sleep source
  // suitability.
  rowItems_[ITEM_RESULT].value =
      !ran_ ? "..."
            : verdict_ == rtc32k::Verdict::Pass ? "PASS — EXT_OSC accepted 32 kHz"
            : verdict_ == rtc32k::Verdict::Partial ? "RAW CLOCK SEEN — EXT_OSC rejected"
            : run_.rtcOk && run_.clkoutOk ? "FAIL — EXT_OSC did not accept 32 kHz"
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
