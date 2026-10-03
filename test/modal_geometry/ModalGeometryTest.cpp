#include <gtest/gtest.h>

#include <cstring>

#include "GeometryTarget.h"
#include "src/components/ListRowPresentation.h"
#include "src/components/ModalTheme.h"
#include "src/components/themes/BaseTheme.h"
#include "src/util/ImageSettingsInput.h"

namespace fui = freeink::ui;

namespace {

constexpr int16_t BODY_X = 16;
constexpr int16_t BODY_W = 368;
constexpr int16_t BODY_Y = 100;
constexpr fui::ActionId ACTION_ROW = imageSettingsInput::kActionRow;

// Production row cadence for a touch target, from the DeviceContext
// primitives a touch device carries (minTouchSize 44 = the SDK default).
constexpr int TOUCH_ROW_H = imageSettingsInput::modalRowHeight(true, 44);
constexpr int16_t ROW_H = static_cast<int16_t>(TOUCH_ROW_H);

// The production DeleteConfirm page fragment around the hazard: two adjacent
// action rows (Cancel / Delete), laid out exactly the way the modal does —
// contiguous bands, list-registered hits, newest-first routing.
void buildDeleteConfirmRows(fui::Frame<16>& frame, const int16_t y) {
  fui::ListItem items[2]{};
  items[0].label = "Cancel";
  items[0].actionValue = 0;
  items[1].label = "Delete";
  items[1].actionValue = 1;

  fui::ListProps props{};
  props.items = items;
  props.count = 2;
  props.scrollIndicator = false;
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.rowHeight = ROW_H;
  fui::list(frame, fui::Rect{BODY_X, y, BODY_W, static_cast<int16_t>(2 * ROW_H)}, props);
}

struct Routed {
  bool routed = false;
  int action = 0;
  int value = 0;
};

// Rebuilds the given page fragment into a fresh frame (the production
// protocol: beginPublishCycle -> build -> publish) and routes one tap.
template <typename PageBuilder>
Routed routeTap(fui::InteractionBuffer<16>& interactions, const freeink::ui::DeviceContext& device, PageBuilder build,
                const int16_t buildY, const int16_t x, const int16_t y) {
  GeometryTarget target;
  const fui::InputSnapshot noInput{};
  interactions.beginPublishCycle();
  fui::Frame<16> frame(target, device, noInput, interactions);
  build(frame, buildY);
  interactions.publish();

  const auto event = interactions.routePublished(tapAt(x, y));
  return {static_cast<bool>(event), event.action, event.value};
}

}  // namespace

// DeleteConfirm destructive invariant, proven against the REAL registration
// and routing (fui::list -> ensureMinTouchRect -> InteractionBuffer):
// every pixel visibly inside the Cancel band routes to Cancel, every pixel
// inside the Delete band routes to Delete, and the bands never overlap — so
// the centered min-touch expansion can never hand a Cancel tap to Delete or
// the reverse (rowHeight == minTouchSize keeps the bands closed).
TEST(ModalGeometry, DeleteConfirmTouchBandsAreDisjointAndRoutedExactly) {
  const auto device = geometryDevice(true, 44);
  fui::InteractionBuffer<16> interactions;
  GeometryTarget target;
  const fui::InputSnapshot noInput{};
  interactions.beginPublishCycle();
  fui::Frame<16> frame(target, device, noInput, interactions);
  buildDeleteConfirmRows(frame, BODY_Y);
  interactions.publish();

  // Exactly the two action rows registered (the SDK's list registers one hit
  // per enabled row; both rows stay in their own band).
  ASSERT_EQ(interactions.publishedCount(), 2u);
  const fui::Interaction* hits = interactions.publishedData();
  const fui::Rect cancelBand = hits[0].rect;
  const fui::Rect deleteBand = hits[1].rect;
  ASSERT_EQ(hits[0].value, 0);
  ASSERT_EQ(hits[1].value, 1);
  // Disjoint by construction: no vertical bleed across the shared edge.
  EXPECT_EQ(cancelBand.bottom(), deleteBand.y);

  // The boundary pixels of each band route to their own row — never across.
  for (int16_t y = cancelBand.y; y < cancelBand.bottom(); ++y) {
    const Routed r = routeTap(interactions, device, buildDeleteConfirmRows, BODY_Y, BODY_X + 10, y);
    ASSERT_TRUE(r.routed) << "y=" << y;
    EXPECT_EQ(r.action, ACTION_ROW) << "y=" << y;
    EXPECT_EQ(r.value, 0) << "y=" << y;  // always Cancel, never Delete
  }
  for (int16_t y = deleteBand.y; y < deleteBand.bottom(); ++y) {
    const Routed r = routeTap(interactions, device, buildDeleteConfirmRows, BODY_Y, BODY_X + 10, y);
    ASSERT_TRUE(r.routed) << "y=" << y;
    EXPECT_EQ(r.action, ACTION_ROW) << "y=" << y;
    EXPECT_EQ(r.value, 1) << "y=" << y;  // always Delete, never Cancel
  }
}

// The stepper fragment of the Image Settings page: one editable row
// (Quantizer, ToneParam index 3) then the Apply row — the hazardous
// adjacency, rendered exactly the way buildSettingsPage() does.
void buildStepperFragment(fui::Frame<16>& frame, const int16_t y) {
  fui::StepperRowProps stepper{};
  stepper.row.label = "Quantizer";
  stepper.row.action = ACTION_ROW;
  stepper.row.valueId = 4;
  stepper.row.inputMask = fui::InputTouch;
  stepper.row.minTouchSize = ROW_H;
  stepper.value = "Default";
  stepper.widestValue = "Canonical";
  stepper.decrement = imageSettingsInput::kActionDecrement;
  stepper.decrementValue = 3;
  stepper.increment = imageSettingsInput::kActionIncrement;
  stepper.incrementValue = 3;
  fui::stepperRow(frame, fui::Rect{BODY_X, y, BODY_W, ROW_H}, stepper);

  fui::SettingRowProps apply{};
  apply.label = "Apply";
  apply.action = ACTION_ROW;
  apply.valueId = 5;
  apply.inputMask = fui::InputTouch;
  apply.minTouchSize = ROW_H;
  fui::settingRow(frame, fui::Rect{BODY_X, static_cast<int16_t>(y + ROW_H), BODY_W, ROW_H}, apply);
}

TEST(ModalGeometry, StepperControlsStayInRowAndRouteToAdjustment) {
  const auto device = geometryDevice(true, 44);
  fui::InteractionBuffer<16> interactions;
  GeometryTarget target;
  const fui::InputSnapshot noInput{};
  interactions.beginPublishCycle();
  fui::Frame<16> frame(target, device, noInput, interactions);
  buildStepperFragment(frame, BODY_Y);
  interactions.publish();

  ASSERT_EQ(interactions.publishedCount(), 4u);
  const fui::Interaction* hits = interactions.publishedData();
  // Registration order: row body, minus, plus, apply.
  const fui::Rect bodyBand = hits[0].rect;
  const fui::Rect minusBand = hits[1].rect;
  const fui::Rect plusBand = hits[2].rect;
  const fui::Rect applyBand = hits[3].rect;
  ASSERT_EQ(hits[0].value, 4);
  ASSERT_EQ(hits[1].action, imageSettingsInput::kActionDecrement);
  ASSERT_EQ(hits[2].action, imageSettingsInput::kActionIncrement);
  ASSERT_EQ(hits[3].value, 5);

  // The +/- control bands stay inside their own row and out of the neighbor
  // (with minTouchSize == row height the stepper's hitPadding fills the band
  // exactly, so ensureMinTouchRect() has nothing left to expand).
  EXPECT_GE(minusBand.y, BODY_Y);
  EXPECT_LE(minusBand.bottom(), BODY_Y + ROW_H);
  EXPECT_GE(plusBand.y, BODY_Y);
  EXPECT_LE(plusBand.bottom(), BODY_Y + ROW_H);
  EXPECT_GE(applyBand.y, BODY_Y + ROW_H);
  EXPECT_EQ(bodyBand.bottom(), applyBand.y);

  // A tap on a control routes to the directional adjustment — never to the
  // row body's Apply route.
  const Routed minus = routeTap(interactions, device, buildStepperFragment, BODY_Y,
                                static_cast<int16_t>(minusBand.x + minusBand.width / 2),
                                static_cast<int16_t>(minusBand.y + minusBand.height / 2));
  EXPECT_EQ(minus.action, imageSettingsInput::kActionDecrement);
  EXPECT_EQ(minus.value, 3);
  const Routed plus = routeTap(interactions, device, buildStepperFragment, BODY_Y,
                               static_cast<int16_t>(plusBand.x + plusBand.width / 2),
                               static_cast<int16_t>(plusBand.y + plusBand.height / 2));
  EXPECT_EQ(plus.action, imageSettingsInput::kActionIncrement);
  EXPECT_EQ(plus.value, 3);

  // The label side of the row keeps Confirm parity (Apply route).
  const Routed body = routeTap(interactions, device, buildStepperFragment, BODY_Y,
                               static_cast<int16_t>(bodyBand.x + 10), static_cast<int16_t>(BODY_Y + ROW_H / 2));
  EXPECT_EQ(body.action, ACTION_ROW);
  EXPECT_EQ(body.value, 4);

  // The Apply row's band routes to Apply, never to the stepper above it.
  const Routed applyTap =
      routeTap(interactions, device, buildStepperFragment, BODY_Y, BODY_X + 10, static_cast<int16_t>(applyBand.y + 2));
  EXPECT_EQ(applyTap.action, ACTION_ROW);
  EXPECT_EQ(applyTap.value, 5);
}

// Button-only targets resolve the 36px density: the DeviceContext's touch
// minimum is irrelevant when hasTouch is false (the hits are never routed).
TEST(ModalGeometry, RowCadenceResolvesFromDeviceContext) {
  using imageSettingsInput::modalRowHeight;
  EXPECT_EQ(modalRowHeight(true, 0), 36);
  EXPECT_EQ(modalRowHeight(true, 24), 36);
  EXPECT_EQ(modalRowHeight(true, 36), 36);
  EXPECT_EQ(modalRowHeight(true, 44), 44);
  EXPECT_EQ(modalRowHeight(true, 48), 48);
  EXPECT_EQ(modalRowHeight(false, 44), 36);
  EXPECT_EQ(modalRowHeight(false, 24), 36);
}

// ---- Modal theme resolution (modalTheme::resolve/applyListProps): the
// modal follows the ACTIVE theme's tokens the same way FreeInkApp's
// resolveListProps themes app lists. Pure resolver proofs — no pixel
// snapshots; the values below are the themes' ThemeMetrics data
// (uiThemeTokens() copies into the tokens verbatim). ----

// Builds the ThemeTokens the firmware's uiThemeTokens() derives for a theme
// shape (themeTokensForLineHeight default + the theme's list metrics).
fui::ThemeTokens tokensFor(const int gap, const uint8_t radius, const fui::SelectionStyle selection,
                           const bool titleBold) {
  fui::ThemeTokens tokens = fui::themeTokensForLineHeight(24);
  tokens.listRowGap = static_cast<int16_t>(gap);
  tokens.listRowRadius = radius;
  tokens.listSelectionStyle = selection;
  tokens.bodyText.bold = titleBold;
  return tokens;
}

// Two substantially different theme shapes resolve to different modal
// presentation values — Classic (no gap/radius, invert selection) vs
// RoundedRaff (6px gap, 20px radius cards, bold titles) vs Lyra (light pill):
// the modal can NOT look the same under every theme anymore.
TEST(ModalTheme, DistinctThemesResolveDistinctModalValues) {
  namespace mt = modalTheme;
  const auto classic = mt::resolve(tokensFor(0, 0, fui::SelectionStyle::InvertFill, false), false);
  const auto raff = mt::resolve(tokensFor(6, 20, fui::SelectionStyle::InvertFill, true), false);

  EXPECT_EQ(classic.rowGap, 0);
  EXPECT_EQ(raff.rowGap, 6);
  EXPECT_EQ(classic.rowRadius, 0);
  EXPECT_EQ(raff.rowRadius, 20);
  EXPECT_FALSE(classic.bodyStyle.bold);
  EXPECT_TRUE(raff.bodyStyle.bold);

  // Lyra's LightPill selection expands over the row styles exactly as
  // Screen::list() does; Classic keeps InvertFill (no marker).
  const auto lyra = mt::resolve(tokensFor(0, 6, fui::SelectionStyle::LightPill, false), false);
  EXPECT_EQ(lyra.rowStyles.selected.background.kind, fui::PaintKind::Dither);
  EXPECT_EQ(classic.rowStyles.selected.background.kind, fui::PaintKind::Solid);
  EXPECT_EQ(lyra.marker, fui::SelectionMarker::None);

  // Underline/Triangle themes resolve the marker + normal-selected rows.
  const auto underlined = mt::resolve(tokensFor(0, 0, fui::SelectionStyle::Underline, false), false);
  EXPECT_EQ(underlined.marker, fui::SelectionMarker::Underline);
  EXPECT_EQ(underlined.rowStyles.selected.background.kind, classic.rowStyles.normal.background.kind);
}

// ONE resolution drives every modal page type: list() pages, settingRow
// pages and stepper rows take the same row styles, gap, radius, side padding
// and row inset — no page renders its own cadence.
TEST(ModalTheme, SameResolvedThemeAcrossModalPageTypes) {
  namespace mt = modalTheme;
  const auto theme = mt::resolve(tokensFor(6, 20, fui::SelectionStyle::LightPill, true), true);
  // Touch bumps the gap to the theme's touch comfort gap minimum.
  EXPECT_EQ(theme.rowGap, 6);

  // A list page takes exactly the resolved values.
  fui::ListProps listProps{};
  listProps.valueText.font = 1;  // explicit values pass through (resolveListProps parity)
  mt::applyListProps(theme, listProps);
  EXPECT_EQ(listProps.rowGap, theme.rowGap);
  EXPECT_EQ(listProps.rowRadius, theme.rowRadius);
  EXPECT_EQ(listProps.sidePadding, theme.sidePadding);
  EXPECT_EQ(listProps.rowInset, theme.rowInset);
  EXPECT_EQ(listProps.selectionMarker, theme.marker);
  EXPECT_TRUE(listProps.rowStyles.selected.background.kind == theme.rowStyles.selected.background.kind);
  EXPECT_TRUE(listProps.labelText.bold == theme.bodyStyle.bold);
  EXPECT_EQ(listProps.valueText.font, 1);
  EXPECT_EQ(listProps.headerText.font, theme.headerStyle.font);

  // The settingRow pages take the SAME resolved values.
  fui::SettingRowProps row{};
  mt::applySettingRow(theme, row);
  EXPECT_TRUE(row.styles.selected.background.kind == theme.rowStyles.selected.background.kind);
  EXPECT_EQ(row.radius, theme.rowRadius);
  EXPECT_EQ(row.sidePadding, theme.sidePadding);
  EXPECT_TRUE(row.labelText.bold == theme.bodyStyle.bold);
}

// Anchored popups (the in-modal Done/Failed confirmations) lie ENTIRELY
// inside their anchor: the outer frame ring included — the modal's repaint
// covers exactly the anchor rect, so nothing may spill outside it.
TEST(ModalPopup, AnchoredPopupLiesFullyInsideAnchor) {
  // A modal-sized anchor; the metrics popup geometry (frame 2, margins).
  const Rect anchor{100, 200, 300, 150};
  constexpr int FRAME = 2;
  const auto contained = [&](const Rect& content) {
    EXPECT_GE(content.x - FRAME, anchor.x);                                  // outerLeft >= anchor.left
    EXPECT_GE(content.y - FRAME, anchor.y);                                  // outerTop >= anchor.top
    EXPECT_LE(content.x + content.width + FRAME, anchor.x + anchor.width);   // outerRight <= anchor.right
    EXPECT_LE(content.y + content.height + FRAME, anchor.y + anchor.height);  // outerBottom <= anchor.bottom
  };

  // "Done" and the failure popup (content = text + margins) — both centered
  // inside the anchor, frame inside it too.
  const Rect done = BaseTheme::popupRectFor(480, 800, 60, 18, 24, 16, FRAME, 0.075f, &anchor);
  contained(done);
  EXPECT_EQ(done.x, anchor.x + (anchor.width - (60 + 48)) / 2);
  EXPECT_EQ(done.y, anchor.y + FRAME);
  const Rect failed = BaseTheme::popupRectFor(480, 800, 120, 18, 24, 16, FRAME, 0.075f, &anchor);
  contained(failed);

  // Unanchored popups keep the original screen-top placement.
  const Rect top = BaseTheme::popupRectFor(480, 800, 60, 18, 24, 16, FRAME, 0.075f, nullptr);
  EXPECT_EQ(top.x, (480 - (60 + 48)) / 2);
  EXPECT_EQ(top.y, static_cast<int>(800 * 0.075f));
}

// ---- Disabled-row cursor presentation (rowPresentation::disabledRowCursor +
// the REAL list registration): the four row states must be visually distinct,
// and a selected disabled row keeps a visible cursor while staying inert. ----

// The resolve() precedence fact behind the UAT finding (the root cause, pinned
// so the app-side workaround stays honest): StateDisabled wins over
// StateSelected, so a selected disabled row resolves to the DISABLED style —
// the strong selected presentation never draws there.
TEST(DisabledFocus, ResolvePutsDisabledAboveSelected) {
  const fui::StyleSet styles = fui::defaultListRowStyles();
  // enabled+selected: the strong selection (solid black fill, white text).
  const auto& selected = styles.resolve(fui::StateSelected);
  EXPECT_TRUE(selected.background.kind == fui::PaintKind::Solid);
  EXPECT_TRUE(selected.background.color == fui::Color::Black);
  // disabled: subdued (white fill, light-gray text).
  const auto& disabled = styles.resolve(fui::StateDisabled);
  EXPECT_TRUE(disabled.background.kind == fui::PaintKind::Solid);
  EXPECT_TRUE(disabled.background.color == fui::Color::White);
  EXPECT_TRUE(disabled.foreground.kind == fui::PaintKind::Dither);
  // selected+disabled resolves to the DISABLED style — SDK-wide precedence,
  // deliberately untouched.
  const fui::State both = static_cast<fui::State>(fui::StateSelected | fui::StateDisabled);
  EXPECT_TRUE(styles.resolve(both).background.color == disabled.background.color);
  EXPECT_TRUE(styles.resolve(both).foreground.kind == disabled.foreground.kind);
  // The three presentations are distinct from each other.
  EXPECT_TRUE(selected.background.color != disabled.background.color);
  EXPECT_TRUE(styles.resolve(fui::StateNormal).background.color != selected.background.color);
}

// The weak cursor policy: the theme's own marker vocabulary for marker-based
// selection styles, the SDK's underline marker for fill-based themes — never
// absent for a selected disabled row.
TEST(DisabledFocus, CursorMarkerFollowsTheme) {
  namespace rp = rowPresentation;
  EXPECT_EQ(rp::disabledRowCursor(fui::SelectionStyle::Underline), fui::SelectionMarker::Underline);
  EXPECT_EQ(rp::disabledRowCursor(fui::SelectionStyle::Triangle), fui::SelectionMarker::Triangle);
  EXPECT_EQ(rp::disabledRowCursor(fui::SelectionStyle::InvertFill), fui::SelectionMarker::Underline);
  EXPECT_EQ(rp::disabledRowCursor(fui::SelectionStyle::LightPill), fui::SelectionMarker::Underline);
}

// Inertness proof against the REAL registration/routing: a selected DISABLED
// row registers no touch hit at all (list() skips !enabled rows), so touch
// activation can never mutate it — while its cursor comes from the marker
// the app layer adds (pure policy above), not from any registered
// interaction.
TEST(DisabledFocus, SelectedDisabledRowIsInert) {
  const auto device = geometryDevice(true, 44);
  // Settings-shaped fragment: enabled row, DISABLED row (the selected one),
  // enabled row. The selected disabled row carries StateSelected via
  // props.selectedIndex and enabled=false, exactly the settings build does.
  const auto buildFragment = [](fui::Frame<16>& frame, const int16_t y) {
    fui::ListItem items[3]{};
    items[0].label = "Enabled";
    items[0].actionValue = 0;
    items[1].label = "DisabledSelected";
    items[1].actionValue = 1;
    items[1].enabled = false;
    items[2].label = "Enabled";
    items[2].actionValue = 2;

    fui::ListProps props{};
    props.items = items;
    props.count = 3;
    props.scrollIndicator = false;
    props.action = ACTION_ROW;
    props.inputMask = fui::InputTouch;
    props.rowHeight = ROW_H;
    props.selectedIndex = 1;  // the cursor rests on the disabled row
    fui::list(frame, fui::Rect{BODY_X, y, BODY_W, static_cast<int16_t>(3 * ROW_H)}, props);
  };

  fui::InteractionBuffer<16> interactions;
  GeometryTarget target;
  const fui::InputSnapshot noInput{};
  interactions.beginPublishCycle();
  fui::Frame<16> frame(target, device, noInput, interactions);
  buildFragment(frame, BODY_Y);
  interactions.publish();

  // Only the ENABLED rows registered hits; the selected disabled row did not.
  ASSERT_EQ(interactions.publishedCount(), 2u);
  const fui::Interaction* hits = interactions.publishedData();
  EXPECT_EQ(hits[0].value, 0);
  EXPECT_EQ(hits[1].value, 2);

  // A tap inside the selected disabled row's band routes NOWHERE.
  const int16_t disabledY = static_cast<int16_t>(BODY_Y + ROW_H + ROW_H / 2);
  const Routed r = routeTap(interactions, device, buildFragment, BODY_Y, BODY_X + 10, disabledY);
  EXPECT_FALSE(r.routed);
  // The enabled rows around it still route to their own rows.
  const Routed first = routeTap(interactions, device, buildFragment, BODY_Y, BODY_X + 10,
                                static_cast<int16_t>(BODY_Y + ROW_H / 2));
  EXPECT_TRUE(first.routed);
  EXPECT_EQ(first.value, 0);
  const Routed last =
      routeTap(interactions, device, buildFragment, BODY_Y, BODY_X + 10, static_cast<int16_t>(BODY_Y + ROW_H * 5 / 2));
  EXPECT_TRUE(last.routed);
  EXPECT_EQ(last.value, 2);
}
