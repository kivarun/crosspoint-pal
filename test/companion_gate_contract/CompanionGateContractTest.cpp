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
  EXPECT_EQ(window.find("gpio.update()"), std::string::npos)
      << "update() would fabricate edges into the ordinary boot";
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
  for (auto pos = window.find("sleepAfterGate();"); pos != std::string::npos; pos = window.find("sleepAfterGate();", pos + 1)) {
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
