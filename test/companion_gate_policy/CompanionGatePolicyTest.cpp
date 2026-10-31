#include <gtest/gtest.h>

#include "util/CompanionGatePolicy.h"

// The headless gate's dispatch matrix (p1-companion-gate preflight §2 Q5/Q6,
// rev. 3). Every row of the matrix is pinned: recovery and the slideshow keep
// priority, every non-dispatching wake clears the one-shot arm, and the radio
// is never raised without an identity and a battery margin.

namespace {

using companiongate::Action;
using companiongate::Inputs;
using companiongate::WakeKind;

Inputs row(const WakeKind wake = WakeKind::Timer, const bool armed = true) {
  Inputs in{};
  in.wake = wake;
  in.recoveryMode = false;
  in.boardSupported = true;
  in.slideshowHeld = false;
  in.gateArmed = armed;
  in.identityValid = true;
  in.batteryOk = true;
  in.usbConnected = false;
  in.diagnosticOverride = false;
  return in;
}

}  // namespace

TEST(CompanionGateDispatch, RunsTheWindowOnACleanTimerWake) {
  EXPECT_EQ(companiongate::dispatch(row()), Action::Run);
}

TEST(CompanionGateDispatch, RecoveryAlwaysWins) {
  auto in = row();
  in.recoveryMode = true;
  EXPECT_EQ(companiongate::dispatch(in), Action::StandDown);
  // Even the diagnostic override cannot push past recovery.
  in.diagnosticOverride = true;
  in.usbConnected = true;
  EXPECT_EQ(companiongate::dispatch(in), Action::StandDown);
}

TEST(CompanionGateDispatch, UnsupportedBoardsNeverDispatch) {
  auto in = row();
  in.boardSupported = false;
  EXPECT_EQ(companiongate::dispatch(in), Action::StandDown);
}

TEST(CompanionGateDispatch, EveryNonTimerWakeClearsTheArm) {
  for (const auto wake : {WakeKind::Other}) {
    auto in = row(wake);
    EXPECT_EQ(companiongate::dispatch(in), Action::StandDown);
    in.usbConnected = true;
    EXPECT_EQ(companiongate::dispatch(in), Action::StandDown);
  }
}

TEST(CompanionGateDispatch, SlideshowKeepsPriority) {
  auto in = row();
  in.slideshowHeld = true;
  EXPECT_EQ(companiongate::dispatch(in), Action::SlideshowWins);
  // The slideshow wins regardless of what the gate inputs say.
  in.usbConnected = true;
  in.identityValid = false;
  EXPECT_EQ(companiongate::dispatch(in), Action::SlideshowWins);
}

TEST(CompanionGateDispatch, UnarmedOrdinaryWakeIsOrdinary) {
  auto in = row(WakeKind::Timer, /*armed=*/false);
  EXPECT_EQ(companiongate::dispatch(in), Action::Ordinary);
  in.slideshowHeld = true;
  EXPECT_EQ(companiongate::dispatch(in), Action::SlideshowWins);
}

TEST(CompanionGateDispatch, NoIdentitySleepsWithoutTheRadio) {
  auto in = row();
  in.identityValid = false;
  EXPECT_EQ(companiongate::dispatch(in), Action::SleepNoRadio);
}

TEST(CompanionGateDispatch, LowBatterySleepsWithoutReArm) {
  auto in = row();
  in.batteryOk = false;
  EXPECT_EQ(companiongate::dispatch(in), Action::SleepNoRadio);
  // The battery check precedes the USB bypass: no input combination yields a
  // radio window on a dead battery.
  in.diagnosticOverride = true;
  in.usbConnected = true;
  EXPECT_EQ(companiongate::dispatch(in), Action::SleepNoRadio);
}

TEST(CompanionGateDispatch, UsbBypassBootsNormally) {
  auto in = row();
  in.usbConnected = true;
  EXPECT_EQ(companiongate::dispatch(in), Action::StandDown);
}

TEST(CompanionGateDispatch, DiagnosticOverrideRunsOnUsb) {
  auto in = row();
  in.usbConnected = true;
  in.diagnosticOverride = true;
  EXPECT_EQ(companiongate::dispatch(in), Action::Run);
}

TEST(CompanionGateDispatch, DiagnosticOverrideCannotRescueMissingIdentity) {
  auto in = row();
  in.usbConnected = true;
  in.diagnosticOverride = true;
  in.identityValid = false;
  EXPECT_EQ(companiongate::dispatch(in), Action::SleepNoRadio);
}
