#pragma once

#include <cstdint>

#include "activities/UiListActivity.h"
#include "util/Rtc32kDiagnosticsPolicy.h"

// X4C-only solder-joint diagnostics: BM8563 CLKOUT 32.768 kHz -> bodge wire ->
// ESP32-S3 GPIO15 (XTAL_32K_P). Temporary hardware-diagnostic code for an
// experiment branch; not part of the release feature set.
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
  bool handleButtons() override;
  void drawFooter() override;
  const char* headerTitle() const override;

 private:
  enum Row {
    ITEM_RTC = 0,
    ITEM_CLKOUT,
    ITEM_SAMPLES,
    ITEM_ESTIMATE,
    ITEM_CAL,
    ITEM_RESULT,
    ITEM_RUN,
    ITEM_COUNT
  };

  void enableClkout();
  void runDiagnostics();
  bool runTestARawEdges();
  bool runTestBExtOscCal();

  freeink::ui::ListItem rowItems_[ITEM_COUNT]{};
  char samplesBuf_[96];
  char estimateBuf_[24];
  char calBuf_[24];
  rtc32k::RunState run_{};
  bool clkoutOriginalValid_ = false;
  uint8_t clkoutOriginal_ = 0;
  rtc32k::Verdict verdict_ = rtc32k::Verdict::Fail;
  bool ran_ = false;
};
