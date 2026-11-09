// The P1-B9-E3 advertising-interval experiment's contract, enforced on the
// source text of lib/BleCompanion/src/BleCompanion.cpp and platformio.ini,
// plus the compiled ms → BLE-unit conversion:
// - only the experimental environment defines the compile-time interval, and
//   it is exactly 500 ms; the control standby env defines nothing;
// - the interval is applied in begin() before the first advertising start, so
//   it survives NimBLE deinit (which deletes the shared advertising object)
//   and is re-applied on every fresh stack bring-up;
// - resumeAdvertising() reuses the same object without a reset or an interval
//   re-set, so the configured interval persists across resumes;
// - the guarded block touches nothing but the advertising interval: no
//   security, bonding, payload, scan-response or connection-parameter change
//   can hide behind it.

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "CompanionAdvInterval.h"

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
  std::string out;
  for (auto pos = text.find("//"); pos != std::string::npos; pos = text.find("//", pos)) {
    const auto lineEnd = text.find('\n', pos);
    if (lineEnd == std::string::npos) {
      return text.substr(0, pos);
    }
    out += text.substr(0, pos);
    text = text.substr(lineEnd);
  }
  return out + text;
}

// The body between two source markers, empty when a marker is missing (the
// caller asserts on the content). Comments are stripped so prose cannot
// satisfy a code pin.
std::string bodyBetween(const std::string& all, const std::string& beginMarker, const std::string& endMarker) {
  const auto begin = all.find(beginMarker);
  if (begin == std::string::npos) return "";
  const auto end = all.find(endMarker, begin);
  if (end == std::string::npos) return "";
  return stripLineComments(all.substr(begin, end - begin));
}

std::string envBlock(const std::string& all, const std::string& envName) {
  const auto begin = all.find("[" + envName + "]");
  if (begin == std::string::npos) return "";
  const auto end = all.find("\n[", begin + 1);
  const std::string raw = end == std::string::npos ? all.substr(begin) : all.substr(begin, end - begin);
  // INI prose is stripped so a comment mentioning the flag cannot satisfy a
  // code pin.
  std::string out;
  std::string line;
  std::istringstream stream(raw);
  while (std::getline(stream, line)) {
    const auto first = line.find_first_not_of(" \t");
    if (first != std::string::npos && line[first] == ';') continue;
    out += line + "\n";
  }
  return out;
}

size_t countOccurrences(const std::string& text, const std::string& needle) {
  size_t count = 0;
  for (auto pos = text.find(needle); pos != std::string::npos; pos = text.find(needle, pos + 1)) ++count;
  return count;
}

}  // namespace

// The conversion is the one piece of real code this change ships; pin its
// arithmetic and its fit inside the legacy advertising range (20 ms .. 10.24 s).
TEST(CompanionAdvInterval, ConvertsMillisecondsToBleUnits) {
  EXPECT_EQ(blecompanion::advertisingUnitsFromMs(500), 800);
  EXPECT_EQ(blecompanion::advertisingUnitsFromMs(500), 0x320);
  EXPECT_EQ(blecompanion::advertisingUnitsFromMs(100), 160);
  EXPECT_EQ(blecompanion::advertisingUnitsFromMs(30), 48);        // NimBLE default min
  EXPECT_EQ(blecompanion::advertisingUnitsFromMs(60), 96);        // NimBLE default max
  EXPECT_EQ(blecompanion::advertisingUnitsFromMs(20), 32);        // spec minimum
  EXPECT_EQ(blecompanion::advertisingUnitsFromMs(10240), 16384);  // spec maximum
}

// The range check in begin() must see the untruncated quotient: a uint16_t
// truncation first could let an out-of-range value pass with a silently
// wrong interval.
TEST(CompanionAdvInterval, RangeChecksSeeTheUntruncatedQuotient) {
  EXPECT_EQ(blecompanion::advertisingUnitsExact(500), 800u);
  EXPECT_GT(blecompanion::advertisingUnitsExact(41280), 16384u)
      << "an out-of-range value must stay out of range after no truncation";
  EXPECT_LT(blecompanion::advertisingUnitsExact(10), 32u);
}

TEST(CompanionAdvInterval, OnlyTheExperimentalEnvironmentDefinesTheInterval) {
  const std::string ini = readAll(fs::path(REPO_ROOT_PATH) / "platformio.ini");
  const std::string experiment = envBlock(ini, "env:x4c-bleautols-standby-adv500");
  ASSERT_FALSE(experiment.empty()) << "the experimental env must exist";
  EXPECT_NE(experiment.find("-DCROSSPOINT_COMPANION_ADV_INTERVAL_MS=500"), std::string::npos)
      << "the experimental env must set exactly 500 ms";
  EXPECT_NE(experiment.find("extends = env:x4c-bleautols-standby\n"), std::string::npos)
      << "the experiment must inherit the control standby env by reference";
  // Every other environment keeps the library default: the flag appears in
  // exactly one env block and nowhere else in the INI.
  const std::string iniNoBlocks = [&ini] {
    std::string out = ini;
    for (auto pos = out.find("[env:"); pos != std::string::npos;) {
      const auto end = out.find("\n[", pos);
      const auto blockEnd = end == std::string::npos ? out.size() : end;
      out.erase(out.find("[env:", pos), blockEnd - out.find("[env:", pos));
      pos = out.find("[env:", pos);
    }
    return out;
  }();
  EXPECT_EQ(iniNoBlocks.find("CROSSPOINT_COMPANION_ADV_INTERVAL_MS"), std::string::npos)
      << "outside the env blocks the flag must not appear";
  EXPECT_EQ(countOccurrences(ini, "-DCROSSPOINT_COMPANION_ADV_INTERVAL_MS="), 1)
      << "exactly one environment may receive the interval override";
}

TEST(CompanionAdvInterval, ControlStandbyEnvHasNoIntervalOverride) {
  const std::string ini = readAll(fs::path(REPO_ROOT_PATH) / "platformio.ini");
  const std::string control = envBlock(ini, "env:x4c-bleautols-standby");
  ASSERT_FALSE(control.empty());
  EXPECT_EQ(control.find("CROSSPOINT_COMPANION_ADV_INTERVAL_MS"), std::string::npos)
      << "the control firmware must keep the NimBLE default interval";
}

TEST(CompanionAdvInterval, IntervalIsAppliedBeforeTheFirstAdvertisingStart) {
  const std::string source = readAll(fs::path(REPO_ROOT_PATH) / "lib" / "BleCompanion" / "src" / "BleCompanion.cpp");
  // The guarded block sits inside begin(), between the payload configuration
  // and the first advertising start: NimBLE deinit deletes the shared
  // NimBLEAdvertising object, so begin() is the only place a re-created
  // configuration can live.
  const std::string beginBody =
      bodyBetween(source, "bool BleCompanion::Impl::begin(", "void BleCompanion::Impl::stop()");
  ASSERT_FALSE(beginBody.empty()) << "begin() must exist";
  const auto payloadPos = beginBody.find("advertising->enableScanResponse(true)");
  const auto guardPos = beginBody.find("#ifdef CROSSPOINT_COMPANION_ADV_INTERVAL_MS");
  const auto startPos = beginBody.find("advertising->start()");
  ASSERT_NE(payloadPos, std::string::npos);
  ASSERT_NE(guardPos, std::string::npos) << "begin() must carry the compile-time interval guard";
  ASSERT_NE(startPos, std::string::npos);
  EXPECT_LT(payloadPos, guardPos);
  EXPECT_LT(guardPos, startPos) << "the interval must be set before the first advertising start";
  // The first interval call sits inside the guard, before its #endif.
  const auto setterPos = beginBody.find("setAdvertisingInterval");
  const auto guardEndPos = beginBody.find("#endif", guardPos);
  ASSERT_NE(setterPos, std::string::npos);
  ASSERT_NE(guardEndPos, std::string::npos);
  EXPECT_GT(setterPos, guardPos);
  EXPECT_LT(setterPos, guardEndPos) << "the interval call lives inside the guard";
  EXPECT_NE(beginBody.find("advertisingUnitsFromMs(CROSSPOINT_COMPANION_ADV_INTERVAL_MS)"), std::string::npos);
  // Exactly one application site in the whole file, and it is the guarded one.
  EXPECT_EQ(countOccurrences(source, "setAdvertisingInterval"), 1);
}

TEST(CompanionAdvInterval, ResumeAdvertisingKeepsTheConfiguration) {
  const std::string source = readAll(fs::path(REPO_ROOT_PATH) / "lib" / "BleCompanion" / "src" / "BleCompanion.cpp");
  const std::string resume =
      bodyBetween(source, "bool resumeAdvertising() {", "void handleConnected(const Entry& entry) {");
  ASSERT_FALSE(resume.empty()) << "resumeAdvertising() must exist";
  EXPECT_EQ(resume.find("setAdvertisingInterval"), std::string::npos) << "a resume must not reconfigure the interval";
  EXPECT_EQ(resume.find("deinit"), std::string::npos) << "a resume must not tear the stack down";
  // The library's reset() is the only call that clears the stored parameters;
  // the companion must never reach it.
  EXPECT_EQ(source.find("NimBLEAdvertising::reset"), std::string::npos);
  EXPECT_EQ(source.find("advertising->reset()"), std::string::npos);
}

TEST(CompanionAdvInterval, TheGuardedBlockChangesOnlyTheInterval) {
  const std::string source = readAll(fs::path(REPO_ROOT_PATH) / "lib" / "BleCompanion" / "src" / "BleCompanion.cpp");
  const std::string beginBody =
      bodyBetween(source, "bool BleCompanion::Impl::begin(", "void BleCompanion::Impl::stop()");
  ASSERT_FALSE(beginBody.empty());
  const auto guardPos = beginBody.find("#ifdef CROSSPOINT_COMPANION_ADV_INTERVAL_MS");
  const auto endifPos = beginBody.find("#endif", guardPos);
  ASSERT_NE(guardPos, std::string::npos);
  ASSERT_NE(endifPos, std::string::npos);
  const std::string guard = beginBody.substr(guardPos, endifPos - guardPos);
  // Inside the guard: the static asserts and the interval setter, nothing
  // else — no security, bonding, payload, scan-response, tx power or
  // connection parameter may change with the interval.
  EXPECT_EQ(countOccurrences(guard, "advertising->"), 1);
  EXPECT_NE(guard.find("setAdvertisingInterval"), std::string::npos);
  EXPECT_EQ(guard.find("setSecurity"), std::string::npos);
  EXPECT_EQ(guard.find("enableScanResponse"), std::string::npos);
  EXPECT_EQ(guard.find("setName"), std::string::npos);
  EXPECT_EQ(guard.find("addServiceUUID"), std::string::npos);
  // The protocol-facing security configuration stays exactly where it was.
  EXPECT_EQ(countOccurrences(source, "setSecurityAuth(true, true, true)"), 1);
  EXPECT_EQ(countOccurrences(source, "setSecurityPasskey"), 1);
  EXPECT_EQ(countOccurrences(source, "setSecurityIOCap"), 1);
}
