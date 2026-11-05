// The one-shot pilot's wiring contract, enforced on the source text of
// src/main.cpp, lib/hal/HalPowerManager.{h,cpp} and platformio.ini (P1-B8):
// - the RTC arm record is written only AFTER the hardware timer accepted the
//   wake request (fail closed against a record armed without its wake source);
// - the pilot wake dispatches through the unchanged companion-gate block, and
//   no path of the window or the abort re-arms a timer;
// - the diagnostic GATEARM command keeps its existing arm-then-sleep shape.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace {

namespace fs = std::filesystem;

std::string readAll(const fs::path& path) {
  std::ifstream in(path);
  std::string all;
  std::string line;
  while (std::getline(in, line)) all += line + "\n";
  return all;
}

std::string stripLineComments(std::string text) {
  for (auto pos = text.find("//"); pos != std::string::npos; pos = text.find("//", pos)) {
    const auto lineEnd = text.find('\n', pos);
    if (lineEnd == std::string::npos) {
      text.resize(pos);
      break;
    }
    text.erase(pos, lineEnd - pos);
  }
  return text;
}

std::string functionBody(const std::string& all, const std::string& signature, const std::string& nextMarker) {
  const auto begin = all.find(signature);
  if (begin == std::string::npos) return "";
  const auto end = all.find(nextMarker, begin);
  if (end == std::string::npos) return "";
  return stripLineComments(all.substr(begin, end - begin));
}

}  // namespace

TEST(CompanionPilotContract, PilotSitsBehindTheCompileTimeFlag) {
  const std::string main = readAll(fs::path(REPO_ROOT_PATH) / "src" / "main.cpp");
  const auto guard = main.find("#ifdef CROSSPOINT_COMPANION_PILOT_AUTOWAKE");
  ASSERT_NE(guard, std::string::npos) << "the pilot must be a compile-time experiment";
  const auto armFn = main.find("static uint64_t armCompanionPilot", guard);
  ASSERT_NE(armFn, std::string::npos);
  const auto endif = main.find("#endif", armFn);
  ASSERT_NE(endif, std::string::npos) << "the pilot helper must close its flag guard";
  const auto enterDeepSleep = main.find("void enterDeepSleep", endif);
  ASSERT_NE(enterDeepSleep, std::string::npos);
  const auto secondGuard = main.find("#ifdef CROSSPOINT_COMPANION_PILOT_AUTOWAKE", enterDeepSleep);
  ASSERT_NE(secondGuard, std::string::npos) << "the call site must share the flag guard";
  const auto secondEndif = main.find("#endif", secondGuard);
  ASSERT_NE(secondEndif, std::string::npos);
  EXPECT_EQ(main.find("armCompanionPilot", secondEndif), std::string::npos)
      << "pilot code must not leak past the flag guards";
}

TEST(CompanionPilotContract, TimerIsPreArmedBeforeTheArmRecord) {
  const std::string main = readAll(fs::path(REPO_ROOT_PATH) / "src" / "main.cpp");
  const std::string body = functionBody(main, "static uint64_t armCompanionPilot", "void enterDeepSleep");
  ASSERT_FALSE(body.empty()) << "the pilot helper must exist";
  const auto preArm = body.find("powerManager.armTimerWake(");
  ASSERT_NE(preArm, std::string::npos) << "the hardware timer must be pre-armed";
  const auto rejected = body.find("pilot timer wake rejected");
  ASSERT_NE(rejected, std::string::npos) << "a rejected timer request must be logged";
  const auto armRecord = body.find("companiongate::arm(");
  ASSERT_NE(armRecord, std::string::npos);
  EXPECT_LT(rejected, armRecord) << "the rejection path must return before any arm record exists";
  EXPECT_LT(body.find("return 0;", rejected), armRecord) << "the rejection path must return 0 (button-only sleep)";
  EXPECT_LT(preArm, armRecord) << "the arm record is written only after the timer was accepted";
}

TEST(CompanionPilotContract, NonDiagnosticOneShotArmThroughTheGateRecord) {
  const std::string main = readAll(fs::path(REPO_ROOT_PATH) / "src" / "main.cpp");
  const std::string body = functionBody(main, "static uint64_t armCompanionPilot", "void enterDeepSleep");
  ASSERT_FALSE(body.empty());
  EXPECT_NE(body.find("companiongate::arm(false)"), std::string::npos)
      << "the pilot arms the same one-shot RTC record the dispatch consumes";
  // The delay comes from the pilot policy constant, not a second interval.
  EXPECT_NE(body.find("companionpilot::kPilotTimerSeconds"), std::string::npos);
  EXPECT_EQ(body.find("30000000"), std::string::npos) << "no literal second interval may diverge from the policy";
}

TEST(CompanionPilotContract, TimerWakeDispatchesThroughTheExistingGateBlock) {
  const std::string main = readAll(fs::path(REPO_ROOT_PATH) / "src" / "main.cpp");
  const std::string body = functionBody(main, "static uint64_t armCompanionPilot", "void enterDeepSleep");
  ASSERT_FALSE(body.empty());
  // The pilot must NOT grow a second dispatch: the timer wake is an ordinary
  // timer wake with an armed record, handled by the unchanged gate block.
  EXPECT_EQ(body.find("companiongate::dispatch"), std::string::npos);
  EXPECT_EQ(body.find("runCompanionGate"), std::string::npos);
  EXPECT_EQ(body.find("enterHardwareDeepSleep"), std::string::npos) << "the pilot arms, it never sleeps directly";
}

TEST(CompanionPilotContract, OrdinarySleepCarriesThePilotTimer) {
  const std::string main = readAll(fs::path(REPO_ROOT_PATH) / "src" / "main.cpp");
  const std::string body = functionBody(main, "void enterDeepSleep(bool fromTimeout", "void enterHardwareDeepSleep");
  ASSERT_FALSE(body.empty());
  const auto pilotCall = body.find("armCompanionPilot(slideshowSleep)");
  ASSERT_NE(pilotCall, std::string::npos) << "the pilot decision rides on the shared sleep entry";
  const auto handoff = body.find("enterHardwareDeepSleep(slideshowSleep ? slideshow::intervalMicros", pilotCall);
  ASSERT_NE(handoff, std::string::npos);
  EXPECT_NE(body.find(": pilotTimerUs", handoff), std::string::npos)
      << "the pre-armed timer must be passed to the shared sleep handoff";
}

TEST(CompanionPilotContract, WindowExitsStayButtonOnly) {
  const std::string main = readAll(fs::path(REPO_ROOT_PATH) / "src" / "main.cpp");
  const std::string body = functionBody(main, "static void sleepAfterGate()", "static void runCompanionGate");
  ASSERT_FALSE(body.empty()) << "the shared gate sleep helper must exist";
  EXPECT_NE(body.find("powerManager.startDeepSleep(gpio, false, 0)"), std::string::npos)
      << "every window exit sleeps power-button-only: the pilot is one-shot";
  EXPECT_EQ(body.find("timerWakeUs"), std::string::npos) << "no window exit may re-arm a timer";
}

TEST(CompanionPilotContract, PowerAbortReturnsIntoTheOrdinaryBootWithoutReArm) {
  const std::string main = readAll(fs::path(REPO_ROOT_PATH) / "src" / "main.cpp");
  // The pilot arm decision is made in exactly one place (the definition and
  // its single call site): a power abort that returns into the ordinary boot
  // cannot silently arm another window.
  size_t occurrences = 0;
  for (auto pos = main.find("armCompanionPilot("); pos != std::string::npos;
       pos = main.find("armCompanionPilot(", pos + 1)) {
    ++occurrences;
  }
  EXPECT_EQ(occurrences, 2u) << "definition plus the single enterDeepSleep call site";
  EXPECT_NE(main.find("wakePowerReleasePending = true;\n      return;"), std::string::npos)
      << "the window's power abort must still return into the ordinary boot";
}

TEST(CompanionPilotContract, DiagnosticGatearmCommandKeepsItsShape) {
  const std::string main = readAll(fs::path(REPO_ROOT_PATH) / "src" / "main.cpp");
  const std::string body = functionBody(main, "if (cmd.startsWith(\"GATEARM\"))", "#ifdef CROSSPOINT_RTC_CLK_DIAG");
  ASSERT_FALSE(body.empty()) << "the diagnostic GATEARM handler must stay";
  // The usage comment documents the command; the code lines pin the behavior.
  EXPECT_NE(main.find("Usage: CMD:GATEARM <seconds in [5,3600]> [diag]"), std::string::npos);
  const auto armRecord = body.find("companiongate::arm(diagnostic);");
  ASSERT_NE(armRecord, std::string::npos);
  const auto sleep = body.find("enterHardwareDeepSleep(static_cast<uint64_t>(seconds) * 1000000ULL);", armRecord);
  ASSERT_NE(sleep, std::string::npos) << "GATEARM still sleeps on the explicit test timer";
}

TEST(CompanionPilotContract, HalPreArmLeavesNoSourceOnRejection) {
  const std::string header = readAll(fs::path(REPO_ROOT_PATH) / "lib" / "hal" / "HalPowerManager.h");
  EXPECT_NE(header.find("bool armTimerWake(uint64_t timerWakeUs) const;"), std::string::npos)
      << "the pre-arm API must exist on the power manager";
  const std::string power = readAll(fs::path(REPO_ROOT_PATH) / "lib" / "hal" / "HalPowerManager.cpp");
  const std::string body = functionBody(power, "bool HalPowerManager::armTimerWake", "#if FREEINK_DEVICE_PICCO");
  ASSERT_FALSE(body.empty()) << "the pre-arm implementation must exist";
  const auto armCall = body.find("esp_sleep_enable_timer_wakeup(timerWakeUs)");
  ASSERT_NE(armCall, std::string::npos);
  const auto disable = body.find("esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER)", armCall);
  ASSERT_NE(disable, std::string::npos) << "a rejected request must clean the wake source";
  const auto restart = body.find("esp_restart()", disable);
  ASSERT_NE(restart, std::string::npos) << "an uncleanable wake-source state must restart, not sleep";
  const auto startSleep = power.find("void HalPowerManager::startDeepSleep");
  ASSERT_NE(startSleep, std::string::npos);
  const auto delegate = power.find("timerWakeArmed = armTimerWake(timerWakeUs);", startSleep);
  ASSERT_NE(delegate, std::string::npos) << "startDeepSleep must arm through the same single path";
}

TEST(CompanionPilotContract, PilotFlagIsDefinedOnlyInTheExperimentalEnv) {
  const std::string ini = readAll(fs::path(REPO_ROOT_PATH) / "platformio.ini");
  size_t occurrences = 0;
  for (auto pos = ini.find("-DCROSSPOINT_COMPANION_PILOT_AUTOWAKE"); pos != std::string::npos;
       pos = ini.find("-DCROSSPOINT_COMPANION_PILOT_AUTOWAKE", pos + 1)) {
    ++occurrences;
  }
  EXPECT_EQ(occurrences, 1u) << "the pilot flag is a single experimental env, default builds stay off";
}
