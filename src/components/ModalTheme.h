#pragma once
#include <FreeInkUI.h>

namespace fui = freeink::ui;

// Modal theme adapter (the Image Viewer modal surface): resolves the ACTIVE
// UI theme's FreeInkUI tokens against the modal's raw primitives, so the
// modal's pages follow the same look as every themed app list — the same
// model FreeInkApp::resolveListProps() applies on Screen-hosted lists, reused
// here because the modal drives a bare fui::Frame (which owns no tokens).
//
// Row HEIGHT stays out of this resolution on purpose: the modal's cadence is
// its own device policy (imageSettingsInput::modalRowHeight) and is passed as
// an explicit ListProps.rowHeight, which resolveListProps also treats as a
// caller-owned minimum. Everything else here is the tokens' shape data.
namespace modalTheme {

// The resolved list-row presentation, shared by every modal page type
// (list() pages, settingRow pages, stepper rows) so Options, Image Settings,
// Slideshow, Image Info and the detail views all follow one resolution.
struct ModalListTheme {
  fui::StyleSet rowStyles{};
  fui::SelectionMarker marker = fui::SelectionMarker::None;
  int16_t rowGap = 0;
  uint8_t rowRadius = 0;
  int16_t sidePadding = 8;
  int16_t rowInset = 0;
  // Text roles the themed rows draw with (FreeInkApp::resolveListProps()'s
  // unset-style defaults): row labels follow the theme body style (title
  // boldness), headers the theme small style.
  fui::TextStyle bodyStyle{};
  fui::TextStyle headerStyle{};
};

// Ports FreeInkApp::resolveListProps()'s shape resolution (everything except
// the font-derived row height the modal pins itself):
// - row styles from the theme's listRow (its default already is the classic
//   set), with the theme's listSelectionStyle expanded over it exactly like
//   Screen::list() does;
// - row gap from the theme, raised to the touch comfort gap on touch targets;
// - row radius, side padding and row inset (the Lyra pill band) from tokens;
// - the body/header text roles from the tokens.
inline ModalListTheme resolve(const fui::ThemeTokens& tokens, const bool touchCapable) {
  ModalListTheme theme;
  theme.rowStyles = tokens.listRow.unset() ? fui::defaultListRowStyles() : tokens.listRow;
  switch (tokens.listSelectionStyle) {
    case fui::SelectionStyle::LightPill:
      theme.rowStyles.selected.background = fui::Paint::dither(fui::Color::LightGray);
      theme.rowStyles.selected.foreground = fui::Paint::solid(fui::Color::Black);
      theme.rowStyles.active = theme.rowStyles.selected;
      break;
    case fui::SelectionStyle::Underline:
    case fui::SelectionStyle::Triangle:
      theme.rowStyles.selected = theme.rowStyles.normal;  // the marker shows the selection
      theme.marker = tokens.listSelectionStyle == fui::SelectionStyle::Underline
                         ? fui::SelectionMarker::Underline
                         : fui::SelectionMarker::Triangle;
      break;
    case fui::SelectionStyle::InvertFill:
    default:
      break;
  }
  theme.rowGap = tokens.listRowGap;
  if (touchCapable && theme.rowGap < tokens.listTouchRowGap) theme.rowGap = tokens.listTouchRowGap;
  theme.rowRadius = tokens.listRowRadius;
  theme.sidePadding = tokens.listSidePadding;
  theme.rowInset = tokens.listInset;
  theme.bodyStyle = tokens.bodyText;
  theme.headerStyle = tokens.smallText;
  return theme;
}

// Fills a raw list()'s props with the resolved theme (unset-style resolution
// identical to FreeInkApp::resolveListProps(): explicit label/value styles
// pass through, unset ones take the theme's roles).
inline void applyListProps(const ModalListTheme& theme, fui::ListProps& props) {
  if (fui::textStyleUnset(props.labelText)) props.labelText = theme.bodyStyle;
  if (fui::textStyleUnset(props.headerText)) props.headerText = theme.headerStyle;
  props.rowStyles = theme.rowStyles;
  props.selectionMarker = theme.marker;
  props.rowGap = theme.rowGap;
  props.rowRadius = theme.rowRadius;
  props.sidePadding = theme.sidePadding;
  props.rowInset = theme.rowInset;
}

// Fills a settingRow's props with the same resolved theme.
inline void applySettingRow(const ModalListTheme& theme, fui::SettingRowProps& row) {
  if (fui::textStyleUnset(row.labelText)) row.labelText = theme.bodyStyle;
  row.styles = theme.rowStyles;
  row.radius = theme.rowRadius;
  row.sidePadding = theme.sidePadding;
}

}  // namespace modalTheme
