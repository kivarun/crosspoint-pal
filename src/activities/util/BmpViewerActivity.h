#pragma once

#include <functional>
#include <string>
#include <vector>

#include <FreeInkUI.h>

#include <ToneLut.h>

#include "GfxRenderer.h"
#include "MappedInputManager.h"
#include "activities/Activity.h"

// Image Lab (experiment/x4-image-lab): runtime BMP display parameter tuning.
// Diagnostic firmware only — settings live in RAM for the duration of the
// activity; no persistence, no production settings subsystem.
//
// Modal model: ONE modal surface over the image. labScreen selects which PAGE
// that surface shows (context menu / settings / info / delete confirm); page
// transitions replace the page contents inside the same panel rect — a child
// popup is never drawn on top of the parent menu. labTone holds the ACTIVE
// session-global tone settings; settingsDraft holds staged values while the
// settings page is open (committed only by Apply).
enum class LabParam : uint8_t { Brightness = 0, Gamma = 1, Contrast = 2, Quantizer = 3, Count = 4 };

enum class LabScreen : uint8_t { Viewer, ContextMenu, ImageSettings, ImageInfo, DeleteConfirm };

// Action codes for the dynamic context-menu rows (order built at page build).
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
  static constexpr int LAB_MAX_ROWS = 7;                     // title header + largest page
  static constexpr int LAB_INTERACTION_CAPACITY = LAB_MAX_ROWS + 1;

  void loadSiblingImages();
  void doSetSleepCover();
  bool canSetSleepCover() const;
  bool renderPng();
  void renderBmp(bool showPopup);
  void drawLabIndicator(LabPass pass);
  void drawLabIndicatorText(int x, int y, const char* text);
  void openLabModal();
  void computeLabModalRect();
  void renderLabModal();
  void repaintLabModal();
  int buildLabPageItems();
  int labPageRowCount() const;
  void handleLabModalInput();
  void handleLabSettingsAxesInput();
  void activateLabRow();
  void labModalBack();
  void labModalAdjust(int delta);
  void saveSleepRenderProfile();
  void menuAction(int action);
  void openInfoPage();
  void performDelete();

  std::string filePath;
  std::vector<std::string> siblingImages;
  int currentImageIndex = -1;
  bool isPng = false;

  // Image Lab state (RAM-only, survives image navigation within the session)
  LabScreen labScreen = LabScreen::Viewer;
  int labModalRow = 0;      // selected row of the current page
  ToneLut labTone{};        // ACTIVE session-wide settings (consumed by the decoder)
  ToneLut settingsDraft{};  // staged values while the settings page is open
  bool debugHudEnabled = true;  // lab HUD visible on entry, toggle from the menu
  std::vector<std::pair<std::string, std::string>> infoRows;  // filled by openInfoPage()

  // Context-menu row actions (dynamic: Show debug info is BMP-only; Set sleep
  // cover appears only when canSetSleepCover()).
  uint8_t menuActions[LAB_MAX_ROWS] = {};
  int menuActionCount = 0;

  // ONE modal surface: fixed panel rect for the whole modal session + page rows
  freeink::ui::Rect labModalRect{};
  freeink::ui::ListItem labItems[LAB_MAX_ROWS] = {};
  char labValueScratch[4][24] = {};  // settings page value strings
  std::string labHeadline;           // delete page filename scratch
  mutable freeink::ui::InteractionBuffer<LAB_INTERACTION_CAPACITY> labInteractions;
};

