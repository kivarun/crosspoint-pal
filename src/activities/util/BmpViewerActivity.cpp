#include "BmpViewerActivity.h"

#include <Bitmap.h>
#include <Epub/converters/PngToFramebufferConverter.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>

#include "CrossPointSettings.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr char CUSTOM_SLEEP_ROOT_BMP[] = "/sleep.bmp";
constexpr char TRANSPARENT_SLEEP_ROOT_BMP[] = "/sleep-overlay.bmp";
constexpr char TRANSPARENT_SLEEP_ROOT_PNG[] = "/sleep-overlay.png";
constexpr size_t COPY_BUFFER_SIZE = 2048;

// Image Lab HUD: offset added to the oriented viewable insets. Physical inset
// from the panel edge is bezel + offset; the X4C's smallest bezel inset is 3 px
// (bottom), so +8 keeps the HUD >= 11 px from the glass edge in every
// orientation (spec: 10-12 px minimum), instead of a blind constant.
constexpr int LAB_HUD_INSET = 8;
constexpr int LAB_HUD_W = 132;
constexpr int LAB_HUD_H = 17;

// Image Lab settings panel (bottom-right, above the button-hint strip).
constexpr int LAB_PANEL_W = 200;
constexpr int LAB_PANEL_MARGIN = 12;
constexpr int LAB_PANEL_ROW_H = 16;
constexpr int LAB_PANEL_PAD = 6;

constexpr int INFO_LINE_MAX_CHARS = 60;
}  // namespace

BmpViewerActivity::BmpViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string path)
    : Activity("BmpViewer", renderer, mappedInput), filePath(std::move(path)) {}

void BmpViewerActivity::loadSiblingImages() {
  siblingImages.clear();
  currentImageIndex = -1;

  if (filePath.empty()) return;

  std::string dirPath = FsHelpers::extractFolderPath(filePath);
  size_t lastSlash = filePath.find_last_of('/');
  std::string fileName = (lastSlash != std::string::npos) ? filePath.substr(lastSlash + 1) : filePath;

  auto dir = Storage.open(dirPath.c_str());
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return;
  }

  char name[500];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    if (!file.isDirectory()) {
      file.getName(name, sizeof(name));
      if (name[0] != '.') {
        std::string fname(name);
        if (FsHelpers::hasBmpExtension(fname) || FsHelpers::hasPngExtension(fname)) {
          siblingImages.push_back(fname);
        }
      }
    }
    file.close();
  }
  dir.close();

  FsHelpers::sortFileList(siblingImages);

  const auto image = std::find(siblingImages.begin(), siblingImages.end(), fileName);
  if (image != siblingImages.end()) {
    currentImageIndex = static_cast<int>(image - siblingImages.begin());
  }
}

bool BmpViewerActivity::canSetSleepCover() const {
  return FsHelpers::hasBmpExtension(filePath) ||
         (SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::TRANSPARENT_CUSTOM &&
          FsHelpers::hasPngExtension(filePath));
}

bool BmpViewerActivity::renderPng() {
  ImageDimensions dimensions;
  if (!PngToFramebufferConverter::getDimensionsStatic(filePath, dimensions)) return false;
  if (dimensions.width <= 0 || dimensions.height <= 0) return false;

  const float scale = std::min(static_cast<float>(renderer.getScreenWidth()) / dimensions.width,
                               static_cast<float>(renderer.getScreenHeight()) / dimensions.height);
  const int width = std::min(renderer.getScreenWidth(), static_cast<int>(dimensions.width * std::min(scale, 1.0f)));
  const int height = std::min(renderer.getScreenHeight(), static_cast<int>(dimensions.height * std::min(scale, 1.0f)));
  RenderConfig config{(renderer.getScreenWidth() - width) / 2, (renderer.getScreenHeight() - height) / 2, width,
                      height};

  PngToFramebufferConverter converter;
  return converter.decodeToFramebuffer(filePath, renderer, config);
}

void BmpViewerActivity::drawLabIndicatorText(int x, int y) {
  char buf[32];
  snprintf(buf, sizeof(buf), "B%u G%d.%02d C%u Q:%c", labTone.brightnessPct, labTone.gammaPct / 100,
           labTone.gammaPct % 100, labTone.contrastPct, labTone.quantizer == 1 ? 'C' : 'L');
  renderer.drawText(UI_10_FONT_ID, x + 4, y + 2, buf, true);
}

void BmpViewerActivity::drawLabIndicator(LabPass pass) {
  // Lab HUD is BMP-only and user-togglable from the context menu; when off it
  // is skipped in EVERY pass (base, both plane passes, BW rebuild).
  if (isPng || !debugHudEnabled) return;

  // Safe inset from the physical panel edge, derived from the live orientation
  // (not blind constants): logical x/y = viewable inset + LAB_HUD_INSET.
  int top, right, bottom, left;
  renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
  const int hudX = left + LAB_HUD_INSET;
  const int hudY = top + LAB_HUD_INSET;

  if (pass == LabPass::PlaneLsb || pass == LabPass::PlaneMsb) {
    if (renderer.grayPlanesAreAbsolute()) {
      // Absolute planes: white patch (bits 1) + black text (bits 0) in each
      // plane -> black text on white patch in the composited output.
      renderer.fillRect(hudX, hudY, LAB_HUD_W, LAB_HUD_H, false);
      drawLabIndicatorText(hudX, hudY);
    } else {
      // Overlay mask planes: bit 0 = no gray change -> keep the base pass
      // rendering of the indicator visible.
      renderer.fillRect(hudX, hudY, LAB_HUD_W, LAB_HUD_H, true);
      drawLabIndicatorText(hudX, hudY);
    }
    return;
  }

  // BW base / BW rebuild passes: white patch + black text.
  renderer.fillRect(hudX, hudY, LAB_HUD_W, LAB_HUD_H, false);
  drawLabIndicatorText(hudX, hudY);
}

void BmpViewerActivity::drawLabSettingsPanel() {
  const int screenH = renderer.getScreenHeight();
  const int panelH = LAB_PANEL_ROW_H * static_cast<int>(LabParam::Count) + LAB_PANEL_PAD * 2;
  const int px = renderer.getScreenWidth() - LAB_PANEL_W - LAB_PANEL_MARGIN;
  const int py = screenH - panelH - LAB_PANEL_ROW_H * 2;  // above the button-hint strip

  renderer.fillRect(px, py, LAB_PANEL_W, panelH, false);

  const char* valueStrings[] = {"Legacy", "Canonical"};
  for (int i = 0; i < static_cast<int>(LabParam::Count); i++) {
    char row[32];
    const int rowY = py + LAB_PANEL_PAD + i * LAB_PANEL_ROW_H;
    switch (static_cast<LabParam>(i)) {
      case LabParam::Brightness:
        snprintf(row, sizeof(row), "Brightness  %u %%", labTone.brightnessPct);
        break;
      case LabParam::Gamma:
        snprintf(row, sizeof(row), "Gamma       %d.%02d", labTone.gammaPct / 100, labTone.gammaPct % 100);
        break;
      case LabParam::Contrast:
        snprintf(row, sizeof(row), "Contrast    %u %%", labTone.contrastPct);
        break;
      case LabParam::Quantizer:
        snprintf(row, sizeof(row), "Quantizer   %s",
                 valueStrings[labTone.quantizer == 1 ? 1 : 0]);
        break;
      default:
        continue;
    }
    if (static_cast<int>(labSelected) == i) {
      renderer.drawText(UI_10_FONT_ID, px + 2, rowY, ">", true);
    }
    renderer.drawText(UI_10_FONT_ID, px + 10, rowY, row, true);
  }
}

void BmpViewerActivity::openContextMenu() {
  labScreen = LabScreen::ContextMenu;
  labSelected = LabParam::Brightness;

  // Option rows are built dynamically; actions[] keeps the row->action mapping
  // so the selection callback never relies on positional guessing.
  // Show debug info is BMP-only (the HUD never renders for PNG); Set sleep
  // cover appears only when the viewer can actually set one.
  const char* options[5];
  std::string debugLabel;
  menuActionCount = 0;
  if (!isPng) {
    options[menuActionCount] = tr(STR_IMAGE_SETTINGS);
    menuActions[menuActionCount++] = LAB_ACT_SETTINGS;
  }
  options[menuActionCount] = tr(STR_INFO);
  menuActions[menuActionCount++] = LAB_ACT_INFO;
  if (!isPng) {
    debugLabel.reserve(32);
    debugLabel += tr(STR_SHOW_DEBUG_INFO);
    debugLabel += ": ";
    debugLabel += debugHudEnabled ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
    options[menuActionCount] = debugLabel.c_str();
    menuActions[menuActionCount++] = LAB_ACT_DEBUG_HUD;
  }
  if (canSetSleepCover()) {
    options[menuActionCount] = tr(STR_SET_SLEEP_COVER);
    menuActions[menuActionCount++] = LAB_ACT_SLEEP;
  }
  options[menuActionCount] = tr(STR_DELETE);
  menuActions[menuActionCount++] = LAB_ACT_DELETE;

  menuPopup.show(tr(STR_IMAGE_LAB), options, menuActionCount, 0, [this](int choice) {
    if (choice < 0 || choice >= menuActionCount) return;
    menuAction(menuActions[choice]);
  });
  menuPopup.processRender(renderer, mappedInput);
}

void BmpViewerActivity::menuAction(int action) {
  switch (action) {
    case LAB_ACT_SETTINGS:  // BMP only; the menu hides this entry for PNG
      labScreen = LabScreen::Settings;
      drawLabSettingsPanel();
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      break;
    case LAB_ACT_INFO:
      openInfo();
      break;
    case LAB_ACT_DEBUG_HUD:
      // Toggle the lab HUD and repaint the image so the old HUD physically
      // leaves the e-ink panel; then re-show the menu with the new state.
      debugHudEnabled = !debugHudEnabled;
      labScreen = LabScreen::Viewer;
      renderBmp(false);
      openContextMenu();
      break;
    case LAB_ACT_SLEEP:
      labScreen = LabScreen::Viewer;  // menu closes; the sleep-cover flow re-renders itself
      doSetSleepCover();              // existing implementation, unchanged; now menu-driven
      break;
    case LAB_ACT_DELETE:
      openDeleteConfirm();
      break;
    default:
      break;
  }
}

void BmpViewerActivity::openInfo() {
  labScreen = LabScreen::Info;
  infoLines.clear();
  infoLines.reserve(5);

  // Filename + path
  size_t lastSlash = filePath.find_last_of('/');
  const std::string fileName = (lastSlash != std::string::npos) ? filePath.substr(lastSlash + 1) : filePath;
  std::string pathLine = filePath;
  if (pathLine.size() > INFO_LINE_MAX_CHARS) {
    pathLine.resize(INFO_LINE_MAX_CHARS);
    pathLine += "...";
  }
  infoLines.push_back(fileName);
  infoLines.push_back(pathLine);

  // Dimensions, format, size through the existing decode APIs only.
  char line[80];
  if (isPng) {
    ImageDimensions dimensions;
    if (PngToFramebufferConverter::getDimensionsStatic(filePath, dimensions)) {
      snprintf(line, sizeof(line), "%d x %d, PNG", dimensions.width, dimensions.height);
    } else {
      snprintf(line, sizeof(line), "PNG (dimensions unavailable)");
    }
    infoLines.push_back(line);
  } else {
    HalFile file;
    if (Storage.openFileForRead("BMP", filePath, file)) {
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() == BmpReaderError::Ok) {
        snprintf(line, sizeof(line), "%d x %d, BMP %ubpp%s", bitmap.getWidth(), bitmap.getHeight(),
                 bitmap.getBpp(), bitmap.is1Bit() ? " (BW)" : " (gray)");
        infoLines.push_back(line);
      } else {
        infoLines.push_back("BMP (parse failed)");
      }
      snprintf(line, sizeof(line), "%u bytes", static_cast<unsigned>(file.size()));
      infoLines.push_back(line);
    } else {
      infoLines.push_back("File unavailable");
    }
  }

  renderer.clearScreen();
  int y = 10;
  for (const auto& textLine : infoLines) {
    renderer.drawText(UI_10_FONT_ID, 12, y, textLine.c_str(), true);
    y += renderer.getLineHeight(UI_10_FONT_ID);
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

void BmpViewerActivity::openDeleteConfirm() {
  labScreen = LabScreen::DeleteConfirm;

  size_t lastSlash = filePath.find_last_of('/');
  const std::string fileName = (lastSlash != std::string::npos) ? filePath.substr(lastSlash + 1) : filePath;
  const char* options[] = {tr(STR_CANCEL), tr(STR_CONFIRM)};
  confirmPopup.show(tr(STR_DELETE), fileName.c_str(), options, 2, 0, [this](int idx) {
    labScreen = LabScreen::Viewer;
    if (idx == 1) {
      performDelete();
    } else {
      renderBmp(false);  // cancelled: restore the image view
    }
  });
  confirmPopup.processRender(renderer, mappedInput);
}

void BmpViewerActivity::performDelete() {
  const std::string dirPath = FsHelpers::extractFolderPath(filePath);
  const int oldCount = static_cast<int>(siblingImages.size());
  const int deletedIndex = currentImageIndex;

  const bool removed = Storage.remove(filePath.c_str());
  if (!removed) {
    LOG_ERR("BMP", "Failed to delete %s", filePath.c_str());
  }
  // Viewer images have no generated cache artifacts in this firmware (there is
  // no removeImageCache() mechanism; BMP/PNG viewers decode from source), so
  // nothing else to clean up.

  // Delete contract: a failed physical delete aborts BEFORE any list/index
  // mutation — siblingImages/currentImageIndex/filePath stay untouched, the
  // viewer returns to the current image.
  const auto next = FsHelpers::imageIndexAfterRemove(removed, oldCount, deletedIndex);
  if (!next.has_value()) {
    GUI.drawPopup(renderer, tr(STR_FAILED_LOWER));
    delay(1000);
    renderBmp(false);
    return;
  }
  if (deletedIndex < 0 || deletedIndex >= oldCount) return;  // inconsistent state: do not corrupt indices

  siblingImages.erase(siblingImages.begin() + deletedIndex);
  currentImageIndex = *next;
  if (*next < 0) {
    // Folder is empty: exit to the file browser at its folder.
    activityManager.goToFileBrowser(dirPath);
    return;
  }
  std::string navPath = dirPath;
  if (!navPath.empty() && navPath.back() != '/') navPath += "/";
  filePath = navPath + siblingImages[currentImageIndex];
  isPng = FsHelpers::hasPngExtension(filePath);
  onEnter();
}

void BmpViewerActivity::labMoveSelection(int delta) {
  const int count = static_cast<int>(LabParam::Count);
  const int next = (static_cast<int>(labSelected) + delta + count) % count;
  labSelected = static_cast<LabParam>(next);
  drawLabSettingsPanel();
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);  // cursor-only partial update
}

void BmpViewerActivity::labAdjust(int delta) {
  // Image Lab settings: adjust the selected parameter (Left = decrease,
  // Right = increase, clamped to the range bounds); the quantizer toggles.
  switch (labSelected) {
    case LabParam::Brightness:
      labTone.brightnessPct = labStepClamped(labTone.brightnessPct, delta, LAB_BRIGHTNESS_MIN, LAB_BRIGHTNESS_MAX);
      break;
    case LabParam::Gamma:
      labTone.gammaPct = labStepClamped(labTone.gammaPct, delta, LAB_GAMMA_MIN, LAB_GAMMA_MAX);
      break;
    case LabParam::Contrast:
      labTone.contrastPct = labStepClamped(labTone.contrastPct, delta, LAB_CONTRAST_MIN, LAB_CONTRAST_MAX);
      break;
    case LabParam::Quantizer:
      labTone.quantizer = (labTone.quantizer == 1) ? 0 : 1;
      break;
    default:
      return;
  }
  buildToneLut(labTone);
  renderBmp(false);  // current image with the new parameters
  drawLabSettingsPanel();
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void BmpViewerActivity::handleLabSettingsInput() {
  // Image settings modal: Up/Down (side keys) select the row, Left/Right
  // adjust the value, Confirm/Back close and return to the image view.
  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    labMoveSelection(-1);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    labMoveSelection(1);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    labAdjust(-1);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    labAdjust(1);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
      mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    labScreen = LabScreen::Viewer;
    renderBmp(false);
  }
}

void BmpViewerActivity::onEnter() {
  Activity::onEnter();

  if (siblingImages.empty() && !filePath.empty()) {
    loadSiblingImages();
  }

  isPng = FsHelpers::hasPngExtension(filePath);

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  if (isPng) {
    Rect popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
    GUI.fillPopupProgress(renderer, popupRect, 20);  // Initial 20% progress
    renderer.clearScreen();
    const bool hasPrevious = siblingImages.size() > 1 && currentImageIndex > 0;
    const bool hasNext = siblingImages.size() > 1 && currentImageIndex != -1 &&
                         currentImageIndex < static_cast<int>(siblingImages.size()) - 1;
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OPTIONS), "", "");
    if (renderPng()) {
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    } else {
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_FILE_OPEN_FAILED));
      GUI.drawButtonHints(renderer, labels.btn1, "", "", "");
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }
    return;
  }

  renderBmp(true);
}

void BmpViewerActivity::renderBmp(bool showPopup) {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  Rect popupRect;
  if (showPopup) {
    popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
    GUI.fillPopupProgress(renderer, popupRect, 20);  // Initial 20% progress
  }

  HalFile file;
  // 1. Open the BMP file
  if (!Storage.openFileForRead("BMP", filePath, file)) {
    // Handle file open error
    renderer.clearScreen();
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_FILE_OPEN_FAILED));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    return;
  }

  // Image Lab: always attach the lab config — quantizer selection is active
  // even when the tone LUT itself is at identity (enabled=false).
  Bitmap bitmap(file, true,
                renderer.grayscaleCapabilities(HalDisplay::GrayscaleMode::Absolute).supported() &&
                    display.getController() == HalDisplay::Controller::SSD1677,
                &labTone);

  // 2. Parse headers to get dimensions
  if (bitmap.parseHeaders() != BmpReaderError::Ok) {
    // Handle file parsing error
    renderer.clearScreen();
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_INVALID_BMP_FILE));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    return;
  }

  int x, y;

  if (bitmap.getWidth() > pageWidth || bitmap.getHeight() > pageHeight) {
    float ratio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
    const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);

    if (ratio > screenRatio) {
      // Wider than screen
      x = 0;
      y = std::round((static_cast<float>(pageHeight) - static_cast<float>(pageWidth) / ratio) / 2);
    } else {
      // Taller than screen
      x = std::round((static_cast<float>(pageWidth) - static_cast<float>(pageHeight) * ratio) / 2);
      y = 0;
    }
  } else {
    // Center small images
    x = (pageWidth - bitmap.getWidth()) / 2;
    y = (pageHeight - bitmap.getHeight()) / 2;
  }

  // 4. Prepare Rendering
  bool hasPrevious = (siblingImages.size() > 1 && currentImageIndex > 0);
  bool hasNext = (siblingImages.size() > 1 && currentImageIndex != -1 &&
                  currentImageIndex < static_cast<int>(siblingImages.size()) - 1);

  // Confirm opens the context menu; its hint reads as an active viewer action.
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OPTIONS), (hasPrevious ? "<" : ""),
                                            (hasNext ? ">" : ""));

  if (showPopup) GUI.fillPopupProgress(renderer, popupRect, 50);

  renderer.clearScreen();
  if (!renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, 0, 0)) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_FILE_OPEN_FAILED));
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    return;
  }
  drawLabIndicator(LabPass::Base);

  // Draw UI hints on the base layer
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  if (bitmap.hasGreyscale()) {
    const bool absolute = renderer.grayscaleCapabilities(HalDisplay::GrayscaleMode::Absolute).supported();
    if (absolute && !renderer.displayGrayscaleBase(HalDisplay::GrayscaleMode::Absolute)) return;
    if (!absolute) renderer.displayGrayscaleBase(HalDisplay::HALF_REFRESH);
    bool planesReady = true;
    for (const auto mode : {GfxRenderer::GRAYSCALE_LSB, GfxRenderer::GRAYSCALE_MSB}) {
      if (bitmap.rewindToData() != BmpReaderError::Ok) {
        LOG_ERR("BMP", "Failed to rewind bitmap for grayscale rendering");
        planesReady = false;
        break;
      }
      renderer.clearScreen(absolute ? 0xFF : 0x00);
      renderer.setRenderMode(mode);
      if (!renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, 0, 0)) {
        planesReady = false;
        break;
      }
      drawLabIndicator(mode == GfxRenderer::GRAYSCALE_LSB ? LabPass::PlaneLsb : LabPass::PlaneMsb);
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      if (mode == GfxRenderer::GRAYSCALE_LSB) {
        renderer.copyGrayscaleLsbBuffers();
      } else {
        renderer.copyGrayscaleMsbBuffers();
      }
    }
    if (planesReady) renderer.displayGrayBuffer();

    // Rebuild the BW framebuffer for popups and subsequent differential updates.
    renderer.setRenderMode(GfxRenderer::BW);
    renderer.clearScreen();
    if (bitmap.rewindToData() != BmpReaderError::Ok ||
        !renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, 0, 0)) {
      LOG_ERR("BMP", "Failed to rewind bitmap to restore the BW framebuffer");
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_FILE_OPEN_FAILED));
      planesReady = false;
    }
    drawLabIndicator(LabPass::BwRebuild);
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.cleanupGrayscaleWithFrameBuffer();
    if (!planesReady) renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  } else {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  }
}

void BmpViewerActivity::onExit() {
  Activity::onExit();
  renderer.clearScreen();
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

void BmpViewerActivity::doSetSleepCover() {
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));

  const bool transparentMode = SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::TRANSPARENT_CUSTOM;
  if (!canSetSleepCover()) return;

  const char* destination =
      transparentMode ? (FsHelpers::hasPngExtension(filePath) ? TRANSPARENT_SLEEP_ROOT_PNG : TRANSPARENT_SLEEP_ROOT_BMP)
                      : CUSTOM_SLEEP_ROOT_BMP;
  bool success = filePath == destination;

  if (!success) {
    auto buffer = makeUniqueNoThrow<uint8_t[]>(COPY_BUFFER_SIZE);
    if (!buffer) {
      LOG_ERR("BMP", "OOM: sleep cover copy buffer");
    } else {
      HalFile inFile, outFile;
      if (Storage.openFileForRead("BMP", filePath, inFile) && Storage.openFileForWrite("BMP", destination, outFile)) {
        int bytesRead;
        success = true;
        while ((bytesRead = inFile.read(buffer.get(), COPY_BUFFER_SIZE)) > 0) {
          if (outFile.write(buffer.get(), static_cast<size_t>(bytesRead)) != static_cast<size_t>(bytesRead)) {
            success = false;
            break;
          }
        }
        if (bytesRead < 0) success = false;
        outFile.close();
      }
    }
  }

  if (success) {
    if (!transparentMode) SETTINGS.sleepScreen = CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM;
    SETTINGS.saveToFile();
    GUI.drawPopup(renderer, tr(STR_DONE));
  } else {
    GUI.drawPopup(renderer, tr(STR_FAILED_LOWER));
  }

  delay(1000);
  onEnter();
}

void BmpViewerActivity::loop() {
  // Keep CPU awake/polling so 1st click works
  Activity::loop();

  auto openSibling = [this](const int delta) {
    if (currentImageIndex < 0) {
      return false;
    }
    const int nextIndex = currentImageIndex + delta;
    if (siblingImages.size() <= 1 || nextIndex < 0 || nextIndex >= static_cast<int>(siblingImages.size())) {
      return false;
    }
    currentImageIndex = nextIndex;
    std::string dirPath = FsHelpers::extractFolderPath(filePath);
    if (dirPath.back() != '/') dirPath += "/";
    filePath = dirPath + siblingImages[currentImageIndex];
    isPng = FsHelpers::hasPngExtension(filePath);
    onEnter();
    return true;
  };

  // Modal input ordering: an active OptionPopup owns ALL viewer input. While
  // it is active, handleInput() runs and loop() returns — no viewer action,
  // no sibling navigation, no image repaint runs. Its requestUpdate callback
  // repaints the popup synchronously (this activity renders in-loop, unlike
  // the render-task activities that pass requestUpdate() and repaint via
  // render()); an empty lambda would move the selection without ever
  // repainting — the "menu ignores navigation keys" bug.
  if (menuPopup.isActive()) {
    menuPopup.handleInput(mappedInput, [this] { menuPopup.processRender(renderer, mappedInput); });
    if (!menuPopup.isActive() && labScreen == LabScreen::ContextMenu) {
      // Dismissed without a selection: restore the image view.
      labScreen = LabScreen::Viewer;
      renderBmp(false);
    }
    return;
  }

  if (confirmPopup.isActive()) {
    confirmPopup.handleInput(mappedInput, [this] { confirmPopup.processRender(renderer, mappedInput); });
    if (!confirmPopup.isActive() && labScreen == LabScreen::DeleteConfirm) {
      // Cancelled (Back or outside-tap): restore the image view.
      labScreen = LabScreen::Viewer;
      renderBmp(false);
    }
    return;
  }

  if (labScreen == LabScreen::Settings) {
    handleLabSettingsInput();
    return;
  }

  if (labScreen == LabScreen::Info) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      labScreen = LabScreen::Viewer;
      renderBmp(false);
    }
    return;
  }

  // Viewer mode — native Image Viewer controls plus the lab menu trigger:
  //   Up (side, left of the right-side pair) / Left -> previous image
  //   Down (side) / Right (front) -> next image
  //   Confirm (GPIO8) -> context menu
  //   Back -> exit to the file browser
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    activityManager.goToFileBrowser(filePath);
    return;
  }

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Left) {
    openSibling(1);
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Right) {
    openSibling(-1);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    openSibling(-1);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    openContextMenu();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    openSibling(-1);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Right) ||
      mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    openSibling(1);
    return;
  }
}
