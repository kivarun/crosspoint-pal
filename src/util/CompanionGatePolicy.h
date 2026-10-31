#pragma once

// Pure dispatch policy for the headless Sleep Companion gate (host-tested):
// the ONE answer to "what does this wake do with the gate arm". No I/O, no
// includes beyond the standard library — the caller in main.cpp gathers the
// inputs (wake cause, RTC states, identity mirror, battery, USB) and executes
// the returned action. The matrix keeps the existing priorities intact:
// recovery and the sleep-screen slideshow always pre-empt the gate, and every
// non-dispatching wake clears the one-shot arm (fail closed).

#include <cstdint>

namespace companiongate {

// The wake discrimination the policy needs (HalGPIO::WakeupReason mapped by
// the caller; a pure header must not pull the HAL in).
enum class WakeKind : uint8_t { Other, Timer };

enum class Action : uint8_t {
  // Nothing to do: continue the ordinary boot flow. The arm is already
  // invalid, so nothing is cleared.
  Ordinary,
  // A slideshow timer wake owns the wake: clear the stale gate arm and let
  // the unchanged slideshow resume machinery run later in setup().
  SlideshowWins,
  // Armed but unsafe to run (no identity, low battery): clear the arm and go
  // straight back to sleep power-button-only — never re-arms a timer, so no
  // wake series can form.
  SleepNoRadio,
  // Runnable except for a stand-down condition (recovery mode, unsupported
  // board, USB without the diagnostic override): clear the arm and continue
  // the ordinary boot.
  StandDown,
  // Run the gate window. The caller consumes the arm (one-shot) and executes
  // the bounded BLE window; the window itself exits by sleeping again or by
  // falling through to the ordinary boot (Power abort).
  Run,
};

struct Inputs {
  WakeKind wake;
  bool recoveryMode;
  bool boardSupported;
  bool slideshowHeld;
  bool gateArmed;
  bool identityValid;
  bool batteryOk;
  bool usbConnected;
  // Dev-only USB bypass override carried in the RTC arm record. Production
  // builds always pass false (the byte is never set nor read there).
  bool diagnosticOverride;
};

inline Action dispatch(const Inputs& in) {
  // Recovery always wins, and a board without the proven timer-wake topology
  // never dispatches (the arm is stale there by construction).
  if (in.recoveryMode || !in.boardSupported) return Action::StandDown;
  // Every non-timer wake clears the arm: fail closed against stale arms.
  if (in.wake != WakeKind::Timer) return Action::StandDown;
  // The sleep-screen slideshow keeps priority over the gate.
  if (in.slideshowHeld) return Action::SlideshowWins;
  if (!in.gateArmed) return Action::Ordinary;
  // No identity → the radio is never raised (no adopt-on-first-bond); low
  // battery → no radio and no re-arm. Both go straight back to sleep.
  if (!in.identityValid || !in.batteryOk) return Action::SleepNoRadio;
  // USB stays a development/debugging surface: boot normally so CDC remains
  // available, unless a diagnostic run explicitly overrides the bypass.
  if (in.usbConnected && !in.diagnosticOverride) return Action::StandDown;
  return Action::Run;
}

}  // namespace companiongate
