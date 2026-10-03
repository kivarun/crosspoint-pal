#include "BmpViewerActivity.h"

#include <Bitmap.h>
#include <Epub/converters/PngToFramebufferConverter.h>
#include <FsHelpers.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>

#include "CrossPointSettings.h"
#include "components/UIScale.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"
#include "util/ImageOnlyRenderer.h"
#include "util/ImageSettingsInput.h"
#include "util/SlideshowPolicy.h"
#include "util/SlideshowState.h"

namespace fui = freeink::ui;

namespace {
constexpr char CUSTOM_SLEEP_ROOT_BMP[] = "/sleep.bmp";
constexpr char TRANSPARENT_SLEEP_ROOT_BMP[] = "/sleep-overlay.bmp";
constexpr char TRANSPARENT_SLEEP_ROOT_PNG[] = "/sleep-overlay.png";
constexpr size_t COPY_BUFFER_SIZE = 2048;

std::string baseNameOf(const std::string& path) {
  const size_t lastSlash = path.find_last_of('/');
  return (lastSlash != std::string::npos) ? path.substr(lastSlash + 1) : path;
}
}  // namespace

BmpViewerActivity::BmpViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string path,
                                     const bool slideshowResume)
    : Activity("BmpViewer", renderer, mappedInput),
      filePath(std::move(path)),
      slideshowResume(slideshowResume),
      activeTone(toneLutFromProfile(SETTINGS.viewerRenderProfile)) {
  // Session start: the ACTIVE tone state comes from the persisted viewer
  // profile, exactly once per session. Image navigation re-runs onEnter() but
  // must NOT re-read settings here (it would clobber an in-session Apply);
  // after Apply the profile and activeTone already agree.
  buildToneLut(activeTone);
}

void BmpViewerActivity::loadSiblingImages() {
  siblingImages.clear();
  currentImageIndex = -1;

  if (filePath.empty()) return;

  siblingImages = imageonly::listImageFiles(FsHelpers::extractFolderPath(filePath));

  const std::string fileName = baseNameOf(filePath);
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

// ---------------------------------------------------------------------------
// Modal: ONE panel surface over the image; viewerPage picks the page that
// fills it. Page transitions replace the page rows inside the same rect.
// ---------------------------------------------------------------------------

void BmpViewerActivity::computeModalRect() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();

  // Exact height of the LARGEST page — Image Settings — computed from the
  // same quantities buildSettingsPage() renders with (title header + the
  // page's rows at the resolved themed stride) plus the panel's borders and
  // padding. The other pages are header + fewer rows (Options ≤ 4, Info 7,
  // Delete 5), so the settings page is the sizing invariant. Clamped to the
  // screen so a degenerate theme cannot push the panel off it.
  fui::GfxRendererTarget target = makeUiTarget(renderer);
  const fui::ThemeTokens& tokens = refreshSharedUiThemeTokens(target);
  const fui::DeviceContext device = target.deviceContext();
  // ONE cadence for every modal page (list rows, settings rows, touch
  // minimum, panel sizing): touch-capable targets raise the row height to the
  // device's touch minimum so ensureMinTouchRect() never expands a hit band
  // into the neighboring row; button-only targets keep the 36px density. The
  // theme's row gap (with its touch comfort bump) rides on top of every
  // stride — sizing and rendering resolve it from the SAME tokens.
  modalRowH = static_cast<int16_t>(imageSettingsInput::modalRowHeight(device.hasTouch, device.minTouchSize));
  const modalTheme::ModalListTheme theme = modalTheme::resolve(tokens, device.hasTouch);
  modalRowGap = theme.rowGap;
  const int border = metrics.popupFrameThickness;

  // Width: the standard OptionPopup convention; on touch targets the two
  // stepper pages (Image Settings, Slideshow) may need more — derive each
  // page's minimum from MEASURED content (labels/values in the row fonts +
  // fixed controls at the touch minimum), take the max with the standard
  // width and clamp by the theme margins. Button-only targets keep the
  // compact width.
  int width = std::min<int>(screenW * 3 / 4, screenW - metrics.optionPopupDialogSideMargin * 2);
  if (device.hasTouch) {
    int16_t maxLabelWidth = 0;
    int16_t maxValueWidth = 0;
    int requiredBodyWidth = 0;
    measureStepperExtents(target, theme, maxLabelWidth, maxValueWidth);
    requiredBodyWidth = imageSettingsInput::stepperRequiredBodyWidth(
        theme.sidePadding, maxLabelWidth, maxValueWidth, target.lineHeight(fui::GfxRendererTarget::FONT_BODY),
        device.minTouchSize);
    measureSlideshowExtents(target, theme, maxLabelWidth, maxValueWidth);
    requiredBodyWidth = std::max<int>(requiredBodyWidth,
                                      imageSettingsInput::stepperRequiredBodyWidth(
                                          theme.sidePadding, maxLabelWidth, maxValueWidth,
                                          target.lineHeight(fui::GfxRendererTarget::FONT_BODY), device.minTouchSize));
    width = std::min<int>(std::max<int>(width, requiredBodyWidth + border * 2 + 8),
                          screenW - metrics.optionPopupDialogSideMargin * 2);
  }

  const int height =
      std::min<int>(screenH, imageSettingsInput::modalBodyHeight(modalHeaderHeight(target), modalRowH, modalRowGap,
                                                                 static_cast<int>(ToneParam::Count)) +
                                   border * 2 + 8);

  modalRect = fui::Rect{static_cast<int16_t>((screenW - width) / 2), static_cast<int16_t>((screenH - height) / 2),
                        static_cast<int16_t>(width), static_cast<int16_t>(height)};
}

int16_t BmpViewerActivity::modalHeaderHeight(const fui::DrawTarget& target) {
  // Raw list() header cadence: line height + 4 (the underline sits 2px below
  // the text). The header draws with the default TextStyle (font 0 =
  // FONT_SMALL), the same style buildSettingsPage() renders it with.
  return static_cast<int16_t>(target.lineHeight(fui::GfxRendererTarget::FONT_SMALL) + 4);
}

void BmpViewerActivity::measureStepperExtents(const fui::DrawTarget& target, const modalTheme::ModalListTheme& theme,
                                              int16_t& maxLabelWidth, int16_t& maxValueWidth) const {
  // The settings page's label/value measurement owner: the widest localized
  // row label (in the themed label style — title boldness included) and the
  // widest possible value text (static format maxima plus the widest
  // localized quantizer label) in the font the rows render values with.
  const fui::TextStyle& labelStyle = theme.bodyStyle;
  fui::TextStyle valueStyle{};
  valueStyle.font = fui::GfxRendererTarget::FONT_BODY;

  maxLabelWidth = 0;
  for (const char* label : {tr(STR_BRIGHTNESS), tr(STR_GAMMA), tr(STR_FILTER_CONTRAST), tr(STR_QUANTIZER)}) {
    maxLabelWidth = std::max<int16_t>(maxLabelWidth, target.measureText(labelStyle.font, label, labelStyle).width);
  }
  const char* quantizerWidest = tr(STR_QUANTIZER_DEFAULT);
  for (const char* candidate : {tr(STR_QUANTIZER_DEFAULT), tr(STR_QUANTIZER_LEGACY), tr(STR_QUANTIZER_CANONICAL)}) {
    if (target.measureText(valueStyle.font, candidate, valueStyle).width >
        target.measureText(valueStyle.font, quantizerWidest, valueStyle).width) {
      quantizerWidest = candidate;
    }
  }
  maxValueWidth = 0;
  for (const char* value : {"110 %", "1.30", "130 %", quantizerWidest}) {
    maxValueWidth = std::max<int16_t>(maxValueWidth, target.measureText(valueStyle.font, value, valueStyle).width);
  }
}

void BmpViewerActivity::measureSlideshowExtents(const fui::DrawTarget& target, const modalTheme::ModalListTheme& theme,
                                                int16_t& maxLabelWidth, int16_t& maxValueWidth) const {
  // The Slideshow page's label/value measurement owner: the widest row label
  // ("Start slideshow", themed label style) and the widest interval/order
  // value, in the same fonts the rows render with.
  const fui::TextStyle& labelStyle = theme.bodyStyle;
  const fui::TextStyle& valueStyle = labelStyle;

  maxLabelWidth = 0;
  for (const char* label : {tr(STR_START_SLIDESHOW), tr(STR_INTERVAL), tr(STR_SLIDESHOW_ORDER)}) {
    maxLabelWidth = std::max<int16_t>(maxLabelWidth, target.measureText(labelStyle.font, label, labelStyle).width);
  }
  maxValueWidth = 0;
  for (const char* value : {tr(STR_SLIDESHOW_INTERVAL_1_MIN), tr(STR_SLIDESHOW_INTERVAL_5_MIN),
                            tr(STR_SLIDESHOW_INTERVAL_10_MIN), tr(STR_SLIDESHOW_INTERVAL_30_MIN),
                            tr(STR_SLIDESHOW_ORDER_FORWARD), tr(STR_SLIDESHOW_ORDER_REVERSE),
                            tr(STR_SLIDESHOW_ORDER_RANDOM)}) {
    maxValueWidth = std::max<int16_t>(maxValueWidth, target.measureText(valueStyle.font, value, valueStyle).width);
  }
}

Rect BmpViewerActivity::modalAnchorRect() const {
  return Rect{modalRect.x, modalRect.y, modalRect.width, modalRect.height};
}

void BmpViewerActivity::renderModal() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int border = metrics.popupFrameThickness;
  const int cornerRadius = metrics.popupCornerRadius;

  // Opaque single surface in the theme's popup shape (the same primitives and
  // popup metrics BaseTheme::drawPopup draws confirmations with): solid white
  // body inside a black frame, rounded when the theme's popups are. Nothing
  // of the previous page (or the image) bleeds through.
  if (cornerRadius > 0) {
    renderer.fillRoundedRect(modalRect.x, modalRect.y, modalRect.width, modalRect.height, cornerRadius + border,
                             Color::Black);
    renderer.fillRoundedRect(modalRect.x + border, modalRect.y + border, modalRect.width - border * 2,
                             modalRect.height - border * 2, cornerRadius, Color::White);
  } else {
    renderer.fillRect(modalRect.x, modalRect.y, modalRect.width, modalRect.height, true);
    renderer.fillRect(modalRect.x + border, modalRect.y + border, modalRect.width - border * 2,
                      modalRect.height - border * 2, false);
  }

  // FreeInkUI frame setup, mirroring OptionPopup::render (raw primitives): the
  // frame registers each enabled row (and stepper control) on the interaction
  // buffer; the loop task routes touch snapshots against the published
  // generation (see handleModalInput). Physical buttons stay on the
  // mapped-input path, so the buffer is touch-only and never competes with
  // them for dispatch.
  fui::GfxRendererTarget target = makeUiTarget(renderer);
  // ONE theme resolution for the whole surface: the tokens' shape (row styles
  // + selection style, gap, radius, side padding, inset, text roles) drives
  // the list pages, the settingRow/stepperRow pages and the detail views
  // alike — the modal follows the active UI theme like every themed list.
  const fui::ThemeTokens& tokens = refreshSharedUiThemeTokens(target);
  const modalTheme::ModalListTheme theme = modalTheme::resolve(tokens, target.deviceContext().hasTouch);
  const fui::DeviceContext device = target.deviceContext();
  const fui::InputSnapshot noInput{};
  modalInteractions.beginPublishCycle();
  fui::Frame<MODAL_INTERACTION_CAPACITY> frame(target, device, noInput, modalInteractions);

  const fui::Rect body{static_cast<int16_t>(modalRect.x + border), static_cast<int16_t>(modalRect.y + border),
                       static_cast<int16_t>(modalRect.width - border * 2),
                       static_cast<int16_t>(modalRect.height - border * 2)};
  if (viewerPage == ViewerPage::ImageSettings) {
    // Stepper rows are SDK components, not list items — the settings page
    // lays out directly with the same cadence list() renders the other pages.
    buildSettingsPage(frame, body, theme);
  } else if (viewerPage == ViewerPage::Slideshow) {
    buildSlideshowPage(frame, body, theme);
  } else {
    const int rows = buildPageItems();

    fui::ListProps props{};
    props.items = modalItems;
    props.count = static_cast<uint16_t>(rows);
    props.scrollIndicator = false;
    props.action = ACTION_ROW;
    props.inputMask = fui::InputTouch;
    // The resolved modal cadence as an explicit row height: the visual row
    // and list()'s device-minimum-based hit rect then agree, so
    // ensureMinTouchRect() never expands one row's band into the next.
    // Labels follow the theme body style (title boldness); values keep the
    // body font the modal's cadence was measured for.
    props.rowHeight = modalRowH;
    props.valueText.font = fui::GfxRendererTarget::FONT_BODY;
    modalTheme::applyListProps(theme, props);
    fui::list(frame, body, props);
  }
  modalInteractions.publish();
  // Handshake re-opens only after a complete table is published (OptionPopup
  // model): until the first publication of the current page, routing refuses.
  modalUiReady = true;

  // Button hints reflect the REAL page semantics: 1D pages use the standard
  // Back/Select/Up/Down labels; the 2-axis settings page shows Back | Apply
  // on every row whose Confirm applies the draft to the viewer (editable rows
  // and the Apply row), Back | Reset on the draft-reset row and Back | Save on
  // the dedicated sleep row — never a hint that Left/Right list rows. The
  // Slideshow page shows Back | Start on EVERY row (Confirm starts from any
  // row; it is never a value edit) and Back | Start | - | + on the editable
  // value rows; its empty slots ERASE, because the page switches hint sets
  // between rows and skipped slots would keep stale frames on the strip (the
  // opt-in flag exists exactly for this modal-repaint case — the viewer's
  // other pages keep the skip semantics where empty slots intentionally leave
  // content visible). Image Info's Confirm is a detail SHOW (Name/Path), and
  // the Info detail view is informational: Back only, with the unused slots
  // erased because the strip shrinks from four labels to one.
  if (viewerPage == ViewerPage::ImageSettings) {
    const char* confirmLabel;
    if (modalRow == 0) {
      confirmLabel = tr(STR_RESET);
    } else if (modalRow == static_cast<int>(ToneParam::Count) + 2) {
      confirmLabel = tr(STR_SAVE);
    } else {
      confirmLabel = tr(STR_APPLY);  // editable value rows + the Apply row
    }
    const auto hint = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, "-", "+");
    GUI.drawButtonHints(renderer, hint.btn1, hint.btn2, hint.btn3, hint.btn4);
  } else if (viewerPage == ViewerPage::Slideshow) {
    const auto slots = slideshow::pageHintSlots(modalRow);
    const auto hint =
        mappedInput.mapLabels(tr(STR_BACK), slots.confirmStart ? tr(STR_START) : "+", slots.stepperSlots ? "-" : "",
                              slots.stepperSlots ? "+" : "");
    GUI.drawButtonHints(renderer, hint.btn1, hint.btn2, hint.btn3, hint.btn4, /*eraseUnused=*/true);
  } else if (viewerPage == ViewerPage::ImageInfo) {
    // Name/Path are the selectable rows and Confirm SHOWS their detail.
    const auto hint = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SHOW), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, hint.btn1, hint.btn2, hint.btn3, hint.btn4);
  } else if (viewerPage == ViewerPage::InfoDetail) {
    const auto hint = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, hint.btn1, hint.btn2, hint.btn3, hint.btn4, /*eraseUnused=*/true);
  } else {
    const auto hint = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, hint.btn1, hint.btn2, hint.btn3, hint.btn4);
  }
}

void BmpViewerActivity::repaintModal() {
  renderModal();
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

int BmpViewerActivity::buildPageItems() {
  int count = 0;
  menuActionCount = 0;
  int selectableIndex = 0;  // modalRow indexes ACTIONS, not visual rows
  // Adds a row. Selectable rows carry one stable action (ACTION_ROW) and their
  // actionValue IS the action index (the row Confirm/touch release activates);
  // headers and informational rows (selectable=false) are enabled=false —
  // list() registers no interaction for them (the SDK's non-interactive row),
  // and they never take focus — the focused visual row is the modalRow-th
  // SELECTABLE row, so what is visibly focused is always what Confirm will
  // execute.
  const auto add = [&](const char* label, const char* value, const bool header = false, const bool selectable = true) {
    auto& item = modalItems[count++];
    item = fui::ListItem{};
    item.label = label;
    item.value = value;
    item.isHeader = header;
    if (header || !selectable) {
      item.enabled = false;
      return;
    }
    item.actionValue = static_cast<int16_t>(selectableIndex);
    if (modalRow == selectableIndex) item.state = fui::StateFocused;
    ++selectableIndex;
  };

  switch (viewerPage) {
    case ViewerPage::Options:
      add(tr(STR_OPTIONS), nullptr, true);
      menuActionCount = 0;
      // Image Settings exist only for formats whose decode path consumes the
      // tone pipeline (BMP); PNG is offered Info/Delete/sleep-cover only.
      if (imageSettingsInput::imageSettingsAvailable(!isPng)) {
        add(tr(STR_IMAGE_SETTINGS), nullptr);
        menuActions[menuActionCount++] = ViewerAction::Settings;
      }
      add(tr(STR_SLIDESHOW), nullptr);
      menuActions[menuActionCount++] = ViewerAction::Slideshow;
      add(tr(STR_INFO), nullptr);
      menuActions[menuActionCount++] = ViewerAction::Info;
      if (canSetSleepCover()) {
        add(tr(STR_SET_SLEEP_COVER), nullptr);
        menuActions[menuActionCount++] = ViewerAction::SleepCover;
      }
      add(tr(STR_DELETE), nullptr);
      menuActions[menuActionCount++] = ViewerAction::Delete;
      break;

    case ViewerPage::ImageInfo:
      add(tr(STR_IMAGE_INFO), nullptr, true);
      // Name and Path are the page's ONLY selectable rows (Confirm opens the
      // value's detail view); the metadata rows (Size / Format / Bit depth /
      // File size) are informational: enabled=false renders the SDK's
      // disabled style, registers no interaction and never takes focus.
      for (size_t i = 0; i < infoPreviewRows.size(); ++i) {
        const bool selectable = i < imageSettingsInput::IMAGE_INFO_SELECTABLE_ROWS;
        add(infoPreviewRows[i].first.c_str(), infoPreviewRows[i].second.c_str(), false, selectable);
      }
      break;

    case ViewerPage::InfoDetail: {
      // Detail view: title header (the row's label) + the FULL value wrapped
      // to the body width — the delete page's measured primitive. No
      // selectable data rows: Confirm is inert, Back returns to Image Info.
      add(infoDetailLabel.c_str(), nullptr, true);
      for (const auto& line : infoDetailLines) {
        add(line.c_str(), nullptr, false, false);
      }
      break;
    }

    case ViewerPage::DeleteConfirm: {
      // Page order (documented + pinned by the host test): title header + Name
      // label + wrapped filename lines are informational (never selectable,
      // never focused); Cancel = action 0, Delete = action 1.
      add(tr(STR_DELETE), nullptr, true);
      add(tr(STR_NAME), nullptr, false, false);
      for (const auto& line : deleteNameLines) {
        add(line.c_str(), nullptr, false, false);
      }
      add(tr(STR_CANCEL), nullptr);
      add(tr(STR_DELETE), nullptr);
      break;
    }

    default:
      break;
  }
  return count;
}

void BmpViewerActivity::buildSettingsPage(fui::Frame<MODAL_INTERACTION_CAPACITY>& frame, const fui::Rect& body,
                                          const modalTheme::ModalListTheme& theme) {
  // Values come from the STAGED draft (activeTone is untouched until Apply).
  snprintf(valueScratch[0], sizeof(valueScratch[0]), "%u %%", draftProfile.brightnessPct);
  snprintf(valueScratch[1], sizeof(valueScratch[1]), "%d.%02d", draftProfile.gammaPct / 100,
           draftProfile.gammaPct % 100);
  snprintf(valueScratch[2], sizeof(valueScratch[2]), "%u %%", draftProfile.contrastPct);
  // Tri-state presentation, derived from the persisted value alone: -1 = no
  // override ("Default"), 0 = explicit Legacy, 1 = explicit Canonical.
  const char* quantizerText;
  switch (imageSettingsInput::quantizerLabel(draftProfile.quantizer)) {
    case imageSettingsInput::QuantizerLabel::Legacy:
      quantizerText = tr(STR_QUANTIZER_LEGACY);
      break;
    case imageSettingsInput::QuantizerLabel::Canonical:
      quantizerText = tr(STR_QUANTIZER_CANONICAL);
      break;
    default:
      quantizerText = tr(STR_QUANTIZER_DEFAULT);
      break;
  }
  snprintf(valueScratch[3], sizeof(valueScratch[3]), "%s", quantizerText);

  // Title header exactly as fui::list renders isHeader rows for this theme:
  // the theme's small text role + a 1px underline 2px below (header row =
  // line height + 4, shared with computeModalRect's sizing via
  // modalHeaderHeight()).
  const int16_t rowX = static_cast<int16_t>(body.x + theme.rowInset);
  const int16_t rowW = static_cast<int16_t>(body.width - theme.rowInset * 2);
  int16_t cursorY = body.y;
  const int16_t headerLh = frame.target().lineHeight(theme.headerStyle.font);
  frame.target().text(fui::Rect{static_cast<int16_t>(rowX + theme.sidePadding), cursorY,
                                static_cast<int16_t>(rowW - theme.sidePadding * 2), headerLh},
                      tr(STR_IMAGE_SETTINGS), theme.headerStyle);
  frame.target().fill(
      fui::Rect{static_cast<int16_t>(rowX + theme.sidePadding), static_cast<int16_t>(cursorY + headerLh + 2),
                static_cast<int16_t>(rowW - theme.sidePadding * 2), 1},
      fui::Paint::solid(theme.headerStyle.color));
  cursorY = static_cast<int16_t>(cursorY + modalHeaderHeight(frame.target()) + theme.rowGap);

  // Shared row contract: a full-row hit with the row's actionValue (the value
  // Confirm and touch activation dispatch on); the focused row follows
  // modalRow (the theme's selection style); minTouchSize = the row height, so
  // ensureMinTouchRect() never expands a hit band into the neighboring rows
  // (its centered expansion would hand boundary taps to the LATER row under
  // newest-first routing). settingRow registers the hit itself, so there is
  // no manual hit-testing anywhere on the page. Rows advance by the themed
  // stride (row height + gap) — the same values computeModalRect sized the
  // panel with.
  fui::TextStyle valueStyle{};
  valueStyle.font = fui::GfxRendererTarget::FONT_BODY;
  const auto rowProps = [&](const char* label, const int actionValue) {
    fui::SettingRowProps row{};
    row.label = label;
    row.valueText = valueStyle;
    row.action = ACTION_ROW;
    row.valueId = static_cast<int16_t>(actionValue);
    row.inputMask = fui::InputTouch;
    row.minTouchSize = modalRowH;
    if (modalRow == actionValue) row.state = fui::StateSelected;
    modalTheme::applySettingRow(theme, row);
    return row;
  };

  // Presentation layout follows the device capability (DeviceContext::hasTouch),
  // never a board name:
  // - button-only: the compact list view that passed X4 Classic UAT — plain
  //   label/value rows (values right-aligned by settingRow), no inline -/+
  //   controls; the hardware Left/Right keys stay represented by the bottom
  //   button hints.
  // - touch: the four editable rows share ONE page-wide fixed column geometry
  //   (label | - | value | +) so the controls hold identical X positions on
  //   every row regardless of the current value text.
  const bool hasTouch = frame.device().hasTouch;

  fui::SettingRowProps reset = rowProps(tr(STR_RESET_TO_DEFAULTS), 0);
  fui::settingRow(frame, fui::Rect{rowX, cursorY, rowW, modalRowH}, reset);
  cursorY = static_cast<int16_t>(cursorY + modalRowH + theme.rowGap);

  const char* labels[] = {tr(STR_BRIGHTNESS), tr(STR_GAMMA), tr(STR_FILTER_CONTRAST), tr(STR_QUANTIZER)};
  const char* widest[] = {"110 %", "1.30", "130 %", nullptr};
  // The quantizer's value slot must fit the widest localized label in pixels.
  const char* quantizerWidest = tr(STR_QUANTIZER_DEFAULT);
  for (const char* candidate : {tr(STR_QUANTIZER_DEFAULT), tr(STR_QUANTIZER_LEGACY), tr(STR_QUANTIZER_CANONICAL)}) {
    if (frame.target().measureText(fui::GfxRendererTarget::FONT_BODY, candidate, valueStyle).width >
        frame.target().measureText(fui::GfxRendererTarget::FONT_BODY, quantizerWidest, valueStyle).width) {
      quantizerWidest = candidate;
    }
  }
  widest[3] = quantizerWidest;

  if (!hasTouch) {
    for (int param = 0; param < static_cast<int>(ToneParam::Count); ++param) {
      auto row = rowProps(labels[param], param + 1);
      row.value = valueScratch[param];
      fui::settingRow(frame, fui::Rect{rowX, cursorY, rowW, modalRowH}, row);
      cursorY = static_cast<int16_t>(cursorY + modalRowH + theme.rowGap);
    }
  } else {
    // ONE page-wide fixed column set for every editable row: valueWidth from
    // the widest possible value text, buttonWidth at least the touch minimum
    // (a fixed column must never expand its hit into a neighbor). Identical
    // controlsW pins controlsX identically on all rows.
    int16_t maxLabelWidth = 0;
    int16_t maxValueWidth = 0;
    measureStepperExtents(frame.target(), theme, maxLabelWidth, maxValueWidth);
    const auto cols = imageSettingsInput::stepperColumns(rowW, theme.sidePadding, maxLabelWidth, maxValueWidth,
                                                         frame.target().lineHeight(fui::GfxRendererTarget::FONT_BODY),
                                                         frame.device().minTouchSize);
    for (int param = 0; param < static_cast<int>(ToneParam::Count); ++param) {
      fui::StepperRowProps stepper{};
      stepper.row = rowProps(labels[param], param + 1);
      stepper.value = valueScratch[param];
      stepper.widestValue = widest[param];
      stepper.buttonWidth = cols.buttonWidth;
      stepper.valueWidth = cols.valueWidth;
      stepper.gap = cols.gap;
      stepper.decrement = ACTION_DECREMENT;
      stepper.decrementValue = static_cast<int16_t>(param);
      stepper.increment = ACTION_INCREMENT;
      stepper.incrementValue = static_cast<int16_t>(param);
      fui::stepperRow(frame, fui::Rect{rowX, cursorY, rowW, modalRowH}, stepper);
      cursorY = static_cast<int16_t>(cursorY + modalRowH + theme.rowGap);
    }
  }

  const int paramCount = static_cast<int>(ToneParam::Count);
  fui::SettingRowProps apply = rowProps(tr(STR_APPLY), paramCount + 1);
  fui::settingRow(frame, fui::Rect{rowX, cursorY, rowW, modalRowH}, apply);
  cursorY = static_cast<int16_t>(cursorY + modalRowH + theme.rowGap);
  fui::SettingRowProps sleep = rowProps(tr(STR_USE_FOR_SLEEP_RENDERING), paramCount + 2);
  fui::settingRow(frame, fui::Rect{rowX, cursorY, rowW, modalRowH}, sleep);
}

void BmpViewerActivity::buildSlideshowPage(fui::Frame<MODAL_INTERACTION_CAPACITY>& frame, const fui::Rect& body,
                                           const modalTheme::ModalListTheme& theme) {
  // Title header exactly as fui::list renders isHeader rows for this theme
  // (shared cadence with the settings page).
  const int16_t rowX = static_cast<int16_t>(body.x + theme.rowInset);
  const int16_t rowW = static_cast<int16_t>(body.width - theme.rowInset * 2);
  int16_t cursorY = body.y;
  const int16_t headerLh = frame.target().lineHeight(theme.headerStyle.font);
  frame.target().text(fui::Rect{static_cast<int16_t>(rowX + theme.sidePadding), cursorY,
                                static_cast<int16_t>(rowW - theme.sidePadding * 2), headerLh},
                      tr(STR_SLIDESHOW), theme.headerStyle);
  frame.target().fill(
      fui::Rect{static_cast<int16_t>(rowX + theme.sidePadding), static_cast<int16_t>(cursorY + headerLh + 2),
                static_cast<int16_t>(rowW - theme.sidePadding * 2), 1},
      fui::Paint::solid(theme.headerStyle.color));
  cursorY = static_cast<int16_t>(cursorY + modalHeaderHeight(frame.target()) + theme.rowGap);

  // Row contract identical to the Image Settings page (full-row hit, focus
  // follows modalRow in the theme's selection style, minTouchSize = the row
  // height, themed stride).
  fui::TextStyle valueStyle{};
  valueStyle.font = fui::GfxRendererTarget::FONT_BODY;
  const auto rowProps = [&](const char* label, const int actionValue) {
    fui::SettingRowProps row{};
    row.label = label;
    row.valueText = valueStyle;
    row.action = ACTION_ROW;
    row.valueId = static_cast<int16_t>(actionValue);
    row.inputMask = fui::InputTouch;
    row.minTouchSize = modalRowH;
    if (modalRow == actionValue) row.state = fui::StateSelected;
    modalTheme::applySettingRow(theme, row);
    return row;
  };

  // Row 0: Start slideshow — arms the CURRENT image (Viewer mode) and hands
  // the frame sleep to the main loop.
  fui::settingRow(frame, fui::Rect{rowX, cursorY, rowW, modalRowH}, rowProps(tr(STR_START_SLIDESHOW), 0));
  cursorY = static_cast<int16_t>(cursorY + modalRowH + theme.rowGap);

  // Row 1: Interval — the shared persisted cadence; stepping persists
  // immediately and repaints the modal only (the image never re-renders).
  const char* intervalLabels[slideshow::INTERVAL_COUNT] = {
      tr(STR_SLIDESHOW_INTERVAL_1_MIN), tr(STR_SLIDESHOW_INTERVAL_5_MIN), tr(STR_SLIDESHOW_INTERVAL_10_MIN),
      tr(STR_SLIDESHOW_INTERVAL_30_MIN)};
  const uint8_t intervalIndex = slideshow::intervalIndexClamped(SETTINGS.slideshowInterval);
  const char* intervalWidest = intervalLabels[0];
  for (const char* candidate : intervalLabels) {
    if (frame.target().measureText(fui::GfxRendererTarget::FONT_BODY, candidate, valueStyle).width >
        frame.target().measureText(fui::GfxRendererTarget::FONT_BODY, intervalWidest, valueStyle).width) {
      intervalWidest = candidate;
    }
  }

  // Row 2: Order — the shared persisted frame order (Forward / Reverse /
  // Random), the same value-stepping semantics as the Interval row.
  const char* orderLabels[slideshow::ORDER_COUNT] = {tr(STR_SLIDESHOW_ORDER_FORWARD), tr(STR_SLIDESHOW_ORDER_REVERSE),
                                                     tr(STR_SLIDESHOW_ORDER_RANDOM)};
  const uint8_t orderIndex = slideshow::orderIndexClamped(SETTINGS.slideshowOrder);
  const char* orderWidest = orderLabels[0];
  for (const char* candidate : orderLabels) {
    if (frame.target().measureText(fui::GfxRendererTarget::FONT_BODY, candidate, valueStyle).width >
        frame.target().measureText(fui::GfxRendererTarget::FONT_BODY, orderWidest, valueStyle).width) {
      orderWidest = candidate;
    }
  }

  fui::SettingRowProps interval = rowProps(tr(STR_INTERVAL), slideshow::INTERVAL_PAGE_ROW);
  fui::SettingRowProps order = rowProps(tr(STR_SLIDESHOW_ORDER), slideshow::ORDER_PAGE_ROW);
  if (!frame.device().hasTouch) {
    // Button-only: compact value rows like the Image Settings page; the
    // hardware Left/Right keys step the selected row's value (button hints
    // say so).
    interval.value = intervalLabels[intervalIndex];
    fui::settingRow(frame, fui::Rect{rowX, cursorY, rowW, modalRowH}, interval);
    cursorY = static_cast<int16_t>(cursorY + modalRowH + theme.rowGap);
    order.value = orderLabels[orderIndex];
    fui::settingRow(frame, fui::Rect{rowX, cursorY, rowW, modalRowH}, order);
    return;
  }

  // Touch: the fixed stepper columns (label | - | value | +), sized from the
  // page's measured extents — the same architecture as Image Settings; both
  // value rows share ONE column set so the controls hold identical X
  // positions on every row.
  int16_t maxLabelWidth = 0;
  int16_t maxValueWidth = 0;
  measureSlideshowExtents(frame.target(), theme, maxLabelWidth, maxValueWidth);
  const auto cols = imageSettingsInput::stepperColumns(rowW, theme.sidePadding, maxLabelWidth, maxValueWidth,
                                                       frame.target().lineHeight(fui::GfxRendererTarget::FONT_BODY),
                                                       frame.device().minTouchSize);

  fui::StepperRowProps stepper{};
  stepper.row = interval;
  stepper.value = intervalLabels[intervalIndex];
  stepper.widestValue = intervalWidest;
  stepper.buttonWidth = cols.buttonWidth;
  stepper.valueWidth = cols.valueWidth;
  stepper.gap = cols.gap;
  stepper.decrement = ACTION_DECREMENT;
  stepper.decrementValue = static_cast<int16_t>(slideshow::INTERVAL_PAGE_ROW);
  stepper.increment = ACTION_INCREMENT;
  stepper.incrementValue = static_cast<int16_t>(slideshow::INTERVAL_PAGE_ROW);
  fui::stepperRow(frame, fui::Rect{rowX, cursorY, rowW, modalRowH}, stepper);
  cursorY = static_cast<int16_t>(cursorY + modalRowH + theme.rowGap);

  stepper.row = order;
  stepper.value = orderLabels[orderIndex];
  stepper.widestValue = orderWidest;
  stepper.decrementValue = static_cast<int16_t>(slideshow::ORDER_PAGE_ROW);
  stepper.incrementValue = static_cast<int16_t>(slideshow::ORDER_PAGE_ROW);
  fui::stepperRow(frame, fui::Rect{rowX, cursorY, rowW, modalRowH}, stepper);
}

int BmpViewerActivity::pageSelectableCount() const {
  // Selectable rows of the current page (title headers are not selectable).
  switch (viewerPage) {
    case ViewerPage::Options: {
      // Must mirror buildPageItems: Slideshow is always offered, then Image
      // settings only for tone-capable formats (BMP), then Info / Delete,
      // plus Set sleep cover when allowed.
      int rows = 3;  // Slideshow / Info / Delete
      if (imageSettingsInput::imageSettingsAvailable(!isPng)) rows++;
      if (canSetSleepCover()) rows++;
      return rows;
    }
    case ViewerPage::ImageSettings:
      return static_cast<int>(ToneParam::Count) + 3;  // Reset + 4 value rows + Apply + Use for sleep rendering
    case ViewerPage::Slideshow:
      return slideshow::SLIDESHOW_PAGE_ROWS;  // Start / Interval / Order
    case ViewerPage::ImageInfo:
      // Only Name and Path take focus (pure policy): the metadata rows are
      // informational.
      return imageSettingsInput::IMAGE_INFO_SELECTABLE_ROWS;
    case ViewerPage::InfoDetail:
      return 0;  // informational page: Confirm inert, Up/Down no-ops
    case ViewerPage::DeleteConfirm:
      return 2;  // Cancel / Delete
    default:
      return 0;
  }
}

void BmpViewerActivity::openModalPage(const ViewerPage page) {
  // Closes the touch handshake until the new page's rows are published —
  // touch routing can never dispatch against the previous page's table.
  modalUiReady = false;
  viewerPage = page;
  modalRow = 0;
  repaintModal();  // publishes the new table and re-opens the handshake
}

void BmpViewerActivity::openOptionsMenu() {
  computeModalRect();
  openModalPage(ViewerPage::Options);
}

void BmpViewerActivity::menuAction(ViewerAction action) {
  // Page transitions inside the ONE modal surface — never a nested popup and
  // never an image re-render.
  switch (action) {
    case ViewerAction::Settings:
      // Stage: copy the active settings into the draft; nothing is committed
      // and the image is not re-rendered while stepping.
      draftProfile = toneProfileFromTone(activeTone);
      openModalPage(ViewerPage::ImageSettings);
      break;
    case ViewerAction::Slideshow:
      openModalPage(ViewerPage::Slideshow);
      break;
    case ViewerAction::Info:
      openInfoPage();
      break;
    case ViewerAction::SleepCover:
      viewerPage = ViewerPage::Viewer;  // modal closes; the sleep-cover flow re-renders itself
      doSetSleepCover();
      break;
    case ViewerAction::Delete:
      deleteHeadline = baseNameOf(filePath);
      // Full-width wrapped filename lines (existing measured primitive: ≤2
      // lines, UTF-8-safe split for spaceless names, ellipsis on overflow).
      // Filled once per page entry, reused by repaints. The wrap width is
      // the themed row's text width (insets + side padding included).
      deleteNameLines.clear();
      {
        const auto& metrics = UITheme::getInstance().getMetrics();
        fui::GfxRendererTarget target = makeUiTarget(renderer);
        const modalTheme::ModalListTheme theme =
            modalTheme::resolve(refreshSharedUiThemeTokens(target), target.deviceContext().hasTouch);
        const int rowW = modalRect.width - metrics.popupFrameThickness * 2 - theme.rowInset * 2 - theme.sidePadding * 2;
        deleteNameLines = renderer.wrappedText(uiScaleSpec().bodyFontId, deleteHeadline.c_str(), rowW, 2);
      }
      openModalPage(ViewerPage::DeleteConfirm);
      break;
  }
}

void BmpViewerActivity::openInfoPage() {
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
        // Bitmap.is1Bit() classifies the pixel encoding; BW/gray are user
        // facing descriptions, so they come from the string table.
        snprintf(line, sizeof(line), "%ubpp %s", bitmap.getBpp(),
                 bitmap.is1Bit() ? tr(STR_BIT_DEPTH_BW) : tr(STR_BIT_DEPTH_GRAY));
        infoRows.emplace_back(tr(STR_BIT_DEPTH), line);
      } else {
        infoRows.emplace_back(tr(STR_FORMAT), tr(STR_INVALID_BMP_FILE));
      }
      snprintf(line, sizeof(line), "%u B", static_cast<unsigned>(file.size()));
      infoRows.emplace_back(tr(STR_FILE_SIZE), line);
    } else {
      infoRows.emplace_back(tr(STR_FORMAT), tr(STR_FILE_OPEN_FAILED));
    }
  }

  // Value-column PREVIEWS for the Info page's rows: infoRows keeps the FULL
  // values (the detail views wrap them); the page rows show a truncated copy
  // so nothing overflows the themed row layout. The label column, the text
  // gap and the insets mirror the themed row layout the page renders with.
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int border = metrics.popupFrameThickness;
  fui::GfxRendererTarget target = makeUiTarget(renderer);
  const modalTheme::ModalListTheme theme =
      modalTheme::resolve(refreshSharedUiThemeTokens(target), target.deviceContext().hasTouch);
  const int sidePad = theme.sidePadding;
  const int textGap = 10;
  const int bodyFont = uiScaleSpec().bodyFontId;
  const auto labelFamilyStyle = theme.bodyStyle.bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
  const int labelColW = std::accumulate(infoRows.begin(), infoRows.end(), 0, [&](const int width, const auto& row) {
    return std::max(width, static_cast<int>(renderer.getTextWidth(bodyFont, row.first.c_str(), labelFamilyStyle)));
  });
  const int availW = modalRect.width - border * 2 - theme.rowInset * 2 - sidePad * 2 - labelColW - textGap;
  infoPreviewRows.clear();
  infoPreviewRows.reserve(infoRows.size());
  for (const auto& row : infoRows) {
    std::string value = row.second;
    if (renderer.getTextWidth(bodyFont, value.c_str()) > availW) {
      value = renderer.truncatedText(bodyFont, value.c_str(), availW);
    }
    infoPreviewRows.emplace_back(row.first, std::move(value));
  }

  openModalPage(ViewerPage::ImageInfo);
}

void BmpViewerActivity::openInfoDetail(const int row) {
  // The FULL value of the Name/Path row, wrapped to the themed row width (the
  // delete page's measured primitive — never the Info page's value-column
  // truncation), with as many lines as the modal body reasonably holds at the
  // themed row stride. The modal rect is sized for the largest page, so the
  // detail view inherits that surface.
  if (row < 0 || row >= static_cast<int>(infoRows.size())) return;
  infoDetailLabel = infoRows[row].first;

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int border = metrics.popupFrameThickness;
  fui::GfxRendererTarget target = makeUiTarget(renderer);
  const modalTheme::ModalListTheme theme =
      modalTheme::resolve(refreshSharedUiThemeTokens(target), target.deviceContext().hasTouch);
  const int rowW = modalRect.width - border * 2 - theme.rowInset * 2 - theme.sidePadding * 2;
  const int lineStride = modalRowH + theme.rowGap;
  const int availH = modalRect.height - border * 2 - modalHeaderHeight(target);
  int maxLines = lineStride > 0 ? availH / lineStride : 0;
  if (maxLines > MODAL_MAX_ROWS - 1) maxLines = MODAL_MAX_ROWS - 1;
  if (maxLines < 1) maxLines = 1;
  infoDetailLines = renderer.wrappedText(uiScaleSpec().bodyFontId, infoRows[row].second.c_str(), rowW, maxLines);

  openModalPage(ViewerPage::InfoDetail);
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
    renderCurrentImage(false);
    return;
  }
  if (deletedIndex < 0 || deletedIndex >= oldCount) {
    // Successful delete with an index outside the old sibling list: the file
    // is gone and the stale list can't be trusted — exit to the file browser.
    activityManager.goToFileBrowser(dirPath);
    return;
  }

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

void BmpViewerActivity::handleModalInput() {
  // The modal owns ALL input while viewerPage != Viewer: navigation moves
  // the page row, Confirm activates it, Back leaves the page. No viewer action
  // ever runs underneath.
  //
  // Touch routing first (OptionPopup model): renderModal registered each
  // enabled row (and stepper control) on the interaction buffer and published
  // that table. routePublished reads ONLY the published generation (never a
  // half-built one; modalUiReady gates the window until the current page's
  // first publication), and each routed event maps to exactly ONE semantic
  // action via the host-tested policy: a row body fires the same activateRow()
  // dispatch Confirm uses (modalRow = actionValue); a stepper control fires
  // the shared draft-adjustment path with the control's direction, moving the
  // focus onto the row it adjusts. No double-dispatch: the touch pass
  // returns, never falling through to the button path; taps outside the
  // registered interactions produce no event.
  const freeink::ui::InputSnapshot snap = touchSnapshotFrom(mappedInput);
  if (snap.touchPressed || snap.touchReleased || snap.touchHeld) {
    if (modalUiReady) {
      const freeink::ui::ActionEvent event = modalInteractions.routePublished(snap);
      if (event) {
        switch (imageSettingsInput::settingsTouchActionFor(event.action)) {
          case imageSettingsInput::StepperTouch::ActivateRow:
            if (event.value >= 0 && event.value < pageSelectableCount()) {
              if (viewerPage == ViewerPage::Slideshow) {
                // Touch row-tap policy (host-tested): the Start row starts
                // the slideshow; value-row taps are SELECT-ONLY (focus
                // move) — value changes come only from the explicit -/+
                // steppers, a tap is never a hidden extra increment.
                switch (slideshow::pageRowTapAction(event.value)) {
                  case slideshow::PageRowAction::Start:
                    modalRow = event.value;
                    startViewerSlideshow();
                    return;
                  case slideshow::PageRowAction::SelectOnly:
                    modalRow = event.value;
                    repaintModal();
                    return;
                  default:
                    return;
                }
              }
              modalRow = event.value;
              activateRow();
            }
            return;
          case imageSettingsInput::StepperTouch::StepDown:
            if (viewerPage == ViewerPage::Slideshow) {
              // The slideshow steppers' control values are the value rows.
              if (event.value == slideshow::INTERVAL_PAGE_ROW) {
                modalRow = slideshow::INTERVAL_PAGE_ROW;
                stepSlideshowInterval(-1);
              } else if (event.value == slideshow::ORDER_PAGE_ROW) {
                modalRow = slideshow::ORDER_PAGE_ROW;
                stepSlideshowOrder(-1);
              }
              return;
            }
            if (event.value >= 0 && event.value < static_cast<int>(ToneParam::Count)) {
              modalRow = imageSettingsInput::editableRowForParam(event.value, static_cast<int>(ToneParam::Count));
              adjustDraft(-1);  // staged draft only; no image render until Apply
            }
            return;
          case imageSettingsInput::StepperTouch::StepUp:
            if (viewerPage == ViewerPage::Slideshow) {
              if (event.value == slideshow::INTERVAL_PAGE_ROW) {
                modalRow = slideshow::INTERVAL_PAGE_ROW;
                stepSlideshowInterval(1);
              } else if (event.value == slideshow::ORDER_PAGE_ROW) {
                modalRow = slideshow::ORDER_PAGE_ROW;
                stepSlideshowOrder(1);
              }
              return;
            }
            if (event.value >= 0 && event.value < static_cast<int>(ToneParam::Count)) {
              modalRow = imageSettingsInput::editableRowForParam(event.value, static_cast<int>(ToneParam::Count));
              adjustDraft(1);
            }
            return;
          default:
            return;  // routed but unknown: consumed, never falls through
        }
      }
    }
    if (snap.touchPressed && modalUiReady) {
      // Touch-down on a row moves the visible focus (OptionPopup highlight
      // pattern): the route() call above latched the hit as the active
      // interaction; read it back from the published table, no re-hit-testing.
      const int16_t idx = modalInteractions.activeIndex();
      if (idx >= 0) {
        const freeink::ui::Interaction& hit = modalInteractions.publishedData()[idx];
        if (hit.action == ACTION_ROW && hit.value >= 0 && hit.value < pageSelectableCount() && modalRow != hit.value) {
          modalRow = hit.value;
          repaintModal();
        }
      }
    }
    return;  // any touch pass while the modal is open is consumed by the modal
  }

  // Per-page input policy: Image Settings and Slideshow are 2-AXIS pages
  // (Up/Down choose the row, Left/Right modify the value) and therefore use
  // the explicit logical buttons — NOT the merged NavNext/NavPrevious list
  // navigation, whose Down||Right / Up||Left merge would make one front
  // Left/Right event match both branches. The 1D pages (options / info /
  // delete confirm) keep the standard merged list navigation.
  if (viewerPage == ViewerPage::ImageSettings || viewerPage == ViewerPage::Slideshow) {
    handleSettingsAxesInput();
    return;
  }

  // ONE 1D-page routing order (imageSettingsInput::modalInputActionFor): Back
  // routes on EVERY page — including informational pages with no selectable
  // rows (the Info detail view), whose only exit is Back; Confirm and the row
  // axes stay inert there.
  const auto action =
      imageSettingsInput::modalInputActionFor(
          pageSelectableCount(), mappedInput.wasPressed(MappedInputManager::Button::NavPrevious),
          mappedInput.wasPressed(MappedInputManager::Button::NavNext),
          mappedInput.wasReleased(MappedInputManager::Button::Confirm),
          mappedInput.wasReleased(MappedInputManager::Button::Back));
  const int rows = pageSelectableCount();
  switch (action) {
    case imageSettingsInput::ModalPageAction::Back:
      modalBack();
      return;
    case imageSettingsInput::ModalPageAction::RowUp:
      modalRow = (modalRow - 1 + rows) % rows;
      repaintModal();  // modal-only repaint; the image is never re-rendered
      return;
    case imageSettingsInput::ModalPageAction::RowDown:
      modalRow = (modalRow + 1) % rows;
      repaintModal();
      return;
    case imageSettingsInput::ModalPageAction::Activate:
      activateRow();
      return;
    default:
      return;
  }
}

void BmpViewerActivity::handleSettingsAxesInput() {
  // The 2-axis pages (Image Settings, Slideshow): explicit axes. Side Up/Down
  // = vertical row selection; front Left/Right = horizontal value stepping;
  // Confirm/Back on release. ALL events route through the ONE semantic
  // mapping (imageSettingsInput::actionFor) — dispatch is mutually exclusive
  // (one event -> one branch -> return), so a single event can never produce
  // both a selection move and a value change or a doubled activation.
  const auto dispatch = [&](const MappedInputManager::Button logical, const imageSettingsInput::Button axis) {
    return imageSettingsInput::actionFor(axis, mappedInput.wasPressed(logical), mappedInput.wasReleased(logical));
  };

  switch (dispatch(MappedInputManager::Button::Up, imageSettingsInput::Button::Up)) {
    case imageSettingsInput::Action::RowUp:
      modalRow = (modalRow - 1 + pageSelectableCount()) % pageSelectableCount();
      repaintModal();  // modal-only repaint; the image is never re-rendered
      return;
    default:
      break;
  }
  switch (dispatch(MappedInputManager::Button::Down, imageSettingsInput::Button::Down)) {
    case imageSettingsInput::Action::RowDown:
      modalRow = (modalRow + 1) % pageSelectableCount();
      repaintModal();
      return;
    default:
      break;
  }
  switch (dispatch(MappedInputManager::Button::Left, imageSettingsInput::Button::Left)) {
    case imageSettingsInput::Action::ValueDown:
      adjustDraft(-1);  // staged draft only; no image render until Apply
      return;
    default:
      break;
  }
  switch (dispatch(MappedInputManager::Button::Right, imageSettingsInput::Button::Right)) {
    case imageSettingsInput::Action::ValueUp:
      adjustDraft(1);
      return;
    default:
      break;
  }
  switch (dispatch(MappedInputManager::Button::Confirm, imageSettingsInput::Button::Confirm)) {
    case imageSettingsInput::Action::Activate:
      activateRow();
      return;
    default:
      break;
  }
  switch (dispatch(MappedInputManager::Button::Back, imageSettingsInput::Button::Back)) {
    case imageSettingsInput::Action::Back:
      modalBack();
      return;
    default:
      break;
  }
}

void BmpViewerActivity::activateRow() {
  switch (viewerPage) {
    case ViewerPage::Options:
      if (modalRow >= 0 && modalRow < menuActionCount) menuAction(menuActions[modalRow]);
      break;

    case ViewerPage::ImageSettings: {
      // Editable value rows AND the explicit Apply row share ONE viewer-Apply
      // implementation (Confirm applies without navigating to Apply); Reset
      // only restages the draft; the dedicated sleep row keeps its own action.
      // Routing is the pure, host-tested imageSettingsInput policy — mutually
      // exclusive by return.
      switch (imageSettingsInput::confirmActionForRow(modalRow, static_cast<int>(ToneParam::Count))) {
        case imageSettingsInput::RowAction::ApplyViewer:
          applySettings();
          break;
        case imageSettingsInput::RowAction::ResetDraft:
          resetDraft();
          break;
        case imageSettingsInput::RowAction::SaveSleepProfile:
          saveSleepProfile();
          break;
        default:
          break;
      }
      break;
    }

    case ViewerPage::Slideshow:
      // ONE activation contract (host-tested policy): Confirm on ANY
      // slideshow-menu row starts the slideshow from the open image.
      // Confirm is never a value edit — the Left/Right keys and the touch
      // steppers own Interval/Order changes.
      if (slideshow::pageRowAction(modalRow) == slideshow::PageRowAction::Start) startViewerSlideshow();
      break;

    case ViewerPage::ImageInfo:
      // Name/Path detail (pure policy): Confirm opens the focused row's full
      // value; the metadata rows never take focus, so they activate nothing.
      // There is deliberately no back/Done action on this page.
      switch (imageSettingsInput::infoDetailForRow(modalRow)) {
        case imageSettingsInput::InfoDetail::Name:
          openInfoDetail(0);
          break;
        case imageSettingsInput::InfoDetail::Path:
          openInfoDetail(1);
          break;
        default:
          break;
      }
      break;

    case ViewerPage::InfoDetail:
      break;  // informational page: Confirm does nothing

    case ViewerPage::DeleteConfirm:
      // Production-owned routing policy (host-tested): action 0 = Cancel,
      // action 1 = Delete, anything else = nothing (the destructive action is
      // never reachable through a visual-row fallback).
      switch (imageSettingsInput::deleteConfirmActionForRow(modalRow)) {
        case imageSettingsInput::DeleteAction::Cancel:
          // Cancel: back to the root options page.
          openModalPage(ViewerPage::Options);
          break;
        case imageSettingsInput::DeleteAction::Delete:
          // Delete: a failed remove aborts before any navigation state change.
          viewerPage = ViewerPage::Viewer;
          performDelete();
          break;
        default:
          break;
      }
      break;
  }
}

void BmpViewerActivity::modalBack() {
  switch (viewerPage) {
    case ViewerPage::Options:
      // Root page Back closes the modal; the image re-renders according to
      // its format (grayscale pipeline for BMP, PNG path for PNG) to restore
      // the panel content under it.
      viewerPage = ViewerPage::Viewer;
      renderCurrentImage(false);
      break;
    case ViewerPage::ImageSettings:
      // Discard the staged draft (activeTone untouched) and return to the
      // root options page — modal-only repaint, no image re-render.
      openModalPage(ViewerPage::Options);
      break;
    case ViewerPage::Slideshow:
      // The interval persists per step; Back just returns to the root page.
      openModalPage(ViewerPage::Options);
      break;
    case ViewerPage::InfoDetail:
      // Back returns to the Image Info page the detail was opened from.
      openModalPage(ViewerPage::ImageInfo);
      break;
    case ViewerPage::ImageInfo:
    case ViewerPage::DeleteConfirm:
      openModalPage(ViewerPage::Options);
      break;
    default:
      break;
  }
}

void BmpViewerActivity::adjustDraft(int delta) {
  // The value axis of the 2-axis pages: the Slideshow page steps the shared
  // persisted interval (only its Interval row is editable), the Image
  // Settings page steps the STAGED draft (no buildToneLut(), no image render
  // until Apply). The quantizer row steps directionally.
  // Explicit row->param mapping (imageSettingsInput policy): the Reset / Apply
  // / sleep action rows step nothing.
  if (viewerPage == ViewerPage::Slideshow) {
    if (modalRow == slideshow::INTERVAL_PAGE_ROW)
      stepSlideshowInterval(delta);
    else if (modalRow == slideshow::ORDER_PAGE_ROW)
      stepSlideshowOrder(delta);
    return;
  }
  if (viewerPage != ViewerPage::ImageSettings) return;
  const int param = imageSettingsInput::editableParamForRow(modalRow, static_cast<int>(ToneParam::Count));
  switch (param) {
    case static_cast<int>(ToneParam::Brightness):
      draftProfile.brightnessPct =
          imageSettingsInput::stepClamped(draftProfile.brightnessPct, delta, TONE_BRIGHTNESS_MIN, TONE_BRIGHTNESS_MAX);
      break;
    case static_cast<int>(ToneParam::Gamma):
      draftProfile.gammaPct =
          imageSettingsInput::stepClamped(draftProfile.gammaPct, delta, TONE_GAMMA_MIN, TONE_GAMMA_MAX);
      break;
    case static_cast<int>(ToneParam::Contrast):
      draftProfile.contrastPct =
          imageSettingsInput::stepClamped(draftProfile.contrastPct, delta, TONE_CONTRAST_MIN, TONE_CONTRAST_MAX);
      break;
    case static_cast<int>(ToneParam::Quantizer):
      // Directional cycle shared by the hardware Left/Right keys and the touch
      // stepper -/+ (imageSettingsInput policy): Default -> Canonical ->
      // Legacy -> Default forward, the reverse backward.
      draftProfile.quantizer = imageSettingsInput::quantizerStepped(draftProfile.quantizer, delta);
      break;
    default:
      return;  // action rows: Left/Right is a no-op
  }
  repaintModal();
}

bool BmpViewerActivity::commitDraftProfile(ToneProfile& profile) {
  // ONE transactional persist implementation for both profile saves. The
  // settings write is the commit boundary: the slot takes the normalized draft
  // candidate, and a failed save restores the previous value — the store never
  // ends up with a profile the disk does not confirm (no false success).
  ToneProfile candidate = draftProfile;
  normalizeToneProfile(candidate);
  const ToneProfile previous = profile;
  profile = candidate;
  if (!SETTINGS.saveToFile()) {
    profile = previous;
    return false;
  }
  return true;
}

void BmpViewerActivity::applySettings() {
  // The ONLY viewer-Apply implementation: commit the draft once, close the
  // ENTIRE modal UI, and render the current image exactly once — no return to
  // the root menu first. Reachable from every editable row and the Apply row.
  //
  // Persistence is the commit boundary (transactional): the staged draft goes
  // to SETTINGS.viewerRenderProfile and only a successful saveToFile() lets it
  // become the activeTone. A failed save restores the previous profile —
  // RAM never ends up "active but unsaved" — shows the failure indication,
  // stays on the settings page with the draft intact, changes no activeTone
  // and re-renders no image. sleepRenderProfile is never touched here (and
  // "Use for sleep rendering" never touches this profile).
  if (!commitDraftProfile(SETTINGS.viewerRenderProfile)) {
    LOG_ERR("BMP", "Failed to save viewer render profile");
    const Rect anchor = modalAnchorRect();
    GUI.drawPopup(renderer, tr(STR_FAILED_LOWER), &anchor);
    delay(1000);
    repaintModal();
    return;
  }
  activeTone = toneLutFromProfile(SETTINGS.viewerRenderProfile);
  buildToneLut(activeTone);
  viewerPage = ViewerPage::Viewer;
  renderBmp(false);
}

void BmpViewerActivity::resetDraft() {
  // "Reset to defaults": restage ONLY the draft (B100 G1.00 C100, quantizer =
  // no override) and stay on the settings page. Nothing is applied, persisted
  // or rendered yet — the user can inspect/modify the reset draft before
  // Apply; Back discards it without touching activeTone or either profile.
  draftProfile = ToneProfile{};
  repaintModal();
}

void BmpViewerActivity::saveSleepProfile() {
  // "Use for sleep rendering": persist the STAGED draft (not activeTone — the
  // two may differ) as the sleep render profile. Transactional (see
  // commitDraftProfile): a failed SD write restores the previous profile (no
  // unsaved-but-active state), shows the failure indication (never a false
  // Done), and stays on the settings page with the draft intact for a retry.
  // No image render, no activeTone change, no viewerRenderProfile change, no
  // sleep-cover file/mode touch.
  //
  // The confirmation anchors INSIDE the modal surface (drawPopup's anchor):
  // a screen-top popup sits outside the modal rect and the modal's partial
  // repaints never clear it — the hardware "Done" ghost that outlived the
  // settings page and every later modal navigation.
  if (!commitDraftProfile(SETTINGS.sleepRenderProfile)) {
    LOG_ERR("BMP", "Failed to save sleep render profile");
    const Rect anchor = modalAnchorRect();
    GUI.drawPopup(renderer, tr(STR_FAILED_LOWER), &anchor);
    delay(1000);
    repaintModal();
    return;
  }
  const Rect anchor = modalAnchorRect();
  GUI.drawPopup(renderer, tr(STR_DONE), &anchor);
  delay(1000);
  openModalPage(ViewerPage::Options);
}

void BmpViewerActivity::renderCurrentImage(const bool showLoadingPopup) {
  if (isPng) {
    // PNG decodes through the shared PNG converter path and never enters
    // renderBmp().
    const auto pageHeight = renderer.getScreenHeight();
    if (showLoadingPopup) {
      const Rect popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
      GUI.fillPopupProgress(renderer, popupRect, 20);  // Initial 20% progress
    }
    renderer.clearScreen();
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OPTIONS), "", "");
    if (imageonly::renderPngToFramebuffer(renderer, filePath)) {
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    } else {
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_FILE_OPEN_FAILED));
      GUI.drawButtonHints(renderer, labels.btn1, "", "", "");
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }
    return;
  }
  renderBmp(showLoadingPopup);
}

void BmpViewerActivity::onEnter() {
  Activity::onEnter();

  if (siblingImages.empty() && !filePath.empty()) {
    loadSiblingImages();
  }

  if (slideshowResume) {
    // Managed timer-wake resume: the retained frame's directory holds the
    // continuation; nothing usable means the slideshow ends, fail closed.
    if (siblingImages.empty()) {
      endViewerSlideshowToHome();
      return;
    }
    advanceSlideshowFrame();
    return;
  }

  isPng = FsHelpers::hasPngExtension(filePath);

  renderCurrentImage(true);
}

void BmpViewerActivity::advanceSlideshowFrame() {
  // Advance under the persisted ORDER policy (Forward/Reverse/Random). A
  // retained image missing from the scan hands the policy its restart
  // semantics; Random advances its randomized exhaustive cycle (RTC-only).
  // Nothing usable means the slideshow ends, fail closed.
  const slideshow::Order order = slideshow::orderClamped(SETTINGS.slideshowOrder);
  slideshow::RandomCycleState cycleOut;
  const std::string next = imageonly::nextImageAfter(filePath, order, slideshow::getRetainedCycle(), esp_random(),
                                                     cycleOut);
  if (next.empty()) {
    endViewerSlideshowToHome();
    return;
  }
  filePath = next;
  isPng = FsHelpers::hasPngExtension(filePath);

  // Re-arm the retained state with the frame being rendered BEFORE sleeping
  // again, so the next timer wake continues from here.
  if (!slideshow::arm(filePath, slideshow::Mode::Viewer)) {
    LOG_ERR("BMP", "Slideshow re-arm failed");
    endViewerSlideshowToHome();
    return;
  }

  // An undecodable frame must not timer-loop on a blank panel: fail closed.
  if (!renderImageOnlyFrame()) {
    LOG_ERR("BMP", "Slideshow frame failed to render");
    endViewerSlideshowToHome();
    return;
  }
  // RTC-only cycle metadata update (no SD write); the re-arm above preserves it.
  slideshow::setRetainedCycle(cycleOut);
  slideshow::requestSleep(slideshow::SleepRequest::Continue);
}

void BmpViewerActivity::endViewerSlideshowToHome() {
  // Fail closed: the slideshow cannot continue — cancel it and route Home.
  // Display was initialized seamless and still holds the last slideshow
  // frame — clean refresh so Home's first paint replaces it.
  slideshow::clearRetainedState();
  activityManager.goHome(HomeMenuItem::NONE, /*cleanInitialRefresh=*/true);
}

void BmpViewerActivity::stepSlideshowInterval(const int delta) {
  // Slideshow page Interval row: one step persists the shared cadence
  // immediately (the same per-selection persistence the Settings UI uses) and
  // repaints the modal only — the image is never re-rendered.
  const uint8_t stepped = slideshow::intervalIndexStepped(SETTINGS.slideshowInterval, delta);
  SETTINGS.slideshowInterval = stepped;
  SETTINGS.saveToFile();
  repaintModal();
}

void BmpViewerActivity::stepSlideshowOrder(const int delta) {
  // Slideshow page Order row: one step persists the shared frame order
  // immediately and repaints the modal only — the image is never re-rendered.
  const uint8_t stepped = slideshow::orderIndexStepped(SETTINGS.slideshowOrder, delta);
  SETTINGS.slideshowOrder = stepped;
  SETTINGS.saveToFile();
  repaintModal();
}

void BmpViewerActivity::startViewerSlideshow() {
  // Slideshow page Start row: arm the CURRENT image with Viewer mode, repaint
  // it image-only and hand the frame sleep to the main loop (which performs
  // the one-time Start persistence). The open image is the first frame of a
  // fresh randomized cycle — reset the retained cycle metadata so the first
  // timer continuation starts a new cycle from here (RTC-only, no SD write).
  if (!slideshow::arm(filePath, slideshow::Mode::Viewer)) {
    LOG_ERR("BMP", "Slideshow arm rejected");
    const Rect anchor = modalAnchorRect();
    GUI.drawPopup(renderer, tr(STR_FAILED_LOWER), &anchor);
    delay(1000);
    repaintModal();
    return;
  }
  slideshow::setRetainedCycle(slideshow::RandomCycleState{});
  // An undecodable first frame must not timer-loop on a blank panel: cancel
  // and restore the normal viewer.
  if (!renderImageOnlyFrame()) {
    LOG_ERR("BMP", "Slideshow frame failed to render");
    slideshow::clearRetainedState();
    viewerPage = ViewerPage::Viewer;
    GUI.drawPopup(renderer, tr(STR_FAILED_LOWER));
    delay(1000);
    renderCurrentImage(true);
    return;
  }
  viewerPage = ViewerPage::Viewer;
  slideshow::requestSleep(slideshow::SleepRequest::Start);
}

void BmpViewerActivity::renderBmp(const bool showPopup) {
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

  // Always attach the active tone: quantizer selection is active even when
  // the tone LUT itself is at identity (enabled=false).
  Bitmap bitmap(file, true,
                renderer.grayscaleCapabilities(HalDisplay::GrayscaleMode::Absolute).supported() &&
                    display.getController() == HalDisplay::Controller::SSD1677,
                &activeTone);

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
  imageonly::fitOnScreen(bitmap.getWidth(), bitmap.getHeight(), pageWidth, pageHeight, &x, &y);

  // 4. Prepare Rendering
  bool hasPrevious = (siblingImages.size() > 1 && currentImageIndex > 0);
  bool hasNext = (siblingImages.size() > 1 && currentImageIndex != -1 &&
                  currentImageIndex < static_cast<int>(siblingImages.size()) - 1);

  // Confirm opens the Options modal; its hint reads as an active viewer action.
  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK), tr(STR_OPTIONS), (hasPrevious ? "<" : ""), (hasNext ? ">" : ""));

  if (showPopup) GUI.fillPopupProgress(renderer, popupRect, 50);

  renderer.clearScreen();
  if (!renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, 0, 0)) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_FILE_OPEN_FAILED));
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    return;
  }

  // Draw UI hints on the base layer (viewer chrome).
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
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.cleanupGrayscaleWithFrameBuffer();
    if (!planesReady) renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  } else {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  }
}

bool BmpViewerActivity::renderImageOnlyFrame() {
  // Slideshow frames render through the shared slideshow seam with the
  // SLEEP-IMAGE presentation policy (imageonly::renderImageFile): the current
  // file, the session's active tone, no chrome, no popup, no error text, no
  // BW framebuffer rebuild.
  return imageonly::renderImageFile(renderer, filePath, activeTone);
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
      // Copy beside the target and swap in only a complete image, so a failed
      // copy keeps the previous sleep cover instead of a truncated one.
      const std::string tmp = std::string(destination) + ".tmp";
      HalFile inFile, outFile;
      if (Storage.openFileForRead("BMP", filePath, inFile) && Storage.openFileForWrite("BMP", tmp, outFile)) {
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
        success = success && Storage.replaceFile(tmp.c_str(), destination);
        if (!success) Storage.remove(tmp.c_str());
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

  // ONE modal owner: while viewerPage != Viewer the modal surface owns ALL
  // input and loop() always returns after its handler — no sibling navigation,
  // no viewer buttons, no image repaint underneath.
  if (viewerPage != ViewerPage::Viewer) {
    handleModalInput();
    return;
  }

  // Viewer mode — native Image Viewer controls plus the Options modal trigger:
  //   Up (side) / Left (front) -> previous image
  //   Down (side) / Right (front) -> next image
  //   Confirm (GPIO8) -> Options modal
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
    openOptionsMenu();
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
