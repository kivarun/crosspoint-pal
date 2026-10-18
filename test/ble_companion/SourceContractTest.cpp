// The companion's contract, enforced on the source text:
// - includes stay in the allow-list (NimBLE only inside BleCompanion.cpp);
// - the whole bond store is never deleted (forget deletes one identity);
// - no persistence/file I/O in the module (the host owns SETTINGS);
// - identity decisions use the bonded ID address, never the OTA address.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <string>

namespace {

namespace fs = std::filesystem;

const std::set<std::string> kOwn = {"BleCompanion.h", "BleCompanionProtocol.h", "BleCompanionJson.h",
                                    "CompanionSession.h", "CompanionPeer.h"};
const std::set<std::string> kStd = {"array", "cstddef", "cstdint", "cstdio", "cstring", "initializer_list"};
const std::set<std::string> kAdapterOnly = {"NimBLEDevice.h", "esp_random.h"};

std::string readAll(const fs::path& path) {
  std::ifstream in(path);
  std::string all;
  std::string line;
  while (std::getline(in, line)) all += line + "\n";
  return all;
}

TEST(BleCompanionSourceContractTest, IncludesStayInsideTheAllowList) {
  const fs::path root = BLECOMPANION_ROOT_PATH;
  const std::regex include(R"(^\s*#\s*include\s*[<"]([^>"]+)[>"])");
  std::string outside;
  unsigned files = 0;
  for (const char* dir : {"src", "include"}) {
    for (const auto& entry : fs::directory_iterator(root / dir)) {
      if (!entry.is_regular_file()) continue;
      ++files;
      std::ifstream in(entry.path());
      std::string line;
      while (std::getline(in, line)) {
        std::smatch m;
        if (!std::regex_search(line, m, include)) continue;
        const std::string name = m[1];
        const std::string file = entry.path().filename().string();
        if (kOwn.count(name) || kStd.count(name)) continue;
        if (kAdapterOnly.count(name) && file == "BleCompanion.cpp") continue;
        if (name == "BoardConfig.h") continue;  // the capability gate, in the .cpp and the header callers
        if (name == "Logging.h") continue;      // firmware logging facade, no product coupling
        outside += file + ": " + name + "\n";
      }
    }
  }
  EXPECT_GE(files, 6u) << "the scan found the module";
  EXPECT_EQ(outside, "") << "includes outside the allow-list:\n" << outside;
}

TEST(BleCompanionSourceContractTest, BondStoreIsNeverDeletedWholesale) {
  const fs::path root = BLECOMPANION_ROOT_PATH;
  for (const char* dir : {"src", "include"}) {
    for (const auto& entry : fs::directory_iterator(root / dir)) {
      if (!entry.is_regular_file()) continue;
      const std::string text = readAll(entry.path());
      EXPECT_EQ(text.find("deleteAllBonds"), std::string::npos)
          << entry.path().filename().string() << " deletes the whole bond store";
      EXPECT_EQ(text.find("ble_store_clear"), std::string::npos)
          << entry.path().filename().string() << " clears the whole bond store";
    }
  }
}

TEST(BleCompanionSourceContractTest, NoPersistenceInsideTheModule) {
  const fs::path root = BLECOMPANION_ROOT_PATH;
  for (const char* dir : {"src", "include"}) {
    for (const auto& entry : fs::directory_iterator(root / dir)) {
      if (!entry.is_regular_file()) continue;
      const std::string text = readAll(entry.path());
      EXPECT_EQ(text.find("saveToFile"), std::string::npos)
          << entry.path().filename().string() << " persists settings from inside the module";
      EXPECT_EQ(text.find("SETTINGS"), std::string::npos)
          << entry.path().filename().string() << " reaches into host settings";
    }
  }
}

TEST(BleCompanionSourceContractTest, IdentityUsesTheBondedIdAddress) {
  const std::string adapter =
      readAll(fs::path(BLECOMPANION_ROOT_PATH) / "src" / "BleCompanion.cpp");
  EXPECT_NE(adapter.find("getIdAddress()"), std::string::npos)
      << "the adapter must capture the bonded ID address";
  EXPECT_EQ(adapter.find("info.getAddress()"), std::string::npos)
      << "the over-the-air address must never take part in identity decisions";
  // The identity policy runs on the main loop, not inside a NimBLE callback:
  // the callback captures and enqueues only.
  const size_t callbackBlock = adapter.find("class ServerCallbacks final");
  const size_t policy = adapter.find("CompanionSession::authenticate");
  ASSERT_NE(callbackBlock, std::string::npos);
  ASSERT_NE(policy, std::string::npos);
  EXPECT_LT(policy, callbackBlock) << "identity policy must not live inside the callback classes";
}

}  // namespace
