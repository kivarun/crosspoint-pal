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

size_t countOccurrences(const std::string& text, const std::string& needle) {
  size_t count = 0;
  for (auto pos = text.find(needle); pos != std::string::npos; pos = text.find(needle, pos + 1)) ++count;
  return count;
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

// --- P1-B9: the continuous standby experiment (analysis revision 3) ----------

TEST(CompanionStandbyContract, StandbyFlagIsOneExperimentalEnvOnThePilot) {
  const std::string ini = readAll(fs::path(REPO_ROOT_PATH) / "platformio.ini");
  size_t occurrences = 0;
  for (auto pos = ini.find("-DCROSSPOINT_COMPANION_GATE_STANDBY"); pos != std::string::npos;
       pos = ini.find("-DCROSSPOINT_COMPANION_GATE_STANDBY", pos + 1)) {
    ++occurrences;
  }
  EXPECT_EQ(occurrences, 1u) << "the standby flag belongs to exactly one experimental env";
  const auto standbyEnv = ini.find("[env:x4c-bleautols-standby]");
  ASSERT_NE(standbyEnv, std::string::npos);
  const auto extends = ini.find("extends = env:x4c-bleautols-pilot", standbyEnv);
  EXPECT_NE(extends, std::string::npos) << "the standby env inherits the accepted pilot configuration";
}

// --- P1-B9-E2: the standby poll-interval A/B experiment ----------------------
// Placed after standbyLoopSource's definition; see the E2 tests at the end.

std::string standbyLoopSource(const std::string& main) {
  const auto begin = main.find("static void runCompanionGate");
  if (begin == std::string::npos) return "";
  const auto end = main.find("void setupDisplayAndFonts", begin);
  if (end == std::string::npos) return "";
  return stripLineComments(main.substr(begin, end - begin));
}

TEST(CompanionStandbyContract, StandbyReplacesOnlyTheDeadline) {
  const std::string main = readAll(fs::path(REPO_ROOT_PATH) / "src" / "main.cpp");
  const std::string body = standbyLoopSource(main);
  ASSERT_FALSE(body.empty()) << "the gate loop must exist";
  // The deadline is the only structural difference: the same loop, the same
  // poll, no teardown per connection.
  EXPECT_NE(body.find("standbydiag::standbyCapMs()"), std::string::npos);
  const auto guard = body.find("Gate window open");
  ASSERT_NE(guard, std::string::npos);
  // The radio is stopped only on refusal/exit paths, never per connection:
  // radio failure, no bond, USB exit, power abort and the window end.
  EXPECT_EQ(countOccurrences(body, "gate.stop();"), 5u)
      << "the standby loop must not tear the radio down between sessions";
  EXPECT_EQ(countOccurrences(body, "sleepAfterGate();"), 4u)
      << "exits stay button-only deep sleeps; only exits, never sessions";
}

TEST(CompanionStandbyContract, UsbExitFollowsTheAbortShape) {
  const std::string main = readAll(fs::path(REPO_ROOT_PATH) / "src" / "main.cpp");
  const std::string body = standbyLoopSource(main);
  ASSERT_FALSE(body.empty());
  const auto usbCheck = body.find("gpio.isUsbConnected()");
  ASSERT_NE(usbCheck, std::string::npos) << "the approved USB-plug exit must be checked in the loop";
  const auto noteExit = body.find("standbydiag::noteExit(standbydiag::ExitKind::Usb);", usbCheck);
  ASSERT_NE(noteExit, std::string::npos);
  const auto stop = body.find("gate.stop();", noteExit);
  ASSERT_NE(stop, std::string::npos);
  EXPECT_LT(noteExit, stop)
      << "the exit is recorded first, so the final PM capture runs while the Guard's light sleep is still on";
  const auto branchReturn = body.find("return;", stop);
  ASSERT_NE(branchReturn, std::string::npos);
  const std::string usbBranch = body.substr(usbCheck, branchReturn - usbCheck);
  EXPECT_EQ(usbBranch.find("sleepAfterGate();"), std::string::npos)
      << "the USB exit returns into the ordinary boot; it never sleeps";
}

TEST(CompanionStandbyContract, CapExitRecordsThenSleepsWithoutAnyReadback) {
  const std::string main = readAll(fs::path(REPO_ROOT_PATH) / "src" / "main.cpp");
  const std::string body = standbyLoopSource(main);
  ASSERT_FALSE(body.empty());
  const auto capNote = body.find("standbydiag::ExitKind::Cap");
  ASSERT_NE(capNote, std::string::npos) << "the 24 h diagnostic cap must be classified at its exit";
  const auto sleep = body.find("sleepAfterGate();", capNote);
  ASSERT_NE(sleep, std::string::npos);
  EXPECT_LT(capNote, sleep) << "the cap exit is recorded before the non-returning sleep";
  // The cap path must not attempt any console readback: the evidence is
  // discarded there by design (deep sleep), and the verdict rule handles the
  // absent evidence as INCONCLUSIVE.
  EXPECT_EQ(body.find("SBDS", capNote), std::string::npos);
  EXPECT_EQ(body.find("notifyRetained", capNote), std::string::npos);
}

TEST(CompanionStandbyContract, ReadbackIsOperatorControlledAfterTheOrdinaryBoot) {
  const std::string main = readAll(fs::path(REPO_ROOT_PATH) / "src" / "main.cpp");
  // The readback lives in loop(): a once-per-boot retained-evidence notice
  // and the operator's SBDS serial commands — never inside the headless gate
  // window, and never an automatic dump (C1 blocker 2: the dump must not
  // destroy the only RAM copy).
  const std::string gate = standbyLoopSource(main);
  EXPECT_EQ(gate.find("notifyRetained"), std::string::npos) << "the gate window must not announce or read back";
  EXPECT_EQ(gate.find("handleCommand"), std::string::npos) << "the gate window must not read back";
  const auto notice = main.find("standbydiag::notifyRetained();");
  const auto loopFn = main.find("void loop()");
  ASSERT_NE(notice, std::string::npos);
  ASSERT_NE(loopFn, std::string::npos);
  EXPECT_GT(notice, loopFn) << "the notice must be a loop()-time recovery path";
  const auto command = main.find("standbydiag::handleCommand(cmd.c_str());", loopFn);
  ASSERT_NE(command, std::string::npos) << "the SBDS commands must be dispatched from loop()";
}

TEST(CompanionStandbyContract, GateLogiMapsToNoOpInTheStandbyEnv) {
  const std::string main = readAll(fs::path(REPO_ROOT_PATH) / "src" / "main.cpp");
  const auto guard = main.find("#ifdef CROSSPOINT_COMPANION_GATE_STANDBY");
  ASSERT_NE(guard, std::string::npos);
  const auto noOp = main.find("#define GATE_LOGI(...) ((void)0)", guard);
  ASSERT_NE(noOp, std::string::npos) << "the standby env must compile the routine gate lines out";
  const auto elsePos = main.find("#else", guard);
  ASSERT_NE(elsePos, std::string::npos);
  EXPECT_LT(noOp, elsePos) << "the no-op mapping must be the standby branch, not the fallback";
  const auto logInf = main.find("LOG_INF(\"GATE\", __VA_ARGS__)", elsePos);
  ASSERT_NE(logInf, std::string::npos) << "the non-standby envs must keep the ordinary log line";
}

TEST(CompanionStandbyContract, StandbyGatePathEmitsNoRoutineConsoleLines) {
  const std::string main = readAll(fs::path(REPO_ROOT_PATH) / "src" / "main.cpp");
  const std::string body = standbyLoopSource(main);
  ASSERT_FALSE(body.empty());
  // The zero-console rule (analysis revision 3 §5.5): every routine GATE log
  // line is compiled out of the standby env through GATE_LOGI; only the
  // radio-start LOG_ERR failure signal remains.
  EXPECT_EQ(countOccurrences(body, "LOG_INF(\"GATE\""), 0u) << "routine gate lines must be GATE_LOGI";
  EXPECT_EQ(countOccurrences(body, "LOG_ERR(\"GATE\""), 1u) << "only the radio-start failure keeps its error line";
}

TEST(CompanionStandbyContract, GuardExitLinesAreSuppressedInTheStandbyEnv) {
  const std::string power = readAll(fs::path(REPO_ROOT_PATH) / "src" / "util" / "CompanionGatePower.cpp");
  const std::string body = functionBody(power, "void Guard::finalize", "void handleCommand");
  ASSERT_FALSE(body.empty());
  const auto guard = body.find("#ifndef CROSSPOINT_COMPANION_GATE_STANDBY");
  ASSERT_NE(guard, std::string::npos) << "the Guard's routine exit lines must be compiled out of the standby env";
  const auto logInf = body.find("LOG_INF", guard);
  ASSERT_NE(logInf, std::string::npos);
  const auto endif = body.find("#endif", logInf);
  ASSERT_NE(endif, std::string::npos) << "the suppression must close before the PM restore";
}

TEST(CompanionStandbyContract, ClockEvidenceIsReadAfterTheRadioIsUp) {
  const std::string main = readAll(fs::path(REPO_ROOT_PATH) / "src" / "main.cpp");
  const std::string body = standbyLoopSource(main);
  ASSERT_FALSE(body.empty());
  const auto begin = body.find("gate.begin(");
  const auto sessionBegin = body.find("standbydiag::sessionBegin(millis())");
  ASSERT_NE(begin, std::string::npos);
  ASSERT_NE(sessionBegin, std::string::npos);
  EXPECT_LT(begin, sessionBegin) << "the LPCLK read must follow controller initialization";
}

// --- P1-B9-E2: the standby poll-interval A/B experiment ----------------------

// Ini text with the ';' comment lines removed, so documentation comments can
// never satisfy or break a code contract below.
std::string stripIniComments(const std::string& ini) {
  std::string code;
  size_t start = 0;
  while (start <= ini.size()) {
    const auto end = ini.find('\n', start);
    const auto lineEnd = end == std::string::npos ? ini.size() : end;
    if (ini[start] != ';') code.append(ini, start, lineEnd - start + 1);
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return code;
}

TEST(CompanionStandbyContract, Poll150OverridesThePeriodInExactlyOneEnv) {
  const std::string ini = stripIniComments(readAll(fs::path(REPO_ROOT_PATH) / "platformio.ini"));
  size_t occurrences = 0;
  for (auto pos = ini.find("-DCROSSPOINT_COMPANION_GATE_POLL_MS"); pos != std::string::npos;
       pos = ini.find("-DCROSSPOINT_COMPANION_GATE_POLL_MS", pos + 1)) {
    ++occurrences;
  }
  EXPECT_EQ(occurrences, 1u) << "the poll override belongs to exactly one experimental env";
  const auto env = ini.find("[env:x4c-bleautols-standby-poll150]");
  ASSERT_NE(env, std::string::npos);
  const auto define = ini.find("-DCROSSPOINT_COMPANION_GATE_POLL_MS=150", env);
  EXPECT_NE(define, std::string::npos) << "the experimental env carries the 150 ms configuration";
}

TEST(CompanionStandbyContract, Poll150InheritsTheStandbyConfiguration) {
  const std::string ini = stripIniComments(readAll(fs::path(REPO_ROOT_PATH) / "platformio.ini"));
  const auto env = ini.find("[env:x4c-bleautols-standby-poll150]");
  ASSERT_NE(env, std::string::npos);
  const auto extends = ini.find("extends = env:x4c-bleautols-standby", env);
  EXPECT_NE(extends, std::string::npos) << "the A/B env must inherit the accepted standby configuration";
  const auto flags = ini.find("${env:x4c-bleautols-standby.build_flags}", extends);
  EXPECT_NE(flags, std::string::npos) << "the inherited flags must be referenced, not duplicated";
}

TEST(CompanionStandbyContract, ControlStandbyEnvKeepsTheDefaultPeriod) {
  const std::string ini = stripIniComments(readAll(fs::path(REPO_ROOT_PATH) / "platformio.ini"));
  const auto standby = ini.find("[env:x4c-bleautols-standby]");
  const auto poll150 = ini.find("[env:x4c-bleautols-standby-poll150]");
  ASSERT_NE(standby, std::string::npos);
  ASSERT_NE(poll150, std::string::npos);
  EXPECT_LT(standby, poll150) << "the control env must stay before the experimental env";
  // The control env's own text (up to the experimental env's header) must
  // carry no poll override: it keeps the 15 ms default.
  const std::string controlEnv = ini.substr(standby, poll150 - standby);
  EXPECT_EQ(controlEnv.find("CROSSPOINT_COMPANION_GATE_POLL_MS"), std::string::npos)
      << "the control standby env must keep the 15 ms default (no override)";
}

TEST(CompanionStandbyContract, PollPeriodOverrideReachesTheGatePollOnly) {
  // The mechanism is the single compile-time parameter in CompanionGatePower.h;
  // the gate loop's poll call site must stay the unmodified shared one.
  const std::string header = readAll(fs::path(REPO_ROOT_PATH) / "src" / "util" / "CompanionGatePower.h");
  const auto guard = header.find("#ifdef CROSSPOINT_COMPANION_GATE_POLL_MS");
  ASSERT_NE(guard, std::string::npos) << "the period must be overridable at compile time";
  const auto fallback = header.find("inline constexpr unsigned kAutolsPollMs = 15;", guard);
  EXPECT_NE(fallback, std::string::npos) << "builds without the override keep the 15 ms default";
  const std::string main = readAll(fs::path(REPO_ROOT_PATH) / "src" / "main.cpp");
  const std::string body = standbyLoopSource(main);
  ASSERT_FALSE(body.empty());
  const auto poll = body.find("delay(companiongatepower::gatePollMs(gateWindow.configured()))");
  ASSERT_NE(poll, std::string::npos) << "the gate loop must poll through the shared period helper";
  EXPECT_EQ(countOccurrences(body, "delay("), 1u) << "the poll period stays the loop's only delay";
}
