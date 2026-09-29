#pragma once

#include <functional>
#include <string>

#include "components/OptionPopup.h"

#include <ToneLut.h>

#include "MappedInputManager.h"
#include "activities/Activity.h"

// Image Lab (experiment/x4-image-lab): runtime BMP display parameter tuning.
// Diagnostic firmware only — settings live in RAM for the duration of the
// activity; no persistence, no production settings subsystem.
enum class LabParam : uint8_t { Brightness = 0, Gamma = 1, Contrast = 2, Quantizer = 3, Count = 4 };

// Which surface owns the buttons right now. Modal surfaces are drawn over the
// image; Viewer is the plain image view with native controls.
enum class LabScreen : uint8_t { Viewer, ContextMenu, Settings, Info, DeleteConfirm };

// Action codes for the dynamic context-menu option rows (order built at show()).
enum LabMenuAction : uint8_t { LAB_ACT_SETTINGS = 0, LAB_ACT_INFO, LAB_ACT_SLEEP, LAB_ACT_DELETE };

// Where the lab HUD is being drawn (each pass must carry it so it survives
// grayscale compositing).
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
  void drawLabIndicatorText(int x, int y);
  void drawLabSettingsPanel();
  void openContextMenu();
  void menuAction(int choice);
  void openInfo();
  void openDeleteConfirm();
  void performDelete();
  void labAdjust(int delta);
  void labMoveSelection(int delta);
  void handleLabSettingsInput();

  std::string filePath;
  std::vector<std::string> siblingImages;
  int currentImageIndex = -1;
  bool isPng = false;

  // Image Lab state (RAM-only, survives image navigation within the session)
  LabScreen labScreen = LabScreen::Viewer;
  LabParam labSelected = LabParam::Brightness;
  ToneLut labTone{};
  std::vector<std::string> infoLines;  // filled by openInfo()

  // Context-menu option rows and their action codes, built by openContextMenu()
  // (Set sleep cover appears only when canSetSleepCover()).
  uint8_t menuActions[4] = {};
  int menuActionCount = 0;

  // Existing CrossPoint modal surfaces (shared menu abstraction, no parallel mechanism)
  OptionPopup menuPopup;      // Image settings / Info / Delete
  OptionPopup confirmPopup;   // delete confirmation (Cancel / Confirm)
};

