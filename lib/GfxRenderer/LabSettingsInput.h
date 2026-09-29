#pragma once

#include <cstdint>

// Image Lab settings-page input policy (experiment/x4-image-lab).
//
// The Image settings page is 2-axis: the vertical axis chooses the setting row
// (the fixed side Up/Down keys), the horizontal axis modifies the selected
// value (the user-configured front Left/Right keys). MappedInputManager's
// merged list navigation (NavNext = Down || Right, NavPrevious = Up || Left)
// is intentionally NOT used on this page: it would make one front Left/Right
// event match both the value branch and the row-selection branch. Here one
// button event maps to EXACTLY ONE semantic action; no event swallowing,
// debounce, or latches are involved.
namespace labSettingsInput {

enum class Button : uint8_t { None = 0, Up, Down, Left, Right, Confirm, Back };
enum class Action : uint8_t { None = 0, RowUp, RowDown, ValueDown, ValueUp, Activate, Back };

// Map one button event (press edge first, then release edge) to its single
// semantic action. Press edges carry the four directional axes; release edges
// carry the two action buttons.
inline Action actionFor(const Button button, const bool press, const bool release) {
  if (press) {
    switch (button) {
      case Button::Up:
        return Action::RowUp;
      case Button::Down:
        return Action::RowDown;
      case Button::Left:
        return Action::ValueDown;
      case Button::Right:
        return Action::ValueUp;
      default:
        return Action::None;
    }
  }
  if (release) {
    switch (button) {
      case Button::Confirm:
        return Action::Activate;
      case Button::Back:
        return Action::Back;
      default:
        return Action::None;
    }
  }
  return Action::None;
}

// Confirm-activation routing for the Image settings page rows (pure policy).
// Every editable value row AND the explicit Apply row share the one
// viewer-Apply route; the dedicated sleep row is the only save-profile route;
// anything out of range activates nothing. Mutually exclusive by construction.
enum class RowAction : uint8_t { None = 0, ApplyViewer, SaveSleepProfile };
inline RowAction confirmActionForRow(const int row, const int paramCount) {
  if (row < 0 || row > paramCount + 1) return RowAction::None;
  return row == paramCount + 1 ? RowAction::SaveSleepProfile : RowAction::ApplyViewer;
}

// DeleteConfirm page contract (pure): labModalRow indexes ACTIONS. The page
// shows informational rows first (title header, Name label, up to two wrapped
// filename lines — never selectable), then Cancel (action 0) and Delete
// (action 1). Maps a visual row index to its action index, or -1 for
// informational rows. The renderer builds exactly this order; host tests pin
// the destructive-action invariant (what is visibly focused == what Confirm
// will execute).
inline int deleteConfirmActionOfVisualRow(const int visualRow, const int nameLineCount) {
  const int action = visualRow - (2 + nameLineCount);  // title + Name label + lines
  return (action == 0 || action == 1) ? action : -1;
}

}  // namespace labSettingsInput
