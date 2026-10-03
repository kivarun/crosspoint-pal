#pragma once

#include <FreeInkUI.h>
#include <ToneLut.h>

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "GfxRenderer.h"
#include "MappedInputManager.h"
#include "activities/Activity.h"
#include "components/ModalTheme.h"
#include "util/ImageSettingsInput.h"

// Image viewer: full-screen BMP/PNG viewing with an Options modal for render
// settings, image info and file management. Tone state starts each session
// from the persisted SETTINGS.viewerRenderProfile; staged edits live in
// draftProfile until Apply.
//
// Modal model: ONE modal surface over the image. viewerPage selects which PAGE
// that surface shows (options / settings / slideshow / info / delete confirm);
// page transitions replace the page contents inside the same panel rect — a
// child popup is never drawn on top of the parent menu. activeTone holds the
// ACTIVE session-global tone settings (consumed by the decoder); draftProfile
// holds staged values while the settings page is open (committed only by
// Apply).
enum class ToneParam : uint8_t { Brightness = 0, Gamma = 1, Contrast = 2, Quantizer = 3, Count = 4 };

struct Rect;  // the app rect (components/themes/BaseTheme.h)

enum class ViewerPage : uint8_t { Viewer, Options, ImageSettings, ImageInfo, InfoDetail, DeleteConfirm, Slideshow };

// Actions of the Options page rows (order built at page build).
enum class ViewerAction : uint8_t { Settings = 0, Info, SleepCover, Delete, Slideshow };

class BmpViewerActivity final : public Activity {
 public:
  // slideshowResume: managed Viewer-mode slideshow continuation (Timer wake;
  // only constructed for a Viewer-mode retained state): onEnter advances to
  // the next image with wrap, re-arms the retained state, renders it through
  // the canonical path and requests the frame sleep. Normal viewer behavior
  // is unchanged when false.
  BmpViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string filePath,
                    bool slideshowResume = false);

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  static constexpr int MODAL_MAX_ROWS = 8;  // title header + largest page
  // Fullest page is Image Settings: each of the 4 stepper rows (one per
  // ToneParam) registers its row body plus the - and + controls, and the
  // Reset / Apply / sleep rows register one body hit each — 4*3 + 3 = 15
  // interactions; headers register none and there is no chrome guard.
  static constexpr int MODAL_MAX_INTERACTIONS = static_cast<int>(ToneParam::Count) * 3 + 3;
  static constexpr int MODAL_INTERACTION_CAPACITY = MODAL_MAX_INTERACTIONS;
  // Height of the pages' title header, exactly as fui::list draws it (raw
  // defaults: header text line + 2px gap + 1px underline).
  static int16_t modalHeaderHeight(const freeink::ui::DrawTarget& target);
  // Touch action space shared with the host-tested imageSettingsInput policy.
  static constexpr freeink::ui::ActionId ACTION_ROW = imageSettingsInput::kActionRow;
  static constexpr freeink::ui::ActionId ACTION_DECREMENT = imageSettingsInput::kActionDecrement;
  static constexpr freeink::ui::ActionId ACTION_INCREMENT = imageSettingsInput::kActionIncrement;

  void loadSiblingImages();
  void doSetSleepCover();
  bool canSetSleepCover() const;
  // Canonical format-aware render entry for the CURRENT image (viewer chrome
  // path): PNG goes through the PNG converter/presentation path and never
  // enters renderBmp(); BMP renders through the grayscale pipeline.
  // showLoadingPopup gates the decode loading popup for both pipelines.
  void renderCurrentImage(bool showLoadingPopup);
  void renderBmp(bool showPopup);
  // Image-only render of the current file through the shared imageonly seam
  // with the sleep-image presentation policy (no chrome, no popup, no BW
  // framebuffer rebuild): the slideshow frames' canonical path.
  bool renderImageOnlyFrame();
  void openOptionsMenu();
  void computeModalRect();
  // The modal rect as BaseTheme::drawPopup's anchor type (the app Rect) —
  // popups drawn over the modal surface anchor inside it, so the modal's own
  // partial repaints clear them again.
  Rect modalAnchorRect() const;
  void renderModal();
  void repaintModal();
  // Page transition inside the modal surface: closes the touch handshake
  // (OptionPopup's uiReady model) so touch routing can never dispatch against
  // the previous page's interaction table.
  void openModalPage(ViewerPage page);
  int buildPageItems();
  // Image Settings page: manual layout from SDK primitives (title header in
  // the list cadence, settingRow for the action rows, stepperRow for the four
  // editable ToneParam rows); every component registers its own interactions.
  // The page renders with the resolved modal theme (rows, header, stride).
  void buildSettingsPage(freeink::ui::Frame<MODAL_INTERACTION_CAPACITY>& frame, const freeink::ui::Rect& body,
                         const modalTheme::ModalListTheme& theme);
  // Slideshow page: the same architecture (title header + settingRow Start +
  // steppers for the shared persisted interval/order); no draft staging, the
  // steps persist immediately and the image is never re-rendered.
  void buildSlideshowPage(freeink::ui::Frame<MODAL_INTERACTION_CAPACITY>& frame, const freeink::ui::Rect& body,
                          const modalTheme::ModalListTheme& theme);
  // Pixel extents of the Slideshow page's label/value columns measured from
  // the localized strings in the themed label style — feeds the modal width
  // sizing on touch targets.
  void measureSlideshowExtents(const freeink::ui::DrawTarget& target, const modalTheme::ModalListTheme& theme,
                               int16_t& maxLabelWidth, int16_t& maxValueWidth) const;
  // Pixel extents of the settings page's label/value columns measured from
  // the localized strings (widest row label, widest possible value) — the
  // single measurement owner for the fixed stepper columns and the modal
  // width sizing.
  void measureStepperExtents(const freeink::ui::DrawTarget& target, const modalTheme::ModalListTheme& theme,
                             int16_t& maxLabelWidth, int16_t& maxValueWidth) const;
  int pageSelectableCount() const;
  void handleModalInput();
  void handleSettingsAxesInput();
  void activateRow();
  void modalBack();
  void adjustDraft(int delta);
  void applySettings();
  void resetDraft();
  void saveSleepProfile();
  // Transactional persist of the staged draft into one profile slot (viewer
  // or sleep): restores the slot and returns false when the settings write
  // fails.
  bool commitDraftProfile(ToneProfile& profile);
  void menuAction(ViewerAction action);
  void openInfoPage();
  // Image Info detail view: the FULL Name/Path value, wrapped to the modal's
  // body width (the same measured primitive the delete page uses); no
  // selectable rows, Confirm inert, Back returns to Image Info.
  void openInfoDetail(int row);
  void performDelete();
  // Slideshow page actions: the interval/order steps persist the shared
  // setting (modal-only repaint, the image is never re-rendered); the start
  // arms the CURRENT image with slideshow::Mode::Viewer, repaints it
  // image-only and requests the Viewer Start sleep.
  void stepSlideshowInterval(int delta);
  void stepSlideshowOrder(int delta);
  void startViewerSlideshow();
  // Fail-closed exit from a broken viewer slideshow: cancel the retained
  // state and route Home (clean refresh replaces the last frame).
  void endViewerSlideshowToHome();
  // Slideshow continuation: wrap-advance to the next image relative to
  // filePath, re-arm the retained state with the new path, render it through
  // the canonical path and request the frame sleep. Routes Home (and clears
  // the slideshow state) when there is nothing left to show or re-arming
  // fails.
  void advanceSlideshowFrame();

  std::string filePath;
  std::vector<std::string> siblingImages;
  int currentImageIndex = -1;
  bool isPng = false;
  // Slideshow-continuation mode (managed Timer-wake resume); never set on a
  // normally opened viewer.
  bool slideshowResume = false;

  // ONE modal cadence, resolved from the DeviceContext (modalRowHeight
  // policy): every modal page's visual row height AND touch minimum AND the
  // panel sizing use this single value — visual row height == touch minimum,
  // so a row's hit band can never bleed into its neighbor's. The theme's row
  // gap rides on top of every stride (resolved once per modal open, the same
  // value the sizing math and every page builder lay rows out with).
  int16_t modalRowH = imageSettingsInput::MODAL_MIN_ROW_H;
  int16_t modalRowGap = 0;

  // Tone state (RAM-only, survives image navigation within the session):
  // activeTone is what the decoder consumes, draftProfile stages edits while
  // the settings page is open.
  ViewerPage viewerPage = ViewerPage::Viewer;
  int modalRow = 0;                                           // selected row of the current page
  ToneLut activeTone{};                                       // ACTIVE session-wide settings (consumed by the decoder)
  ToneProfile draftProfile{};                                 // staged values while the settings page is open
  std::vector<std::pair<std::string, std::string>> infoRows;  // filled by openInfoPage() with FULL values
  // Image Info display copies: the value-column previews for the Info page's
  // rows (truncated once at page entry), and the detail view's content
  // (label + wrapped full-width lines, filled at detail entry).
  std::vector<std::pair<std::string, std::string>> infoPreviewRows;
  std::string infoDetailLabel;
  std::vector<std::string> infoDetailLines;

  // Options page row actions (dynamic: Set sleep cover appears only when
  // canSetSleepCover()).
  ViewerAction menuActions[MODAL_MAX_ROWS] = {};
  int menuActionCount = 0;

  // ONE modal surface: fixed panel rect for the whole modal session + page rows
  freeink::ui::Rect modalRect{};
  freeink::ui::ListItem modalItems[MODAL_MAX_ROWS] = {};
  char valueScratch[4][24] = {};             // settings page value strings
  std::string deleteHeadline;                // delete page filename scratch
  std::vector<std::string> deleteNameLines;  // delete page: wrapped full-width filename lines (filled at page entry)
  // Touch hit-testing (OptionPopup model): renderModal registers the rows on
  // the render pass and publishes the table when done; handleModalInput routes
  // touch snapshots against the published generation. modalUiReady stays open
  // across same-page repaints and closes on page transitions (openModalPage),
  // so a release is never routed against the previous page's table.
  freeink::ui::InteractionBuffer<MODAL_INTERACTION_CAPACITY> modalInteractions;
  std::atomic<bool> modalUiReady{false};
};
