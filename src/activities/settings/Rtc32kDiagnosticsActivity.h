#pragma once

#include <cstdint>

#include "activities/UiListActivity.h"
#include "util/Rtc32kDiagnosticsPolicy.h"

// X4C-only XTAL32K hardware diagnostics: drives the ESP32-S3 internal 32 kHz
// crystal oscillator on the dedicated XTAL_32K_P/N pads (GPIO15/16 carry the
// board's 32.768 kHz crystal) and reports raw RTC-calibration measurements.
// The BM8563 RTC is shown as read-only presence info; its CLKOUT register is
// never written and GPIO15/16 are never touched as digital GPIO. Temporary
// hardware-diagnostic code for an experiment branch; not part of the release
// feature set.
class Rtc32kDiagnosticsActivity final : public UiListActivity {
 public:
  explicit Rtc32kDiagnosticsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : UiListActivity("Rtc32kDiagnostics", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;

 protected:
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;

 private:
  enum Row {
    ITEM_RTC = 0,
    ITEM_STATE,
    ITEM_SRC,
    ITEM_ENABLE,
    ITEM_CALIBRATE,
    ITEM_DISABLE,
    ITEM_CAL,
    ITEM_RESULT,
    ITEM_CAL1,
    ITEM_CAL2,
    ITEM_CAL3,
    ITEM_CAL4,
    ITEM_CAL5,
    ITEM_COUNT
  };

  void doEnable();
  void doCalibrate();
  void doDisable();
  void probeRtc();

  freeink::ui::ListItem rowItems_[ITEM_COUNT]{};
  char rtcBuf_[16];
  char calMedianBuf_[24];
  char calSampleBuf_[rtc32k::CAL_SAMPLE_COUNT][20];
  rtc32k::DiagState state_{};
  rtc32k::SlowClkSource slowSrc_ = rtc32k::SlowClkSource::Invalid;
  bool rtcProbed_ = false;
  bool rtcPresent_ = false;
};
