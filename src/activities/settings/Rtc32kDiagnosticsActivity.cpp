#include "Rtc32kDiagnosticsActivity.h"

#include <BoardConfig.h>
#include <FreeRTOS.h>
#include <Logging.h>
#include <Wire.h>
#include <soc/rtc.h>
#include <task.h>

#include "components/UITheme.h"

namespace fui = freeink::ui;

// ---------------------------------------------------------------------------
// Read-only I2C presence probe for the BM8563 (PCF8563-compatible) RTC. The
// diagnostics screen never writes to the RTC: the permanent freeink Rtc driver
// owns that device, and the BM8563 CLKOUT is not part of this diagnostic
// anymore (the 32 kHz source under test is the ESP32-S3 XTAL32K crystal on the
// dedicated XTAL_32K_P/N pads). The probe rides the board's sensor bus
// (BoardConfig::ACTIVE.sensors — no hardcoded pins) from the same task that
// owns all other I2C users.
// ---------------------------------------------------------------------------
namespace {

constexpr uint8_t PCF8563_REG_CLKOUT = 0x0D;  // read only in this screen

TwoWire& diagWire() {
#if SOC_I2C_NUM > 1
  return BoardConfig::ACTIVE.sensors.i2cBus == 1 ? Wire1 : Wire;
#else
  return Wire;
#endif
}

// Mirrors BatteryMonitor's lazy bus bring-up: Wire.begin is idempotent, so an
// already-initialized bus is simply re-asserted with the same board pins.
void ensureDiagWire() {
  static bool ready = false;
  if (ready) return;
  const auto& s = BoardConfig::ACTIVE.sensors;
  diagWire().begin(s.i2cSda, s.i2cScl, s.i2cHz);
  ready = true;
}

bool diagReadReg(const uint8_t reg, uint8_t& out) {
  auto& wire = diagWire();
  wire.beginTransmission(BoardConfig::ACTIVE.sensors.rtcAddr);
  wire.write(reg);
  if (wire.endTransmission(false) != 0) return false;
  if (wire.requestFrom(BoardConfig::ACTIVE.sensors.rtcAddr, 1u) < 1) return false;
  out = wire.read();
  return true;
}

rtc32k::SlowClkSource mapSlowSource(const soc_rtc_slow_clk_src_t src) {
  switch (src) {
    case SOC_RTC_SLOW_CLK_SRC_RC_SLOW:
      return rtc32k::SlowClkSource::RcSlow;
    case SOC_RTC_SLOW_CLK_SRC_XTAL32K:
      return rtc32k::SlowClkSource::Xtal32k;
    case SOC_RTC_SLOW_CLK_SRC_RC_FAST_D256:
      return rtc32k::SlowClkSource::RcFastD256;
    default:
      return rtc32k::SlowClkSource::Invalid;
  }
}

}  // namespace

// ---------------------------------------------------------------------------

void Rtc32kDiagnosticsActivity::onEnter() {
  // Session starts clean: the generator is OFF and nothing is owed to the
  // firmware clock state. The read-only probes run before the first render so
  // the opening frame already shows real values.
  state_.reset();
  probeRtc();
  slowSrc_ = mapSlowSource(rtc_clk_slow_src_get());
  UiListActivity::onEnter();
}

void Rtc32kDiagnosticsActivity::onExit() {
  // Undo ONLY what this activity did: if it enabled the generator, turn it
  // off again. No persistent settings, no slow-clock changes, no pad work —
  // GPIO15/16 belong to the XTAL32K oscillator and are never reconfigured.
  if (rtc32k::shouldDisableOnExit(state_)) {
    rtc_clk_32k_enable(false);
    state_.markDisabled();
    LOG_INF("RTC32K", "XTAL32K disabled on exit");
  }
  UiListActivity::onExit();
}

// One I2C register read, no writes: the BM8563 answers (or not), and the
// CLKOUT control register value is logged for the record only.
void Rtc32kDiagnosticsActivity::probeRtc() {
  rtcProbed_ = true;
  rtcPresent_ = false;
  if (!BoardConfig::isX4Classic() || BoardConfig::ACTIVE.sensors.rtcAddr == 0) return;
  ensureDiagWire();
  uint8_t clkout = 0;
  if (diagReadReg(PCF8563_REG_CLKOUT, clkout)) {
    rtcPresent_ = true;
    LOG_DBG("RTC32K", "BM8563 present, CLKOUT reg 0x%02X (read-only)", clkout);
  } else {
    LOG_ERR("RTC32K", "BM8563 not detected (read reg 0x0D)");
  }
}

// Crystal mode enable: the pad pair is routed to the internal oscillator
// (RTC_IO_X32P/X32N_MUX_SEL) and the oscillator core is powered up. This is
// the stock ESP-IDF path (rtc_clk_32k_enable), NOT the single-ended
// external-clock input mode (rtc_clk_32k_enable_external). The generator
// stays on until Disable or exit so the technician can probe the crystal.
void Rtc32kDiagnosticsActivity::doEnable() {
  if (state_.xtalOn) {
    LOG_DBG("RTC32K", "XTAL32K already on");
    return;
  }
  rtc_clk_32k_enable(true);
  state_.markEnabled();
  LOG_INF("RTC32K", "XTAL32K enabled (crystal mode)");
  requestUpdate();
}

// Five independent RTC-calibration measurements of the XTAL32K clock. The
// calibration peripheral times the 32k clock against the main crystal without
// switching the system's RTC_SLOW_CLK source; every sample is stored, the
// median and per-sample values are shown, and a dead oscillator times out
// (period 0) instead of reporting a bogus frequency.
void Rtc32kDiagnosticsActivity::doCalibrate() {
  app.clearTapFlash();
  // Fresh per-run state BEFORE measuring: the screen shows THIS run's empty
  // sample rows while the measurements run, never the previous run's values.
  state_.cal.beginRun();
  requestUpdate();

  if (state_.xtalOn) {
    // Let a freshly enabled crystal settle before the first measurement.
    vTaskDelay(pdMS_TO_TICKS(rtc32k::XTAL_STARTUP_SETTLE_MS));
  } else {
    // Oscillator off: every sample will time out — an honest result, shown as
    // such. The calibration never touches the oscillator state.
    LOG_INF("RTC32K", "Calibrate with oscillator off: TIMEOUT expected");
  }
  for (size_t i = 0; i < rtc32k::CAL_SAMPLE_COUNT; i++) {
    const uint32_t period = rtc_clk_cal(RTC_CAL_32K_XTAL, rtc32k::CAL_CYCLES);
    const uint32_t hz = rtc32k::periodToHz(period);
    state_.cal.record(period != 0, hz);
    LOG_INF("RTC32K", "Cal #%u: %lu Hz (period %lu)", static_cast<unsigned>(i + 1), static_cast<unsigned long>(hz),
            static_cast<unsigned long>(period));
    if (i + 1 < rtc32k::CAL_SAMPLE_COUNT) vTaskDelay(pdMS_TO_TICKS(10));
  }
  requestUpdate();
}

// Standard disable path (rtc_clk_32k_enable(false)): powers the oscillator
// core down. The screen refreshes immediately; the pad pair stays in its
// crystal-mux state — no GPIO reconfiguration.
void Rtc32kDiagnosticsActivity::doDisable() {
  rtc_clk_32k_enable(false);
  state_.markDisabled();
  LOG_INF("RTC32K", "XTAL32K disabled");
  requestUpdate();
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
  rowItems_[ITEM_STATE].label = "XTAL32K state";
  rowItems_[ITEM_SRC].label = "RTC slow clock";
  rowItems_[ITEM_ENABLE].label = "XTAL32K enable";
  rowItems_[ITEM_CALIBRATE].label = "XTAL32K calibrate";
  rowItems_[ITEM_DISABLE].label = "XTAL32K disable";
  rowItems_[ITEM_CAL].label = "Calibration";
  rowItems_[ITEM_RESULT].label = "Result";
  rowItems_[ITEM_CAL1].label = "Cal #1";
  rowItems_[ITEM_CAL2].label = "Cal #2";
  rowItems_[ITEM_CAL3].label = "Cal #3";
  rowItems_[ITEM_CAL4].label = "Cal #4";
  rowItems_[ITEM_CAL5].label = "Cal #5";

  rowItems_[ITEM_RTC].value = !rtcProbed_ ? "..." : rtcPresent_ ? "detected" : "not detected";
  rowItems_[ITEM_STATE].value = rtc32k::stateLabel(state_.xtalOn);
  rowItems_[ITEM_SRC].value = rtc32k::slowClockSourceName(slowSrc_);
  rowItems_[ITEM_ENABLE].value = "";
  rowItems_[ITEM_CALIBRATE].value = "";
  rowItems_[ITEM_DISABLE].value = "";
  rtc32k::formatCalMedian(calMedianBuf_, sizeof(calMedianBuf_), state_.cal);
  rowItems_[ITEM_CAL].value = calMedianBuf_;
  rowItems_[ITEM_RESULT].value = rtc32k::resultLabel(state_.cal);
  for (size_t i = 0; i < rtc32k::CAL_SAMPLE_COUNT; i++) {
    rtc32k::formatCalSample(calSampleBuf_[i], sizeof(calSampleBuf_[i]), state_.cal, i);
    rowItems_[ITEM_CAL1 + static_cast<int>(i)].value = calSampleBuf_[i];
  }

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
  switch (index) {
    case ITEM_ENABLE:
      app.clearTapFlash();
      doEnable();
      break;
    case ITEM_CALIBRATE:
      app.clearTapFlash();
      doCalibrate();
      break;
    case ITEM_DISABLE:
      app.clearTapFlash();
      doDisable();
      break;
    default:
      break;
  }
}
