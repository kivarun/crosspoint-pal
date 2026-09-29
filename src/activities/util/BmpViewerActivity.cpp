#include "BmpViewerActivity.h"

#include <Bitmap.h>
#include <Epub/converters/PngToFramebufferConverter.h>
#include <FsHelpers.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>

#include "components/UiAppHelpers.h"

#include <LabSettingsInput.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "CrossPointSettings.h"
#include "components/UIScale.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace fui = freeink::ui;

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

// The modal panel hosts the largest page (info: title + 6 rows). Row cadence
// and header metrics come from the theme at compute time.
constexpr int LAB_MAX_DATA_ROWS = 6;

// Center the BMP/PNG image on the page (shared by the render paths).
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

std::string baseNameOf(const std::string& path) {
  const size_t lastSlash = path.find_last_of('/');
  return (lastSlash != std::string::npos) ? path.substr(lastSlash + 1) : path;
}
}  // namespace

BmpViewerActivity::BmpViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string path)
    : Activity("BmpViewer", renderer, mappedInput), filePath(std::move(path)) {}

void BmpViewerActivity::loadSiblingImages() {
  siblingImages.clear();
  currentImageIndex = -1;

  if (filePath.empty()) return;

  std::string dirPath = FsHelpers::extractFolderPath(filePath);
  const std::string fileName = baseNameOf(filePath);

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

void BmpViewerActivity::drawLabIndicatorText(int x, int y, const char* text) {
  renderer.drawText(UI_10_FONT_ID, x + LAB_HUD_PAD_X, y + LAB_HUD_PAD_Y, text, true);
}

void BmpViewerActivity::drawLabIndicator(LabPass pass) {
  // Lab HUD is BMP-only and user-togglable from the menu; when off it is
  // skipped in EVERY pass (base, both plane passes, BW rebuild).
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
  const int patchW = renderer.getTextWidth(UI_10_FONT_ID, buf) + LAB_HUD_PAD_X * 2;
  const int patchH = renderer.getLineHeight(UI_10_FONT_ID) + LAB_HUD_PAD_Y * 2;

  if (pass == LabPass::PlaneLsb || pass == LabPass::PlaneMsb) {
    if (renderer.grayPlanesAreAbsolute()) {
      // Absolute planes: white patch (bits 1) + black text (bits 0) in each
      // plane -> black text on white patch in the composited output.
      renderer.fillRect(hudX, hudY, patchW, patchH, false);
      drawLabIndicatorText(hudX, hudY, buf);
    } else {
      // Overlay mask planes: bit 0 = no gray change -> keep the base pass
      // rendering of the indicator visible.
      renderer.fillRect(hudX, hudY, patchW, patchH, true);
      drawLabIndicatorText(hudX, hudY, buf);
    }
    return;
  }

  // BW base / BW rebuild passes: white patch + black text.
  renderer.fillRect(hudX, hudY, patchW, patchH, false);
  drawLabIndicatorText(hudX, hudY, buf);
}

// ---------------------------------------------------------------------------
// Lab modal: ONE panel surface over the image; labScreen picks the page that
// fills it. Page transitions replace the page rows inside the same rect.
// ---------------------------------------------------------------------------

void BmpViewerActivity::computeLabModalRect() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();

  // Standard popup width (the OptionPopup/fui dialog convention: 3/4 of the
  // screen, clamped by the theme's side margins), centered.
  const int width = std::min<int>(screenW * 3 / 4, screenW - metrics.optionPopupDialogSideMargin * 2);

  // Height from the theme's row cadence: title header row + the largest page.
  const auto& theme = refreshSharedUiThemeTokens(makeUiTarget(renderer));
  const int rowH = theme.rowHeight > 36 ? theme.rowHeight : 36;  // list()'s raw minimum
  const int height = LAB_MAX_DATA_ROWS * rowH + metrics.popupFrameThickness * 2 + 8;

  labModalRect = fui::Rect{static_cast<int16_t>((screenW - width) / 2), static_cast<int16_t>((screenH - height) / 2),
                           static_cast<int16_t>(width), static_cast<int16_t>(height)};
}

void BmpViewerActivity::renderLabModal() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int border = metrics.popupFrameThickness;

  // Opaque single surface: theme border + solid white body. Nothing of the
  // previous page (or the image) bleeds through.
  renderer.fillRect(labModalRect.x, labModalRect.y, labModalRect.width, labModalRect.height, true);
  renderer.fillRect(labModalRect.x + border, labModalRect.y + border, labModalRect.width - border * 2,
                    labModalRect.height - border * 2, false);

  // FreeInkUI frame setup, mirroring OptionPopup::render (raw primitives; the
  // X4C has no touch, so the interaction buffer only satisfies the Frame API).
  fui::GfxRendererTarget target = makeUiTarget(renderer);
  const fui::ThemeTokens& theme = refreshSharedUiThemeTokens(target);
  const fui::DeviceContext device = target.deviceContext();
  const fui::InputSnapshot noInput{};
  labInteractions.beginPublishCycle();
  fui::Frame<LAB_INTERACTION_CAPACITY> frame(target, device, noInput, labInteractions);

  const int rows = buildLabPageItems();

  fui::ListProps props{};
  props.items = labItems;
  props.count = static_cast<uint16_t>(rows);
  props.scrollIndicator = false;
  props.labelText.font = fui::GfxRendererTarget::FONT_BODY;
  props.valueText.font = fui::GfxRendererTarget::FONT_BODY;
  fui::list(frame, fui::Rect{static_cast<int16_t>(labModalRect.x + border), static_cast<int16_t>(labModalRect.y + border),
                             static_cast<int16_t>(labModalRect.width - border * 2),
                             static_cast<int16_t>(labModalRect.height - border * 2)},
            props);
  labInteractions.publish();

  // Button hints reflect the REAL page semantics: 1D pages use the standard
  // Back/Select/Up/Down labels; the 2-axis settings page shows Back | Apply
  // on every row whose Confirm applies the draft to the viewer (editable rows
  // and the Apply row) and Select on the dedicated sleep row — never a hint
  // that Left/Right list rows.
  if (labScreen == LabScreen::ImageSettings) {
    const auto hint = mappedInput.mapLabels(tr(STR_BACK), labModalRow <= static_cast<int>(LabParam::Count)
                                                               ? tr(STR_APPLY)
                                                               : tr(STR_SELECT),
                                            "-", "+");
    GUI.drawButtonHints(renderer, hint.btn1, hint.btn2, hint.btn3, hint.btn4);
  } else {
    const auto hint = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, hint.btn1, hint.btn2, hint.btn3, hint.btn4);
  }
}

void BmpViewerActivity::repaintLabModal() {
  renderLabModal();
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

int BmpViewerActivity::buildLabPageItems() {
  int count = 0;
  menuActionCount = 0;
  // Adds the page title as a non-selectable section header row, then rows;
  // rows get the focus state at the current selection. For the menu page the
  // dynamic row order is recorded into menuActions[].
  const auto add = [&](const char* label, const char* value, const bool header = false) {
    auto& item = labItems[count++];
    item = fui::ListItem{};
    item.label = label;
    item.value = value;
    item.isHeader = header;
    if (!header && labModalRow == count - 2) item.state = fui::StateFocused;
  };

  switch (labScreen) {
    case LabScreen::ContextMenu:
      add(tr(STR_IMAGE_LAB), nullptr, true);
      menuActionCount = 0;
      add(tr(STR_IMAGE_SETTINGS), nullptr);
      menuActions[menuActionCount++] = LAB_ACT_SETTINGS;
      add(tr(STR_INFO), nullptr);
      menuActions[menuActionCount++] = LAB_ACT_INFO;
      if (!isPng) {
        auto& item = labItems[count];
        add(tr(STR_SHOW_DEBUG_INFO), nullptr);
        GUI.setCheckboxRow(item, debugHudEnabled);  // upstream checkbox row API
        menuActions[menuActionCount++] = LAB_ACT_DEBUG_HUD;
      }
      if (canSetSleepCover()) {
        add(tr(STR_SET_SLEEP_COVER), nullptr);
        menuActions[menuActionCount++] = LAB_ACT_SLEEP;
      }
      add(tr(STR_DELETE), nullptr);
      menuActions[menuActionCount++] = LAB_ACT_DELETE;
      break;

    case LabScreen::ImageSettings: {
      // Values come from the STAGED draft (labTone is untouched until Apply).
      snprintf(labValueScratch[0], sizeof(labValueScratch[0]), "%u %%", settingsDraft.brightnessPct);
      snprintf(labValueScratch[1], sizeof(labValueScratch[1]), "%d.%02d", settingsDraft.gammaPct / 100,
               settingsDraft.gammaPct % 100);
      snprintf(labValueScratch[2], sizeof(labValueScratch[2]), "%u %%", settingsDraft.contrastPct);
      snprintf(labValueScratch[3], sizeof(labValueScratch[3]), "%s",
               settingsDraft.quantizer == 1 ? tr(STR_QUANTIZER_CANONICAL) : tr(STR_QUANTIZER_LEGACY));

      add(tr(STR_IMAGE_SETTINGS), nullptr, true);
      add(tr(STR_BRIGHTNESS), labValueScratch[0]);
      add(tr(STR_GAMMA), labValueScratch[1]);
      add(tr(STR_FILTER_CONTRAST), labValueScratch[2]);
      add(tr(STR_QUANTIZER), labValueScratch[3]);
      add(tr(STR_APPLY), nullptr);
      add(tr(STR_USE_FOR_SLEEP_RENDERING), nullptr);
      break;
    }

    case LabScreen::ImageInfo:
      add(tr(STR_IMAGE_INFO), nullptr, true);
      for (const auto& row : infoRows) {
        add(row.first.c_str(), row.second.c_str());
      }
      break;

    case LabScreen::DeleteConfirm:
      add(tr(STR_DELETE), nullptr, true);
      add(tr(STR_NAME), labHeadline.c_str());
      add(tr(STR_CANCEL), nullptr);
      add(tr(STR_DELETE), nullptr);
      break;

    default:
      break;
  }
  return count;
}

int BmpViewerActivity::labPageRowCount() const {
  // Selectable rows of the current page (title headers are not selectable).
  switch (labScreen) {
    case LabScreen::ContextMenu: {
      int rows = 3;  // Image settings / Info / Delete
      if (!isPng) rows++;
      if (canSetSleepCover()) rows++;
      return rows;
    }
    case LabScreen::ImageSettings:
      return static_cast<int>(LabParam::Count) + 2;  // 4 value rows + Apply + Use for sleep rendering
    case LabScreen::ImageInfo:
      return static_cast<int>(infoRows.size());
    case LabScreen::DeleteConfirm:
      return 2;  // Cancel / Delete
    default:
      return 0;
  }
}

void BmpViewerActivity::openLabModal() {
  computeLabModalRect();
  labScreen = LabScreen::ContextMenu;
  labModalRow = 0;
  repaintLabModal();
}

void BmpViewerActivity::menuAction(int action) {
  // Page transitions inside the ONE modal surface — never a nested popup and
  // never an image re-render (except the debug toggle, which must repaint the
  // image to physically add/remove the HUD).
  switch (action) {
    case LAB_ACT_SETTINGS:
      // Stage: copy the active settings into the draft; nothing is committed
      // and the image is not re-rendered while stepping.
      settingsDraft = labTone;
      labScreen = LabScreen::ImageSettings;
      labModalRow = 0;
      repaintLabModal();
      break;
    case LAB_ACT_INFO:
      openInfoPage();
      break;
    case LAB_ACT_DEBUG_HUD:
      debugHudEnabled = !debugHudEnabled;
      labScreen = LabScreen::Viewer;
      renderBmp(false);  // full re-render so the old HUD physically leaves the panel
      labScreen = LabScreen::ContextMenu;
      labModalRow = 0;
      repaintLabModal();
      break;
    case LAB_ACT_SLEEP:
      labScreen = LabScreen::Viewer;  // modal closes; the sleep-cover flow re-renders itself
      doSetSleepCover();              // existing implementation, unchanged; menu-driven
      break;
    case LAB_ACT_DELETE:
      labHeadline = baseNameOf(filePath);
      labScreen = LabScreen::DeleteConfirm;
      labModalRow = 0;  // default on Cancel
      repaintLabModal();
      break;
    default:
      break;
  }
}

void BmpViewerActivity::openInfoPage() {
  labScreen = LabScreen::ImageInfo;
  labModalRow = 0;
  infoRows.clear();
  infoRows.reserve(6);

  infoRows.emplace_back(tr(STR_NAME), baseNameOf(filePath));
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
      snprintf(line, sizeof(line), "%u B", static_cast<unsigned>(file.size()));
      infoRows.emplace_back(tr(STR_FILE_SIZE), line);
    } else {
      infoRows.emplace_back(tr(STR_FORMAT), "File unavailable");
    }
  }

  // Truncate oversized values (long paths) to the value column: the label
  // column and the text gap mirror fui::list's default row layout.
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int border = metrics.popupFrameThickness;
  const int sidePad = 8;  // list()'s raw sidePadding default
  const int textGap = 10;
  int labelColW = 0;
  for (const auto& row : infoRows) {
    labelColW = std::max(labelColW, renderer.getTextWidth(UI_10_FONT_ID, row.first.c_str()));
  }
  const int availW = labModalRect.width - border * 2 - sidePad * 2 - labelColW - textGap;
  const int valueFont = uiScaleSpec().bodyFontId;
  for (auto& row : infoRows) {
    if (renderer.getTextWidth(valueFont, row.second.c_str()) > availW) {
      row.second = renderer.truncatedText(valueFont, row.second.c_str(), availW);
    }
  }

  repaintLabModal();
}

void BmpViewerActivity::performDelete() {  const std::string dirPath = FsHelpers::extractFolderPath(filePath);
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

void BmpViewerActivity::handleLabModalInput() {
  // The lab modal owns ALL input while labScreen != Viewer: navigation moves
  // the page row, Confirm activates it, Back leaves the page. No viewer action
  // ever runs underneath.
  // Per-page input policy: Image settings is a 2-AXIS page (Up/Down choose the
  // row, Left/Right modify the value) and therefore uses the explicit logical
  // buttons — NOT the merged NavNext/NavPrevious list navigation, whose
  // Down||Right / Up||Left merge would make one front Left/Right event match
  // both branches. The 1D pages (menu / info / delete confirm) keep the
  // standard merged list navigation.
  if (labScreen == LabScreen::ImageSettings) {
    handleLabSettingsAxesInput();
    return;
  }

  const int rows = labPageRowCount();
  if (rows <= 0) return;

  if (mappedInput.wasPressed(MappedInputManager::Button::NavPrevious)) {
    labModalRow = (labModalRow - 1 + rows) % rows;
    repaintLabModal();  // modal-only repaint; the image is never re-rendered
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::NavNext)) {
    labModalRow = (labModalRow + 1) % rows;
    repaintLabModal();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateLabRow();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    labModalBack();
    return;
  }
}

void BmpViewerActivity::handleLabSettingsAxesInput() {
  // Image settings: explicit axes. Side Up/Down = vertical row selection;
  // front Left/Right = horizontal value stepping; Confirm/Back on release.
  // Dispatch is mutually exclusive (one event -> one branch -> return), so a
  // single event can never produce both a selection move and a value change.
  const auto dispatch = [&](const MappedInputManager::Button logical,
                            const labSettingsInput::Button axis) {
    return labSettingsInput::actionFor(axis, mappedInput.wasPressed(logical), mappedInput.wasReleased(logical));
  };

  switch (dispatch(MappedInputManager::Button::Up, labSettingsInput::Button::Up)) {
    case labSettingsInput::Action::RowUp:
      labModalRow = (labModalRow - 1 + labPageRowCount()) % labPageRowCount();
      repaintLabModal();  // modal-only repaint; the image is never re-rendered
      return;
    default:
      break;
  }
  switch (dispatch(MappedInputManager::Button::Down, labSettingsInput::Button::Down)) {
    case labSettingsInput::Action::RowDown:
      labModalRow = (labModalRow + 1) % labPageRowCount();
      repaintLabModal();
      return;
    default:
      break;
  }
  switch (dispatch(MappedInputManager::Button::Left, labSettingsInput::Button::Left)) {
    case labSettingsInput::Action::ValueDown:
      labModalAdjust(-1);  // staged draft only; no image render until Apply
      return;
    default:
      break;
  }
  switch (dispatch(MappedInputManager::Button::Right, labSettingsInput::Button::Right)) {
    case labSettingsInput::Action::ValueUp:
      labModalAdjust(1);
      return;
    default:
      break;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateLabRow();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    labModalBack();
    return;
  }
}

void BmpViewerActivity::activateLabRow() {
  switch (labScreen) {
    case LabScreen::ContextMenu:
      if (labModalRow >= 0 && labModalRow < menuActionCount) menuAction(menuActions[labModalRow]);
      break;

    case LabScreen::ImageSettings: {
      // Editable value rows AND the explicit Apply row share ONE viewer-Apply
      // implementation (Confirm applies without navigating to Apply); the
      // dedicated sleep row keeps its own action. Routing is the pure,
      // host-tested labSettingsInput policy — mutually exclusive by return.
      switch (labSettingsInput::confirmActionForRow(labModalRow, static_cast<int>(LabParam::Count))) {
        case labSettingsInput::RowAction::ApplyViewer:
          applyLabSettings();
          break;
        case labSettingsInput::RowAction::SaveSleepProfile:
          saveSleepRenderProfile();
          break;
        default:
          break;
      }
      break;
    }

    case LabScreen::ImageInfo:
      // Confirm returns to the root menu page (same surface, no image render).
      labScreen = LabScreen::ContextMenu;
      labModalRow = 0;
      repaintLabModal();
      break;

    case LabScreen::DeleteConfirm:
      if (labModalRow == 0) {
        // Cancel: back to the root menu page.
        labScreen = LabScreen::ContextMenu;
        labModalRow = 0;
        repaintLabModal();
      } else {
        // Delete: transactional contract unchanged (failed remove aborts).
        labScreen = LabScreen::Viewer;
        performDelete();
      }
      break;

    default:
      break;
  }
}

void BmpViewerActivity::labModalBack() {
  switch (labScreen) {
    case LabScreen::ContextMenu:
      // Root page Back closes the modal; the image re-renders (grayscale
      // pipeline) to restore the panel content under it.
      labScreen = LabScreen::Viewer;
      renderBmp(false);
      break;
    case LabScreen::ImageSettings:
      // Discard the staged draft (labTone untouched) and return to the root
      // menu page — modal-only repaint, no image re-render.
      labScreen = LabScreen::ContextMenu;
      labModalRow = 0;
      repaintLabModal();
      break;
    case LabScreen::ImageInfo:
    case LabScreen::DeleteConfirm:
      labScreen = LabScreen::ContextMenu;
      labModalRow = 0;
      repaintLabModal();
      break;
    default:
      break;
  }
}

void BmpViewerActivity::labModalAdjust(int delta) {
  // Settings page: Left/Right adjust the STAGED draft only — no buildToneLut(),
  // no image render until Apply. The quantizer row toggles on either press.
  if (labScreen != LabScreen::ImageSettings || labModalRow >= static_cast<int>(LabParam::Count)) {
    return;  // other pages / the Apply row have nothing to step
  }
  switch (static_cast<LabParam>(labModalRow)) {
    case LabParam::Brightness:
      settingsDraft.brightnessPct =
          labStepClamped(settingsDraft.brightnessPct, delta, LAB_BRIGHTNESS_MIN, LAB_BRIGHTNESS_MAX);
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
  repaintLabModal();
}

void BmpViewerActivity::applyLabSettings() {
  // The ONLY viewer-Apply implementation: commit the draft once, close the
  // ENTIRE modal UI, and render the current image exactly once — no return to
  // the root menu first. Reachable from every editable row and the Apply row.
  labTone = settingsDraft;
  buildToneLut(labTone);
  labScreen = LabScreen::Viewer;
  renderBmp(false);
}

void BmpViewerActivity::saveSleepRenderProfile() {
  // "Use for sleep rendering": persist the STAGED draft (not labTone — the two
  // may differ) as the sleep render profile. Transactional: a failed SD write
  // restores the previous profile (no unsaved-but-active state), shows the
  // failure popup (never a false Done), and stays on the settings page with
  // the draft intact for a retry. No image render, no labTone change, no
  // sleep-cover file/mode touch.
  ToneProfile candidate = toneProfileFromTone(settingsDraft);
  normalizeToneProfile(candidate);
  const ToneProfile previous = SETTINGS.sleepRenderProfile;
  SETTINGS.sleepRenderProfile = candidate;
  if (!SETTINGS.saveToFile()) {
    SETTINGS.sleepRenderProfile = previous;
    GUI.drawPopup(renderer, tr(STR_FAILED_LOWER));
    delay(1000);
    repaintLabModal();
    return;
  }
  GUI.drawPopup(renderer, tr(STR_DONE));
  delay(1000);
  labScreen = LabScreen::ContextMenu;
  labModalRow = 0;
  repaintLabModal();
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

  // Confirm opens the lab modal; its hint reads as an active viewer action.
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

  // ONE modal owner: while labScreen != Viewer the lab modal surface owns ALL
  // input and loop() always returns after its handler — no sibling navigation,
  // no viewer buttons, no image repaint underneath.
  if (labScreen != LabScreen::Viewer) {
    handleLabModalInput();
    return;
  }

  // Viewer mode — native Image Viewer controls plus the lab modal trigger:
  //   Up (side) / Left (front) -> previous image
  //   Down (side) / Right (front) -> next image
  //   Confirm (GPIO8) -> lab modal (Options)
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
    openLabModal();
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
