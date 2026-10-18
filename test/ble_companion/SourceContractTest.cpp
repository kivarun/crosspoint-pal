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

const std::set<std::string> kOwn = {"BleCompanion.h",     "BleCompanionProtocol.h", "BleCompanionJson.h",
                                    "CompanionSession.h", "CompanionPeer.h",        "ConnectionHandle.h"};
const std::set<std::string> kStd = {"array", "cstddef", "cstdint", "cstdio", "cstring", "initializer_list", "memory"};
const std::set<std::string> kAdapterOnly = {"NimBLEDevice.h", "esp_random.h", "Memory.h"};

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
  const std::string adapter = readAll(fs::path(BLECOMPANION_ROOT_PATH) / "src" / "BleCompanion.cpp");
  EXPECT_NE(adapter.find("getIdAddress()"), std::string::npos) << "the adapter must capture the bonded ID address";
  EXPECT_EQ(adapter.find("info.getAddress()"), std::string::npos)
      << "the over-the-air address must never take part in identity decisions";
  EXPECT_NE(adapter.find("isBonded()"), std::string::npos)
      << "authentication must require a persistent bond, not a paired-only link";
  EXPECT_NE(adapter.find("sessionBound("), std::string::npos)
      << "control packets must be bound to the accepted companion session";
}

TEST(BleCompanionSourceContractTest, SessionHandleUsesNimBLEsSentinel) {
  const std::string adapter = readAll(fs::path(BLECOMPANION_ROOT_PATH) / "src" / "BleCompanion.cpp");
  EXPECT_NE(adapter.find("kNoSessionHandle == BLE_HS_CONN_HANDLE_NONE"), std::string::npos)
      << "the no-session sentinel must be pinned to NimBLE's BLE_HS_CONN_HANDLE_NONE";
  EXPECT_EQ(adapter.find("sessionHandle_ == 0"), std::string::npos)
      << "connection handle 0 is valid; it must never mean no session";
  EXPECT_EQ(adapter.find("sessionHandle_ != 0"), std::string::npos)
      << "connection handle 0 is valid; it must never mean no session";
  EXPECT_NE(adapter.find("sessionHandle_ = kNoSessionHandle;"), std::string::npos)
      << "cleared sessions must reset to the sentinel";
}

TEST(BleCompanionSourceContractTest, ProtocolEgressIsAlwaysHandleScoped) {
  const std::string adapter = readAll(fs::path(BLECOMPANION_ROOT_PATH) / "src" / "BleCompanion.cpp");
  size_t pos = 0;
  int notifyCalls = 0;
  while ((pos = adapter.find("->notify(", pos)) != std::string::npos) {
    ++notifyCalls;
    pos += 9;
  }
  EXPECT_EQ(notifyCalls, 1) << "every protocol message must go through the handle-scoped notifyTo helper";
  EXPECT_NE(adapter.find("notifyTo(uint16_t handle"), std::string::npos)
      << "egress must be scoped to one connection handle";
  EXPECT_EQ(adapter.find("connHandle = BLE_HS_CONN_HANDLE_NONE"), std::string::npos)
      << "the sentinel must never be used as a broadcast fallback";
}

TEST(BleCompanionSourceContractTest, RenderTaskNeverReadsCompanionState) {
  const std::string activity =
      readAll(fs::path(REPO_ROOT_PATH) / "src" / "activities" / "network" / "CompanionActivity.cpp");
  const size_t renderStart = activity.find("void CompanionActivity::render(");
  ASSERT_NE(renderStart, std::string::npos);
  const size_t renderEnd = activity.find("renderer.displayBuffer();", renderStart);
  ASSERT_NE(renderEnd, std::string::npos);
  const std::string renderBody = activity.substr(renderStart, renderEnd - renderStart);
  EXPECT_EQ(renderBody.find("ble_."), std::string::npos)
      << "render() must read only the published snapshot, never BleCompanion state";
  EXPECT_NE(activity.find("RenderLock::Mode::Try"), std::string::npos)
      << "snapshot publication must take the rendering mutex (try-lock, retried per pass)";
}
TEST(BleCompanionSourceContractTest, IdentityAddressIsNotReadThroughATemporary) {
  const std::string adapter = readAll(fs::path(BLECOMPANION_ROOT_PATH) / "src" / "BleCompanion.cpp");
  // The negatives carry the invariant: chaining an interior-pointer accessor
  // onto the by-value temporary is the hazard; any bound-local spelling of
  // the fix is acceptable.
  EXPECT_EQ(adapter.find("info.getIdAddress().getBase()"), std::string::npos)
      << "getBase() points into a by-value temporary; the value must be bound to a local first";
  EXPECT_EQ(adapter.find("info.getIdAddress().getVal()"), std::string::npos)
      << "getVal() points into a by-value temporary; the value must be bound to a local first";
}

TEST(BleCompanionSourceContractTest, OverflowPoisonsTheQueueBeforeFurtherProcessing) {
  const std::string adapter = readAll(fs::path(BLECOMPANION_ROOT_PATH) / "src" / "BleCompanion.cpp");
  // Drain-loop ordering: the poison is observed before the next entry reaches
  // product logic, so a control entry queued after a dropped lifecycle event
  // can never mutate state or emit egress.
  const size_t loopStart = adapter.find("void loop() {");
  ASSERT_NE(loopStart, std::string::npos);
  const size_t loopEnd = adapter.find("Status status() const", loopStart);
  ASSERT_NE(loopEnd, std::string::npos);
  const std::string loopBody = adapter.substr(loopStart, loopEnd - loopStart);
  const size_t poisonCheck = loopBody.find("takeQueueOverflow()");
  ASSERT_NE(poisonCheck, std::string::npos);
  const size_t dispatch = loopBody.find("switch (entry.kind)");
  ASSERT_NE(dispatch, std::string::npos);
  EXPECT_LT(poisonCheck, dispatch) << "the overflow check must precede entry dispatch";
  EXPECT_NE(loopBody.find("overflowFailClosed();"), std::string::npos)
      << "an observed overflow must fail closed and stop processing";
  EXPECT_NE(loopBody.find("return;"), std::string::npos)
      << "an observed overflow must return from the loop without processing more entries";
  // The fail-closed path itself: discard queued entries, tear the accepted
  // session down, settle in the non-session state, emit nothing.
  const size_t failStart = adapter.find("void overflowFailClosed()");
  ASSERT_NE(failStart, std::string::npos);
  const size_t failEnd = adapter.find("void handleConnected", failStart);
  ASSERT_NE(failEnd, std::string::npos);
  const std::string failClosed = adapter.substr(failStart, failEnd - failStart);
  EXPECT_NE(failClosed.find("queueHead_ = queueTail_"), std::string::npos)
      << "remaining queued entries must be discarded";
  EXPECT_NE(failClosed.find("disconnect(sessionHandle_)"), std::string::npos)
      << "the exact accepted handle must be forced down";
  EXPECT_NE(failClosed.find("sessionHandle_ = kNoSessionHandle"), std::string::npos) << "the session must be cleared";
  EXPECT_NE(failClosed.find("setState(RadioState::Advertising)"), std::string::npos)
      << "product state must settle in the non-session state";
  EXPECT_EQ(failClosed.find("notifyTo("), std::string::npos) << "no protocol egress on overflow";
}

TEST(BleCompanionSourceContractTest, CallbacksAreSingleOwnerNoBareNew) {
  const std::string adapter = readAll(fs::path(BLECOMPANION_ROOT_PATH) / "src" / "BleCompanion.cpp");
  EXPECT_EQ(adapter.find("new ServerCallbacks"), std::string::npos)
      << "callback allocation must use the nothrow ownership pattern";
  EXPECT_EQ(adapter.find("new ControlCallbacks"), std::string::npos)
      << "callback allocation must use the nothrow ownership pattern";
  EXPECT_NE(adapter.find("makeUniqueNoThrow<ServerCallbacks>"), std::string::npos)
      << "callbacks must be allocated no-throw and owned by Impl";
  EXPECT_NE(adapter.find("setCallbacks(serverCallbacks_.get()"), std::string::npos)
      << "NimBLE must not own the server callbacks";
  EXPECT_EQ(adapter.find("serverCallbacks_ = nullptr;"), std::string::npos)
      << "stop() must not release the Impl-owned callbacks before ~Impl";
}
}  // namespace
