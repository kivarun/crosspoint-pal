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

}  // namespace labSettingsInput
