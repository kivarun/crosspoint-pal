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
#include <cmath>
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
// HUD patch padding around the measured text (not a fixed patch size: the
// background is sized from real font metrics at draw time).
constexpr int LAB_HUD_PAD_X = 4;
constexpr int LAB_HUD_PAD_Y = 2;

// Centered modal dialog metrics (padding in px; everything else derived from
// font metrics / theme tokens at draw time).
constexpr int LAB_DIALOG_PAD = 8;
constexpr int LAB_DIALOG_ROW_GAP = 4;
constexpr int LAB_DIALOG_TITLE_GAP = 4;
constexpr int LAB_DIALOG_COL_GAP = 14;

// Center the BMP/PNG image on the page (shared by the render and the
// framebuffer-repair paths).
void fitImageOnScreen(const int imageW, const int imageH, const int pageW, const int pageH, int* x, int* y) {
  if (imageW > pageW || imageH > pageH) {
    const float ratio = static_cast<float>(imageW) / static_cast<float>(imageH);
    const float screenRatio = static_cast<float>(pageW) / static_cast<float>(pageH);
    if (ratio > screenRatio) {
      *x = 0;
      *y = std::round((static_cast<float>(pageH) - static_cast<float>(pageW) / ratio) / 2);
    } else {
      *x = std::round((static_cast<float>(pageW) - static_cast<float>(pageH) * ratio) / 2);
      *y = 0;
    }
  } else {
    *x = (pageW - imageW) / 2;
    *y = (pageH - imageH) / 2;
  }
}
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

BmpViewerActivity::LabDialogLayout BmpViewerActivity::drawLabDialogFrame(const char* title, int colW, int labelColW,
                                                                         int rowCount) {
  LabDialogLayout d{};
  d.lineH = renderer.getLineHeight(UI_10_FONT_ID);
  d.rowH = d.lineH + LAB_DIALOG_ROW_GAP;
  const int titleW = renderer.getTextWidth(UI_10_FONT_ID, title);
  const int contentW = std::max(colW, titleW);
  d.w = contentW + LAB_DIALOG_PAD * 2;
  d.h = LAB_DIALOG_PAD + d.lineH + LAB_DIALOG_TITLE_GAP + rowCount * d.rowH + LAB_DIALOG_PAD;
  d.x = (renderer.getScreenWidth() - d.w) / 2;
  d.y = (renderer.getScreenHeight() - d.h) / 2;

  // Opaque dialog body with the theme's popup frame thickness as border.
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int border = metrics.popupFrameThickness;
  renderer.fillRect(d.x, d.y, d.w, d.h, true);                              // border
  renderer.fillRect(d.x + border, d.y + border, d.w - border * 2, d.h - border * 2, false);  // body
  renderer.drawText(UI_10_FONT_ID, d.x + (d.w - titleW) / 2, d.y + LAB_DIALOG_PAD, title, true);

  d.textX = d.x + LAB_DIALOG_PAD;
  d.textY = d.y + LAB_DIALOG_PAD + d.lineH + LAB_DIALOG_TITLE_GAP;
  d.valueColX = d.textX + labelColW + LAB_DIALOG_COL_GAP;
  return d;
}

void BmpViewerActivity::drawLabIndicatorText(int x, int y) {
  char buf[32];
  snprintf(buf, sizeof(buf), "B%u G%d.%02d C%u Q:%c", labTone.brightnessPct, labTone.gammaPct / 100,
           labTone.gammaPct % 100, labTone.contrastPct, labTone.quantizer == 1 ? 'C' : 'L');
  renderer.drawText(UI_10_FONT_ID, x + LAB_HUD_PAD_X, y + LAB_HUD_PAD_Y, buf, true);
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

  // Patch size from real font metrics: measured text width / line height plus
  // padding — fully covers the glyphs, never a hand-tuned fixed rect.
  char buf[32];
  snprintf(buf, sizeof(buf), "B%u G%d.%02d C%u Q:%c", labTone.brightnessPct, labTone.gammaPct / 100,
           labTone.gammaPct % 100, labTone.contrastPct, labTone.quantizer == 1 ? 'C' : 'L');
  const int textW = renderer.getTextWidth(UI_10_FONT_ID, buf);
  const int patchW = textW + LAB_HUD_PAD_X * 2;
  const int patchH = renderer.getLineHeight(UI_10_FONT_ID) + LAB_HUD_PAD_Y * 2;

  if (pass == LabPass::PlaneLsb || pass == LabPass::PlaneMsb) {
    if (renderer.grayPlanesAreAbsolute()) {
      // Absolute planes: white patch (bits 1) + black text (bits 0) in each
      // plane -> black text on white patch in the composited output.
      renderer.fillRect(hudX, hudY, patchW, patchH, false);
      drawLabIndicatorText(hudX, hudY);
    } else {
      // Overlay mask planes: bit 0 = no gray change -> keep the base pass
      // rendering of the indicator visible.
      renderer.fillRect(hudX, hudY, patchW, patchH, true);
      drawLabIndicatorText(hudX, hudY);
    }
    return;
  }

  // BW base / BW rebuild passes: white patch + black text.
  renderer.fillRect(hudX, hudY, patchW, patchH, false);
  drawLabIndicatorText(hudX, hudY);
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
    case LAB_ACT_SETTINGS:
      openLabSettings();
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

void BmpViewerActivity::drawLabSettingsDialog() {
  // Values come from the STAGED draft (labTone is untouched until Apply).
  char valueBuf[4][24];
  snprintf(valueBuf[0], sizeof(valueBuf[0]), "%u %%", settingsDraft.brightnessPct);
  snprintf(valueBuf[1], sizeof(valueBuf[1]), "%d.%02d", settingsDraft.gammaPct / 100, settingsDraft.gammaPct % 100);
  snprintf(valueBuf[2], sizeof(valueBuf[2]), "%u %%", settingsDraft.contrastPct);
  snprintf(valueBuf[3], sizeof(valueBuf[3]), "%s",
           settingsDraft.quantizer == 1 ? tr(STR_QUANTIZER_CANONICAL) : tr(STR_QUANTIZER_LEGACY));

  // Column widths from real text metrics.
  const char* labels[] = {tr(STR_BRIGHTNESS), tr(STR_GAMMA), tr(STR_FILTER_CONTRAST), tr(STR_QUANTIZER)};
  const int applyW = renderer.getTextWidth(UI_10_FONT_ID, tr(STR_APPLY));
  int labelColW = 0;
  int valueColW = 0;
  for (int i = 0; i < static_cast<int>(LabParam::Count); i++) {
    labelColW = std::max(labelColW, renderer.getTextWidth(UI_10_FONT_ID, labels[i]));
    valueColW = std::max(valueColW, renderer.getTextWidth(UI_10_FONT_ID, valueBuf[i]));
  }
  const int colW = LAB_DIALOG_COL_GAP + labelColW + LAB_DIALOG_COL_GAP + valueColW;

  const auto d = drawLabDialogFrame(tr(STR_IMAGE_SETTINGS), colW, labelColW,
                                    static_cast<int>(LabParam::Count) + 1 /* Apply row */);

  for (int i = 0; i <= static_cast<int>(LabParam::Count); i++) {
    const int rowY = d.textY + i * d.rowH;
    if (labSettingsRow == i) renderer.drawText(UI_10_FONT_ID, d.textX, rowY, ">", true);
    if (i < static_cast<int>(LabParam::Count)) {
      renderer.drawText(UI_10_FONT_ID, d.valueColX - LAB_DIALOG_COL_GAP, rowY, labels[i], true);
      renderer.drawText(UI_10_FONT_ID, d.valueColX, rowY, valueBuf[i], true);
    } else {
      renderer.drawText(UI_10_FONT_ID, d.valueColX - LAB_DIALOG_COL_GAP, rowY, tr(STR_APPLY), true);
    }
  }

  const auto labelsHint = mappedInput.mapLabels(tr(STR_BACK), "", "-", "+");
  GUI.drawButtonHints(renderer, labelsHint.btn1, labelsHint.btn2, labelsHint.btn3, labelsHint.btn4);
}

void BmpViewerActivity::openLabSettings() {
  // Stage: copy the active settings into the draft; nothing is committed and
  // the image is not re-rendered while stepping.
  settingsDraft = labTone;
  labSettingsRow = 0;
  labScreen = LabScreen::Settings;
  drawLabSettingsDialog();
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void BmpViewerActivity::applyLabSettings() {
  // Apply: commit the draft once, then close BOTH the settings dialog and the
  // parent context menu and re-render the current image a single time.
  labTone = settingsDraft;
  buildToneLut(labTone);
  labScreen = LabScreen::Viewer;
  renderBmp(false);
}

void BmpViewerActivity::drawInfoDialog() {
  // Two-column table from the pre-collected rows (labels left, values right).
  int labelColW = 0;
  int valueColW = 0;
  for (const auto& row : infoRows) {
    labelColW = std::max(labelColW, renderer.getTextWidth(UI_10_FONT_ID, row.first.c_str()));
    valueColW = std::max(valueColW, renderer.getTextWidth(UI_10_FONT_ID, row.second.c_str()));
  }
  const int colW = LAB_DIALOG_COL_GAP + labelColW + LAB_DIALOG_COL_GAP + valueColW;
  const auto d = drawLabDialogFrame(tr(STR_IMAGE_INFO), colW, labelColW, static_cast<int>(infoRows.size()));

  for (size_t i = 0; i < infoRows.size(); i++) {
    const int rowY = d.textY + static_cast<int>(i) * d.rowH;
    renderer.drawText(UI_10_FONT_ID, d.valueColX - LAB_DIALOG_COL_GAP, rowY, infoRows[i].first.c_str(), true);
    renderer.drawText(UI_10_FONT_ID, d.valueColX, rowY, infoRows[i].second.c_str(), true);
  }

  const auto labelsHint = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "", "");
  GUI.drawButtonHints(renderer, labelsHint.btn1, labelsHint.btn2, labelsHint.btn3, labelsHint.btn4);
}

void BmpViewerActivity::openInfo() {
  labScreen = LabScreen::Info;
  infoRows.clear();
  infoRows.reserve(6);

  size_t lastSlash = filePath.find_last_of('/');
  infoRows.emplace_back(tr(STR_NAME),
                        (lastSlash != std::string::npos) ? filePath.substr(lastSlash + 1) : filePath);
  infoRows.emplace_back(tr(STR_PATH), filePath);

  // Dimensions, format, bit depth, file size through the existing decode APIs
  // only (no new parser). PNG: the converter exposes dimensions only.
  char line[80];
  if (isPng) {
    ImageDimensions dimensions;
    if (PngToFramebufferConverter::getDimensionsStatic(filePath, dimensions)) {
      snprintf(line, sizeof(line), "%d x %d", dimensions.width, dimensions.height);
      infoRows.emplace_back(tr(STR_SIZE), line);
    }
    infoRows.emplace_back(tr(STR_FORMAT), "PNG");
  } else {
    HalFile file;
    if (Storage.openFileForRead("BMP", filePath, file)) {
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() == BmpReaderError::Ok) {
        snprintf(line, sizeof(line), "%d x %d", bitmap.getWidth(), bitmap.getHeight());
        infoRows.emplace_back(tr(STR_SIZE), line);
        infoRows.emplace_back(tr(STR_FORMAT), "BMP");
        snprintf(line, sizeof(line), "%ubpp %s", bitmap.getBpp(), bitmap.is1Bit() ? "BW" : "gray");
        infoRows.emplace_back(tr(STR_BIT_DEPTH), line);
      } else {
        infoRows.emplace_back(tr(STR_FORMAT), "BMP (parse failed)");
      }
      snprintf(line, sizeof(line), "%u bytes", static_cast<unsigned>(file.size()));
      infoRows.emplace_back(tr(STR_FILE_SIZE), line);
    } else {
      infoRows.emplace_back(tr(STR_FORMAT), "File unavailable");
    }
  }

  // Truncate oversized values (long paths) to the dialog's value column.
  int labelColW = 0;
  for (const auto& row : infoRows) {
    labelColW = std::max(labelColW, renderer.getTextWidth(UI_10_FONT_ID, row.first.c_str()));
  }
  const int maxDialogW = renderer.getScreenWidth() * 3 / 4;
  const int availW = maxDialogW - 2 * LAB_DIALOG_PAD - labelColW - LAB_DIALOG_COL_GAP * 2;
  for (auto& row : infoRows) {
    if (renderer.getTextWidth(UI_10_FONT_ID, row.second.c_str()) > availW) {
      row.second = renderer.truncatedText(UI_10_FONT_ID, row.second.c_str(), availW);
    }
  }

  drawInfoDialog();
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
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

bool BmpViewerActivity::rebuildBwFramebuffer() {
  // Repaint the BW framebuffer with the current image + lab chrome WITHOUT any
  // panel refresh — used before overpainting modal remnants with another
  // dialog (return to the parent menu from Settings/Info must not run the
  // full grayscale pipeline).
  if (isPng) {
    renderer.clearScreen();
    if (!renderPng()) return false;
    GUI.drawButtonHints(renderer, "", "", "", "");
    return true;
  }

  HalFile file;
  if (!Storage.openFileForRead("BMP", filePath, file)) return false;
  Bitmap bitmap(file, true,
                renderer.grayscaleCapabilities(HalDisplay::GrayscaleMode::Absolute).supported() &&
                    display.getController() == HalDisplay::Controller::SSD1677,
                &labTone);
  if (bitmap.parseHeaders() != BmpReaderError::Ok) return false;

  int x, y;
  fitImageOnScreen(bitmap.getWidth(), bitmap.getHeight(), renderer.getScreenWidth(), renderer.getScreenHeight(), &x,
                   &y);

  renderer.setRenderMode(GfxRenderer::BW);
  renderer.clearScreen();
  if (!renderer.drawBitmap(bitmap, x, y, renderer.getScreenWidth(), renderer.getScreenHeight(), 0, 0)) return false;
  drawLabIndicator(LabPass::Base);
  return true;
}

void BmpViewerActivity::labMoveSettingsRow(int delta) {
  // Side Up/Down move the row cursor (parameters, then Apply); wrap like the
  // OptionPopup menus. Cursor-only repaint: the image is never re-rendered.
  const int rowCount = static_cast<int>(LabParam::Count) + 1;
  labSettingsRow = (labSettingsRow + delta + rowCount) % rowCount;
  drawLabSettingsDialog();
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void BmpViewerActivity::labAdjustDraft(int delta) {
  // Left/Right adjust the STAGED draft only — no buildToneLut(), no image
  // render until Apply. The quantizer row toggles on either press.
  if (labSettingsRow >= static_cast<int>(LabParam::Count)) return;  // Apply row: nothing to step
  switch (static_cast<LabParam>(labSettingsRow)) {
    case LabParam::Brightness:
      settingsDraft.brightnessPct = labStepClamped(settingsDraft.brightnessPct, delta, LAB_BRIGHTNESS_MIN, LAB_BRIGHTNESS_MAX);
      break;
    case LabParam::Gamma:
      settingsDraft.gammaPct = labStepClamped(settingsDraft.gammaPct, delta, LAB_GAMMA_MIN, LAB_GAMMA_MAX);
      break;
    case LabParam::Contrast:
      settingsDraft.contrastPct = labStepClamped(settingsDraft.contrastPct, delta, LAB_CONTRAST_MIN, LAB_CONTRAST_MAX);
      break;
    case LabParam::Quantizer:
      settingsDraft.quantizer = (settingsDraft.quantizer == 1) ? 0 : 1;
      break;
    default:
      return;
  }
  drawLabSettingsDialog();
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void BmpViewerActivity::handleLabSettingsInput() {
  // Image settings modal: Up/Down (side keys) move the row, Left/Right adjust
  // the staged value, Confirm on the Apply row commits, Back discards the
  // draft and returns to the parent context menu.
  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    labMoveSettingsRow(-1);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    labMoveSettingsRow(1);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    labAdjustDraft(-1);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    labAdjustDraft(1);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (labSettingsRow == static_cast<int>(LabParam::Count)) applyLabSettings();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    // Discard the draft (labTone untouched) and reopen the parent menu; the
    // settings remnants are cleaned by a framebuffer-only repaint, not a
    // grayscale re-render.
    labScreen = LabScreen::ContextMenu;
    if (!rebuildBwFramebuffer()) renderBmp(false);
    openContextMenu();
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
  fitImageOnScreen(bitmap.getWidth(), bitmap.getHeight(), pageWidth, pageHeight, &x, &y);

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

  // Modal input ordering: every modal surface is checked BEFORE any viewer
  // action, and while one is active loop() ALWAYS returns after its handler —
  // no sibling navigation, no viewer buttons, no image repaint underneath.
  if (confirmPopup.isActive()) {
    confirmPopup.handleInput(mappedInput, [this] { confirmPopup.processRender(renderer, mappedInput); });
    if (!confirmPopup.isActive() && labScreen == LabScreen::DeleteConfirm) {
      // Cancelled (Back or outside-tap): restore the image view.
      labScreen = LabScreen::Viewer;
      renderBmp(false);
    }
    return;
  }

  if (menuPopup.isActive()) {
    menuPopup.handleInput(mappedInput, [this] { menuPopup.processRender(renderer, mappedInput); });
    if (!menuPopup.isActive() && labScreen == LabScreen::ContextMenu) {
      // Dismissed without a selection: restore the image view.
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
      // Return to the parent context menu without a grayscale re-render: the
      // info remnants are cleaned by a framebuffer-only repaint, then the
      // menu redraws over it with one fast refresh.
      labScreen = LabScreen::ContextMenu;
      if (!rebuildBwFramebuffer()) renderBmp(false);
      openContextMenu();
    }
    return;
  }

  // Viewer mode — native Image Viewer controls plus the lab menu trigger:
  //   Up (side) / Left (front) -> previous image
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
