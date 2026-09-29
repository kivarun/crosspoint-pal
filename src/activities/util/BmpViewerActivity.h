#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "components/OptionPopup.h"

#include <ToneLut.h>

#include "MappedInputManager.h"
#include "activities/Activity.h"

// Image Lab (experiment/x4-image-lab): runtime BMP display parameter tuning.
// Diagnostic firmware only — settings live in RAM for the duration of the
// activity; no persistence, no production settings subsystem.
//
// Modal model (a stack of owned surfaces over the image):
//   Image -> [context menu -> settings dialog / info dialog / delete confirm]
// labTone holds the ACTIVE session-global tone settings; settingsDraft holds
// staged values while the settings dialog is open (committed only by Apply).
enum class LabParam : uint8_t { Brightness = 0, Gamma = 1, Contrast = 2, Quantizer = 3, Count = 4 };

// Which surface owns the buttons right now. Modal surfaces are drawn over the
// image; Viewer is the plain image view with native controls.
enum class LabScreen : uint8_t { Viewer, ContextMenu, Settings, Info, DeleteConfirm };

// Action codes for the dynamic context-menu option rows (order built at show()).
enum LabMenuAction : uint8_t {
  LAB_ACT_SETTINGS = 0,
  LAB_ACT_INFO,
  LAB_ACT_DEBUG_HUD,
  LAB_ACT_SLEEP,
  LAB_ACT_DELETE
};

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
  // Centered modal dialog frame drawn from font/theme metrics (no screen-relative
  // magic coordinates): opaque body, theme border, centered title.
  struct LabDialogLayout {
    int x = 0, y = 0, w = 0, h = 0;
    int textX = 0, textY = 0;  // content origin below the title
    int valueColX = 0;         // second column origin (0 = single column)
    int lineH = 0;
    int rowH = 0;
  };
  LabDialogLayout drawLabDialogFrame(const char* title, int colW, int labelColW, int rowCount);

  void loadSiblingImages();
  void doSetSleepCover();
  bool canSetSleepCover() const;
  bool renderPng();
  void renderBmp(bool showPopup);
  bool rebuildBwFramebuffer();
  void drawLabIndicator(LabPass pass);
  void drawLabIndicatorText(int x, int y);
  void drawLabSettingsDialog();
  void drawInfoDialog();
  void openContextMenu();
  void menuAction(int action);
  void openLabSettings();
  void applyLabSettings();
  void openInfo();
  void openDeleteConfirm();
  void performDelete();
  void labAdjustDraft(int delta);
  void labMoveSettingsRow(int delta);
  void handleLabSettingsInput();

  std::string filePath;
  std::vector<std::string> siblingImages;
  int currentImageIndex = -1;
  bool isPng = false;

  // Image Lab state (RAM-only, survives image navigation within the session)
  LabScreen labScreen = LabScreen::Viewer;
  LabParam labSelected = LabParam::Brightness;
  ToneLut labTone{};        // ACTIVE session-wide settings (consumed by the decoder)
  ToneLut settingsDraft{};  // staged values while Image Settings is open
  int labSettingsRow = 0;   // 0..3 = parameter rows, 4 = Apply
  bool debugHudEnabled = true;  // lab HUD visible on entry, toggle from the menu
  std::vector<std::pair<std::string, std::string>> infoRows;  // filled by openInfo()

  // Context-menu option rows and their action codes, built by openContextMenu()
  // (Show debug info is BMP-only; Set sleep cover appears only when canSetSleepCover()).
  uint8_t menuActions[5] = {};
  int menuActionCount = 0;

  // Existing CrossPoint modal surfaces (shared menu abstraction, no parallel mechanism)
  OptionPopup menuPopup;      // Image settings / Info / Show debug info / Set sleep cover / Delete
  OptionPopup confirmPopup;   // delete confirmation (Cancel / Confirm)
};

