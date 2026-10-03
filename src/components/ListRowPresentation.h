#pragma once
#include <FreeInkUI.h>

namespace fui = freeink::ui;

// App-side presentation helpers for list rows where the SDK's StyleSet
// resolution cannot express the wanted state combination. The SDK's global
// resolve() precedence (StateDisabled above StateSelected) is deliberately
// NOT touched here — it would change every upstream screen.
namespace rowPresentation {

// A selected DISABLED row draws its disabled style (StateDisabled wins
// resolve()), so the strong selected fill never appears and the physical
// button cursor would vanish on e.g. the Settings rows that Sleep Screen =
// Slideshow disables. The app layer instead draws the theme's SELECTION
// MARKER on that one row: the theme's own marker vocabulary when its
// selection style is marker-based, otherwise the SDK's underline marker as
// the generic weak cursor for fill-based themes. Always weaker than the
// strong selected presentation, and the subdued disabled foreground stays.
inline fui::SelectionMarker disabledRowCursor(const fui::SelectionStyle themeSelectionStyle) {
  switch (themeSelectionStyle) {
    case fui::SelectionStyle::Underline:
      return fui::SelectionMarker::Underline;
    case fui::SelectionStyle::Triangle:
      return fui::SelectionMarker::Triangle;
    case fui::SelectionStyle::InvertFill:
    case fui::SelectionStyle::LightPill:
    default:
      return fui::SelectionMarker::Underline;
  }
}

}  // namespace rowPresentation
