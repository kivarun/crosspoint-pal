// The sleep companion gate's contract, enforced on the source text of
// src/main.cpp (p1-companion-gate preflight, Slice A):
// - the gate block never returns into loop() (it sleeps in place or falls
//   through to the ordinary boot);
// - the gate polls the physical power-button passthrough and never touches
//   the update()-sampled input state;
// - the gate never reads SETTINGS or mounts SD inside the window;
// - the radio is never begun without a validated identity, and no exit
//   re-arms a timer.

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

// Extracts the text between two marker comments, empty when the markers are
// missing (the caller asserts on the content). Line comments are stripped so
// prose inside the code (e.g. "must never write SETTINGS") cannot satisfy a
// code pin.
std::string between(const std::string& all, const std::string& beginMarker, const std::string& endMarker) {
  const auto begin = all.find(beginMarker);
  if (begin == std::string::npos) return "";
  const auto end = all.find(endMarker, begin);
  if (end == std::string::npos) return "";
  return stripLineComments(all.substr(begin, end - begin));
}

std::string gateWindowSource(const std::string& main) {
  const auto begin = main.find("static void runCompanionGate");
  if (begin == std::string::npos) return "";
  const auto end = main.find("void setup()", begin);
  if (end == std::string::npos) return "";
  return stripLineComments(main.substr(begin, end - begin));
}

}  // namespace

TEST(CompanionGateContract, GateBlockIsMarkedAndBounded) {
  const std::string main = readAll(fs::path(REPO_ROOT_PATH) / "src" / "main.cpp");
  const std::string block = between(main, "// companion-gate-begin", "// companion-gate-end");
  EXPECT_NE(block.find("companiongate::dispatch"), std::string::npos) << "the block must dispatch through the policy";
  EXPECT_NE(block.find("runCompanionGate"), std::string::npos) << "the block must run the window";
  // No bare return: the block's only exits are the sleep call (never returns)
  // and the fall-through into the ordinary boot flow.
  EXPECT_EQ(block.find("return"), std::string::npos) << "the gate block must not return out of setup()";
}

TEST(CompanionGateContract, GateWindowPollsThePhysicalPowerButton) {
  const std::string window = gateWindowSource(readAll(fs::path(REPO_ROOT_PATH) / "src" / "main.cpp"));
  ASSERT_NE(window.find("isPowerButtonPhysicallyPressed"), std::string::npos)
      << "the window must poll the physical power-button read";
  EXPECT_EQ(window.find("gpio.isPressed"), std::string::npos)
      << "isPressed reads the cached update() state and is useless here";
  EXPECT_EQ(window.find("rawInputActive"), std::string::npos)
      << "rawInputActive ORs in board-conditional buttons; never a Power equivalent";
  EXPECT_EQ(window.find("gpio.update()"), std::string::npos) << "update() would fabricate edges into the ordinary boot";
}

TEST(CompanionGateContract, GateWindowPersistsNothingAndNeverReArms) {
  const std::string window = gateWindowSource(readAll(fs::path(REPO_ROOT_PATH) / "src" / "main.cpp"));
  ASSERT_FALSE(window.empty()) << "the gate window function must exist";

  // No SETTINGS access: the SD is not mounted inside the window.
  EXPECT_EQ(window.find("SETTINGS"), std::string::npos) << "the headless window must not touch SETTINGS";

  // Identity arrives from the mirror, validated before the radio starts.
  const auto loadPos = window.find("companionidentity::load(");
  const auto beginPos = window.find("gate.begin(");
  ASSERT_NE(loadPos, std::string::npos);
  ASSERT_NE(beginPos, std::string::npos);
  EXPECT_LT(loadPos, beginPos) << "the identity must be loaded before begin()";

  // Every exit sleeps power-button-only through the shared helper: no timer
  // re-arm, and never the full hardware sleep path (no display/SD here).
  size_t exits = 0;
  for (auto pos = window.find("sleepAfterGate();"); pos != std::string::npos;
       pos = window.find("sleepAfterGate();", pos + 1)) {
    ++exits;
  }
  EXPECT_GE(exits, 3u) << "the deterministic sleep exits must go through the shared helper";
  EXPECT_EQ(window.find("enterHardwareDeepSleep"), std::string::npos)
      << "the window must not run the full hardware sleep path";
}

TEST(CompanionGateContract, MirrorBondCheckGuardsTheWindow) {
  const std::string window = gateWindowSource(readAll(fs::path(REPO_ROOT_PATH) / "src" / "main.cpp"));
  ASSERT_FALSE(window.empty());
  // mirror ∩ bond must gate the window before any session work.
  const auto bondPos = window.find("storedIdentityBonded()");
  const auto openPos = window.find("Gate window open");
  ASSERT_NE(bondPos, std::string::npos);
  ASSERT_NE(openPos, std::string::npos);
  EXPECT_LT(bondPos, openPos) << "the bond check must precede the productive window";
}

TEST(CompanionGateContract, SleepNoRadioGoesStraightBackToSleep) {
  const std::string main = readAll(fs::path(REPO_ROOT_PATH) / "src" / "main.cpp");
  const std::string block = between(main, "// companion-gate-begin", "// companion-gate-end");
  const auto sleepPos = block.find("Action::SleepNoRadio");
  ASSERT_NE(sleepPos, std::string::npos) << "the caller must implement the SleepNoRadio contract";
  const auto nextElse = block.find("else if", sleepPos);
  const std::string branch =
      block.substr(sleepPos, nextElse == std::string::npos ? std::string::npos : nextElse - sleepPos);
  EXPECT_NE(branch.find("companiongate::clearArm()"), std::string::npos) << "the arm must be consumed before the sleep";
  EXPECT_LT(branch.find("clearArm"), branch.find("sleepAfterGate();"))
      << "the arm must be consumed before the sleep, not after";
  EXPECT_NE(branch.find("sleepAfterGate();"), std::string::npos)
      << "an unsafe armed wake must sleep, not continue the ordinary boot";
  EXPECT_EQ(branch.find("enterHardwareDeepSleep"), std::string::npos)
      << "the pre-Storage sleep shape must stay the shared helper";
}

TEST(CompanionGateContract, DispatchDecisionIsLoggedWithItsInputs) {
  const std::string main = readAll(fs::path(REPO_ROOT_PATH) / "src" / "main.cpp");
  const std::string block = between(main, "// companion-gate-begin", "// companion-gate-end");
  EXPECT_NE(block.find("LOG_INF(\"GATE\", \"dispatch:"), std::string::npos)
      << "every wake must log the dispatch decision";
  EXPECT_NE(block.find("gateActionName(gateAction)"), std::string::npos)
      << "the logged decision must be the named action";
}

TEST(CompanionGateContract, EveryGateExitNamesItsCause) {
  const std::string window = gateWindowSource(readAll(fs::path(REPO_ROOT_PATH) / "src" / "main.cpp"));
  ASSERT_FALSE(window.empty());
  // Each deterministic exit of the window must be distinguishable in the log.
  EXPECT_NE(window.find("No stored companion identity"), std::string::npos);
  EXPECT_NE(window.find("The companion radio did not start"), std::string::npos);
  EXPECT_NE(window.find("No secure bond for the stored identity"), std::string::npos);
  EXPECT_NE(window.find("Power pressed: aborting the gate window"), std::string::npos);
  EXPECT_NE(window.find("Power pressed at the sleep handoff"), std::string::npos);
  EXPECT_NE(window.find("Gate window closed"), std::string::npos);
}

size_t countOccurrences(const std::string& text, const std::string& needle) {
  size_t count = 0;
  for (auto pos = text.find(needle); pos != std::string::npos; pos = text.find(needle, pos + 1)) ++count;
  return count;
}

// The dispatch verdict must be stored in every dispatch branch, and only an
// armed timer wake may store it in the fall-through branch: a button or USB
// wake, and an unarmed timer wake (e.g. the sleep-screen slideshow), must
// never overwrite the preserved verdict of the last armed gate.
TEST(CompanionGateContract, DispatchVerdictStoredOnTimerWakesOnly) {
  const std::string main = readAll(fs::path(REPO_ROOT_PATH) / "src" / "main.cpp");
  const std::string block = between(main, "// companion-gate-begin", "// companion-gate-end");
  EXPECT_EQ(countOccurrences(block, "companiongate::storeDispatchVerdict("), 3u)
      << "Run, SleepNoRadio, and the fall-through branch must each store the verdict";
  const auto sleepPos = block.find("Action::SleepNoRadio");
  ASSERT_NE(sleepPos, std::string::npos);
  const auto sleepCall = block.find("sleepAfterGate();", sleepPos);
  ASSERT_NE(sleepCall, std::string::npos);
  EXPECT_LT(block.find("storeDispatchVerdict", sleepPos), sleepCall)
      << "the SleepNoRadio verdict must be stored before the sleep";
  const auto fallThrough = block.rfind("} else {");
  ASSERT_NE(fallThrough, std::string::npos);
  const auto armedGuard = block.find("gateArmed &&", fallThrough);
  ASSERT_NE(armedGuard, std::string::npos);
  const auto guardPos = block.find("gateWake == companiongate::WakeKind::Timer", fallThrough);
  ASSERT_NE(guardPos, std::string::npos);
  const auto storePos = block.find("companiongate::storeDispatchVerdict(", guardPos);
  ASSERT_NE(storePos, std::string::npos);
  EXPECT_LT(armedGuard, guardPos) << "the fall-through store must require an armed wake";
  EXPECT_LT(guardPos, storePos) << "the fall-through store must sit inside the timer-wake guard";
}

// A Run that is refused before or inside the window must remain visible as a
// distinct run outcome, not masked as a completed window.
TEST(CompanionGateContract, RunOutcomeRecordedAtEveryGateExit) {
  const std::string window = gateWindowSource(readAll(fs::path(REPO_ROOT_PATH) / "src" / "main.cpp"));
  ASSERT_FALSE(window.empty());
  EXPECT_GE(countOccurrences(window, "companiongate::updateRunOutcome("), 6u)
      << "every deterministic exit must update the run outcome";
  EXPECT_NE(window.find("RunOutcome::NoIdentity"), std::string::npos);
  EXPECT_NE(window.find("RunOutcome::RadioStartFailed"), std::string::npos);
  EXPECT_NE(window.find("RunOutcome::NoBond"), std::string::npos);
  EXPECT_NE(window.find("RunOutcome::WindowTimeout"), std::string::npos);
  EXPECT_GE(countOccurrences(window, "RunOutcome::PowerAbort"), 2u) << "both power-abort exits must record the outcome";
}

// GATESTAT is the only readback interface for the persisted verdict.
TEST(CompanionGateContract, GatestatExposesThePersistedVerdict) {
  const std::string power = readAll(fs::path(REPO_ROOT_PATH) / "src" / "util" / "CompanionGatePower.cpp");
  EXPECT_NE(power.find("companiongate::readDispatchVerdict"), std::string::npos)
      << "GATESTAT must read the persisted dispatch verdict";
  EXPECT_NE(power.find("last timer-wake dispatch"), std::string::npos)
      << "GATESTAT must name the verdict line distinctly from the window stats";
}

// The window must be finalized explicitly at finish(), before the
// non-returning deep-sleep handoff: a destructor-only finalize loses the
// statistics of every window that ends in sleepAfterGate().
TEST(CompanionGateContract, WindowFinalizesAtFinishBeforeTheSleepHandoff) {
  const std::string power = readAll(fs::path(REPO_ROOT_PATH) / "src" / "util" / "CompanionGatePower.cpp");
  const auto finishBody = power.find("void Guard::finish(const uint8_t outcome) {");
  ASSERT_NE(finishBody, std::string::npos);
  const auto finalizeCall = power.find("finalize();", finishBody);
  ASSERT_NE(finalizeCall, std::string::npos);
  EXPECT_LT(finalizeCall, power.find("}", finishBody)) << "finish() must finalize before returning";
  const auto dtor = power.find("Guard::~Guard() { finalize(); }");
  EXPECT_NE(dtor, std::string::npos) << "the destructor must stay the defensive finalize for returning paths";
  const auto finalizeBody = power.find("void Guard::finalize() {");
  ASSERT_NE(finalizeBody, std::string::npos);
  EXPECT_NE(power.find("if (!configured_ || finalized_) return;", finalizeBody), std::string::npos)
      << "finalization must be idempotent";
  EXPECT_NE(power.find("const bool deltaValid = entrySampled_ && sampled && windowDelta(start_, end, delta);"),
            std::string::npos)
      << "a window's delta needs an entry sample, an exit sample, and monotonic counters";
  EXPECT_NE(
      power.find("const bool counted = windowCounts(kGateLightSleepEnabled, entrySampled_, sampled, deltaValid);"),
      std::string::npos)
      << "counting goes through windowCounts: a control window (light sleep off) and a metrics failure never "
         "accumulate";
  const std::string window = gateWindowSource(readAll(fs::path(REPO_ROOT_PATH) / "src" / "main.cpp"));
  ASSERT_FALSE(window.empty());
  const auto timeoutFinish = window.find("gateWindow.finish(companiongatepower::WindowTimeout);");
  ASSERT_NE(timeoutFinish, std::string::npos);
  const auto handoff = window.find("sleepAfterGate();", timeoutFinish);
  ASSERT_NE(handoff, std::string::npos);
  EXPECT_LT(timeoutFinish, handoff) << "the timeout window must be finalized before the non-returning sleep";
}

// The refusal exits run before the window opens: no identity, radio failure,
// or missing bond must never create a Guard or touch the window accounting.
TEST(CompanionGateContract, RefusalExitsPrecedeTheWindowGuard) {
  const std::string window = gateWindowSource(readAll(fs::path(REPO_ROOT_PATH) / "src" / "main.cpp"));
  ASSERT_FALSE(window.empty());
  const auto identityLoad = window.find("companionidentity::load(");
  const auto bondCheck = window.find("gateMayProceed(");
  const auto guardCtor = window.find("companiongatepower::Guard gateWindow");
  ASSERT_NE(identityLoad, std::string::npos);
  ASSERT_NE(bondCheck, std::string::npos);
  ASSERT_NE(guardCtor, std::string::npos);
  EXPECT_LT(identityLoad, guardCtor);
  EXPECT_LT(bondCheck, guardCtor);
}
