#pragma once

// Pure arm policy for the one-shot autonomous BLE wake pilot (P1-B8,
// experimental): the ONE answer to "may this ordinary sleep entry arm a
// single delayed headless gate window". The caller in main.cpp gathers the
// inputs at the shared deep-sleep entry and executes the decision: on Arm it
// pre-arms the hardware timer through HalPowerManager::armTimerWake FIRST and
// only then writes the RTC gate arm record, so a rejected timer request
// leaves no armed record behind (fail closed). No retries, no schedule, no
// persistent setting: exactly one window per eligible sleep, and the window
// itself never re-arms.

#include <cstdint>

namespace companionpilot {

// The single pilot delay (P1-B8 experiment; not a production interval).
inline constexpr uint32_t kPilotTimerSeconds = 30;

enum class Action : uint8_t { Skip, Arm };

// Why an ordinary sleep entry may not arm the pilot window.
enum class SkipReason : uint8_t { None, Board, Usb, Slideshow, Identity, Battery };

struct Inputs {
  bool boardSupported;  // the proven timer-wake topology (X4 Classic)
  bool usbConnected;
  bool slideshowSleep;  // a slideshow owns the wake: this sleep arms the
                        // interval timer, or a retained slideshow state would
                        // pre-empt the wake at the dispatch
  bool identityValid;
  bool batteryOk;
};

struct Decision {
  Action action;
  SkipReason reason;  // SkipReason::None when action == Action::Arm
};

inline Decision evaluate(const Inputs& in) {
  if (!in.boardSupported) return {Action::Skip, SkipReason::Board};
  // The slideshow owns this wake; arming a second wake would leave one of the
  // two wake purposes unfulfilled.
  if (in.slideshowSleep) return {Action::Skip, SkipReason::Slideshow};
  if (in.usbConnected) return {Action::Skip, SkipReason::Usb};
  // No identity → the radio is never raised; low battery → no radio.
  if (!in.identityValid) return {Action::Skip, SkipReason::Identity};
  if (!in.batteryOk) return {Action::Skip, SkipReason::Battery};
  return {Action::Arm, SkipReason::None};
}

inline const char* skipReasonName(const SkipReason reason) {
  switch (reason) {
    case SkipReason::None:
      return "none";
    case SkipReason::Board:
      return "board";
    case SkipReason::Usb:
      return "usb";
    case SkipReason::Slideshow:
      return "slideshow";
    case SkipReason::Identity:
      return "identity";
    case SkipReason::Battery:
      return "battery";
  }
  return "unknown";
}

}  // namespace companionpilot
