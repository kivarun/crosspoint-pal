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

// Image Lab indicator HUD patch (logical px, drawn over the image)
constexpr int LAB_PATCH_X = 2;
constexpr int LAB_PATCH_Y = 2;
constexpr int LAB_PATCH_W = 160;
constexpr int LAB_PATCH_H = 17;
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

void BmpViewerActivity::drawLabIndicatorText() {
  char buf[40];
  snprintf(buf, sizeof(buf), "B%u G%d.%02d C%u Q:%c  [%c]", labTone.brightnessPct, labTone.gammaPct / 100,
           labTone.gammaPct % 100, labTone.contrastPct, labTone.quantizer == 1 ? 'C' : 'L',
           labSelected == LabParam::Brightness ? 'B'
           : labSelected == LabParam::Gamma    ? 'G'
           : labSelected == LabParam::Contrast ? 'C'
                                               : 'Q');
  renderer.drawText(UI_10_FONT_ID, LAB_PATCH_X + 4, LAB_PATCH_Y + 1, buf, true);
}

void BmpViewerActivity::drawLabIndicator(LabPass pass) {
  if (isPng) return;

  if (pass == LabPass::PlaneLsb || pass == LabPass::PlaneMsb) {
    const bool absolute = renderer.grayPlanesAreAbsolute();
    if (absolute) {
      // Absolute planes: white patch (bits 1) + black text (bits 0) in each
      // plane -> black text on white patch in the composited output.
      renderer.fillRect(LAB_PATCH_X, LAB_PATCH_Y, LAB_PATCH_W, LAB_PATCH_H, false);
      drawLabIndicatorText();
    } else {
      // Overlay mask planes: bit 0 = no gray change -> keep the base pass
      // rendering of the indicator visible.
      renderer.fillRect(LAB_PATCH_X, LAB_PATCH_Y, LAB_PATCH_W, LAB_PATCH_H, true);
      drawLabIndicatorText();
    }
    return;
  }

  // BW base / BW rebuild passes: white patch + black text.
  renderer.fillRect(LAB_PATCH_X, LAB_PATCH_Y, LAB_PATCH_W, LAB_PATCH_H, false);
  drawLabIndicatorText();
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
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
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

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", (hasPrevious ? "-" : ""), (hasNext ? "+" : ""));

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

void BmpViewerActivity::labAdjust(int delta) {
  // Image Lab: adjust the selected parameter (Left = decrease, Right = increase;
  // clamped to the range bounds). Quantizer toggles on either press.
  switch (labSelected) {
    case LabParam::Brightness:
      labTone.brightnessPct = std::clamp<uint8_t>(labTone.brightnessPct + 5 * delta, 70, 110);
      break;
    case LabParam::Gamma:
      labTone.gammaPct = std::clamp<uint8_t>(labTone.gammaPct + 5 * delta, 70, 130);
      break;
    case LabParam::Contrast:
      labTone.contrastPct = std::clamp<uint8_t>(labTone.contrastPct + 5 * delta, 80, 130);
      break;
    case LabParam::Quantizer:
      labTone.quantizer = (labTone.quantizer == 1) ? 0 : 1;
      break;
    default:
      return;
  }
  buildToneLut(labTone);
  renderBmp(false);
}

void BmpViewerActivity::labCycle() {
  // Image Lab: select the next parameter (Brightness -> Gamma -> Contrast -> Quantizer).
  labSelected = static_cast<LabParam>((static_cast<uint8_t>(labSelected) + 1) % static_cast<uint8_t>(LabParam::Count));
  renderBmp(false);
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

  // Image Lab mapping (BMP only; PNG decode is not tuned by the lab):
  // Up / Down -> previous / next image
  // Left / Right -> decrease / increase the selected parameter
  // Confirm -> select the next parameter
  // Back -> exit viewer
  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    openSibling(-1);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    openSibling(1);
    return;
  }

  if (isPng) return;  // PNG: parameter keys are no-ops

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    labCycle();
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
}
