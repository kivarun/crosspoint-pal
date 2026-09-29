#pragma once

#include <functional>
#include <string>

#include <ToneLut.h>

#include "MappedInputManager.h"
#include "activities/Activity.h"

// Image Lab (experiment/x4-image-lab): runtime BMP display parameter tuning.
// Diagnostic firmware only — settings live in RAM for the duration of the
// activity; no persistence, no production settings subsystem.
enum class LabParam : uint8_t { Brightness = 0, Gamma = 1, Contrast = 2, Quantizer = 3, Count = 4 };

enum class LabPass : uint8_t {
  Base,      // BW base layer (also shown if grayscale path fails)
  PlaneLsb,  // absolute LSB plane pass
  PlaneMsb,  // absolute MSB plane pass
  BwRebuild  // BW framebuffer rebuild after grayscale display
};

class BmpViewerActivity final : public Activity {
 public:
  BmpViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string filePath);

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  void loadSiblingImages();
  void doSetSleepCover();
  bool canSetSleepCover() const;
  bool renderPng();
  void renderBmp(bool showPopup);
  void drawLabIndicator(LabPass pass);
  void drawLabIndicatorText();
  void labAdjust(int delta);
  void labCycle();

  std::string filePath;
  std::vector<std::string> siblingImages;
  int currentImageIndex = -1;
  bool isPng = false;

  // Image Lab state (RAM-only, survives image navigation within the activity)
  ToneLut labTone{};
  LabParam labSelected = LabParam::Brightness;
};

