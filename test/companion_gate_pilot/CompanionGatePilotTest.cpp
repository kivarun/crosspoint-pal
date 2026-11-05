// The one-shot pilot's arm decision (P1-B8): pure, no I/O, one discriminant
// per guard. Every refusal names its cause; every acceptance is the same
// one-shot arm.
#include <gtest/gtest.h>

#include "util/CompanionGatePilot.h"

namespace {

companionpilot::Decision evaluate(const bool boardSupported, const bool usbConnected, const bool slideshowSleep,
                                  const bool identityValid, const bool batteryOk) {
  companionpilot::Inputs in{};
  in.boardSupported = boardSupported;
  in.usbConnected = usbConnected;
  in.slideshowSleep = slideshowSleep;
  in.identityValid = identityValid;
  in.batteryOk = batteryOk;
  return companionpilot::evaluate(in);
}

companionpilot::Decision eligible() { return evaluate(true, false, false, true, true); }

}  // namespace

TEST(CompanionGatePilot, EligibleSleepArmsTheOneShotWindow) {
  const auto decision = eligible();
  EXPECT_EQ(decision.action, companionpilot::Action::Arm);
  EXPECT_EQ(decision.reason, companionpilot::SkipReason::None);
}

TEST(CompanionGatePilot, UsbConnectedNeverArms) {
  const auto decision = evaluate(true, true, false, true, true);
  EXPECT_EQ(decision.action, companionpilot::Action::Skip);
  EXPECT_EQ(decision.reason, companionpilot::SkipReason::Usb);
}

TEST(CompanionGatePilot, SlideshowSleepNeverArms) {
  const auto decision = evaluate(true, false, true, true, true);
  EXPECT_EQ(decision.action, companionpilot::Action::Skip);
  EXPECT_EQ(decision.reason, companionpilot::SkipReason::Slideshow);
}

TEST(CompanionGatePilot, MissingIdentityNeverArms) {
  const auto decision = evaluate(true, false, false, false, true);
  EXPECT_EQ(decision.action, companionpilot::Action::Skip);
  EXPECT_EQ(decision.reason, companionpilot::SkipReason::Identity);
}

TEST(CompanionGatePilot, LowBatteryNeverArms) {
  const auto decision = evaluate(true, false, false, true, false);
  EXPECT_EQ(decision.action, companionpilot::Action::Skip);
  EXPECT_EQ(decision.reason, companionpilot::SkipReason::Battery);
}

TEST(CompanionGatePilot, UnsupportedBoardNeverArms) {
  const auto decision = evaluate(false, false, false, true, true);
  EXPECT_EQ(decision.action, companionpilot::Action::Skip);
  EXPECT_EQ(decision.reason, companionpilot::SkipReason::Board);
}

// Refusals are decided in priority order: a single input flip names exactly
// its own cause, and the first blocking condition wins.
TEST(CompanionGatePilot, FirstBlockingConditionWins) {
  const auto decision = evaluate(false, true, true, false, false);
  EXPECT_EQ(decision.action, companionpilot::Action::Skip);
  EXPECT_EQ(decision.reason, companionpilot::SkipReason::Board);
}

// The pilot is strictly one-shot: its delay is the experiment's 30 s.
TEST(CompanionGatePilot, PilotDelayIsTheExperimentConstant) { EXPECT_EQ(companionpilot::kPilotTimerSeconds, 30u); }
