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
                                    "CompanionSession.h", "CompanionPeer.h",        "ConnectionHandle.h",
                                    "PoisonedHandles.h"};
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

TEST(BleCompanionSourceContractTest, ShortBackForwardsPairLongBackExitsAndPowerIsStock) {
  const std::string activity =
      readAll(fs::path(REPO_ROOT_PATH) / "src" / "activities" / "network" / "CompanionActivity.cpp");
  // The accepted F4 contract: a short Back press reaches the phone as one
  // coherent pair sent on its release, a long Back hold exits the session
  // with no Back edge sent at all, and Power keeps the firmware's stock
  // semantics (main.cpp's sleep/refresh handling owns it; the companion must
  // never intercept it).
  const size_t forwardedStart = activity.find("const MappedInputManager::Button FORWARDED[]");
  ASSERT_NE(forwardedStart, std::string::npos);
  const size_t forwardedEnd = activity.find("};", forwardedStart);
  ASSERT_NE(forwardedEnd, std::string::npos);
  const std::string forwarded = activity.substr(forwardedStart, forwardedEnd - forwardedStart);
  EXPECT_NE(forwarded.find("Button::Back"), std::string::npos)
      << "Back must stay listed: the array order is the protocol's wire value order";
  const size_t loopStart = activity.find("void CompanionActivity::loop(");
  ASSERT_NE(loopStart, std::string::npos);
  const size_t loopEnd = activity.find("void CompanionActivity::render(", loopStart);
  ASSERT_NE(loopEnd, std::string::npos);
  const std::string loopBody = activity.substr(loopStart, loopEnd - loopStart);
  // The generic edge loop must not send the Back press: the hold-aware path
  // owns Back, and a press sent before the long/short decision could leave
  // the phone with a dangling pressed=true after a hold exit.
  EXPECT_NE(loopBody.find("if (b == MappedInputManager::Button::Back) continue;"), std::string::npos)
      << "the generic forwarding loop must skip Back (the hold-aware path owns it)";
  const size_t longBack = loopBody.find("wasLongPressed(MappedInputManager::Button::Back, EXIT_HOLD_MS)");
  ASSERT_NE(longBack, std::string::npos) << "a held Back must exit the session";
  const size_t finishAfterLongBack = loopBody.find("Activity::finish();", longBack);
  ASSERT_NE(finishAfterLongBack, std::string::npos)
      << "the long-Back exit must finish the activity (onExit runs the BLE teardown)";
  const size_t shortBack = loopBody.find("wasReleased(MappedInputManager::Button::Back)");
  ASSERT_NE(shortBack, std::string::npos) << "a short Back must be forwarded on its release";
  const size_t pressPair = loopBody.find("ble_.sendButtonEvent(backWire, true);", shortBack);
  ASSERT_NE(pressPair, std::string::npos);
  const size_t releasePair = loopBody.find("ble_.sendButtonEvent(backWire, false);", pressPair);
  ASSERT_NE(releasePair, std::string::npos)
      << "the short Back must send one coherent press/release pair, in that order";
  EXPECT_EQ(activity.find("MappedInputManager::Button::Power"), std::string::npos)
      << "Power is stock firmware behavior: the companion must never intercept it";
  EXPECT_NE(activity.find("constexpr unsigned long EXIT_HOLD_MS = 1500;"), std::string::npos)
      << "the Back exit hold threshold is 1500ms";
  EXPECT_NE(activity.find("constexpr unsigned long PAIRING_RESET_HOLD_MS = 1500;"), std::string::npos)
      << "the Down/Confirm pairing reset hold stays at 1500ms";
  // The hint binds to the physical Back key: mapLabels() pins its first
  // argument under the Back key, and drawButtonHints renders it there (a
  // too-wide label wraps to two lines inside the slot). The F3 standalone
  // instruction row is gone.
  const size_t renderStart = activity.find("void CompanionActivity::render(");
  ASSERT_NE(renderStart, std::string::npos);
  const size_t renderEnd = activity.find("renderer.displayBuffer();", renderStart);
  ASSERT_NE(renderEnd, std::string::npos);
  const std::string renderBody = activity.substr(renderStart, renderEnd - renderStart);
  EXPECT_NE(renderBody.find("mapLabels(tr(STR_COMPANION_EXIT_HINT), \"\", \"\", \"\")"), std::string::npos)
      << "the exit hint must be the first mapLabels() argument (the physical Back slot)";
  EXPECT_NE(renderBody.find("GUI.drawButtonHints("), std::string::npos);
  EXPECT_EQ(renderBody.find("instructionY"), std::string::npos)
      << "the F3 standalone instruction row was replaced by the Back-slot hint";
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
  // The fail-closed path itself: discard queued entries, terminate every
  // connected peer (pre-auth links included) and poison its handle, clear
  // the session, emit nothing, and claim no product state — the teardown
  // completes only on each Disconnected entry, which resumes advertising.
  const size_t failStart = adapter.find("void overflowFailClosed()");
  ASSERT_NE(failStart, std::string::npos);
  const size_t failEnd = adapter.find("bool resumeAdvertising()", failStart);
  ASSERT_NE(failEnd, std::string::npos);
  const std::string failClosed = adapter.substr(failStart, failEnd - failStart);
  EXPECT_NE(failClosed.find("queueHead_ = queueTail_"), std::string::npos)
      << "remaining queued entries must be discarded";
  EXPECT_NE(failClosed.find("getPeerDevices()"), std::string::npos)
      << "connected peers must be enumerated by real handle (getPeerInfo(i) misresolves around freed slots)";
  EXPECT_NE(failClosed.find("poisonedHandles_.forgetNotIn("), std::string::npos)
      << "poisoned handles whose teardown already completed must be released";
  EXPECT_NE(failClosed.find("poisonedHandles_.poison("), std::string::npos)
      << "terminated handles must stay poisoned until their disconnect lands";
  EXPECT_NE(failClosed.find("disconnect(handle)"), std::string::npos) << "each connected peer must be forced down";
  EXPECT_NE(failClosed.find("LOG_ERR"), std::string::npos) << "a fail-close must be diagnosable from serial logs";
  const size_t emptyPeers = failClosed.find("peers.empty()");
  ASSERT_NE(emptyPeers, std::string::npos);
  const size_t failResume = failClosed.find("resumeAdvertising()");
  ASSERT_NE(failResume, std::string::npos);
  EXPECT_LT(emptyPeers, failResume)
      << "with zero connected peers no Disconnected entry will arrive: the fail-close owns the resume";
  EXPECT_NE(failClosed.find("sessionHandle_ = kNoSessionHandle"), std::string::npos) << "the session must be cleared";
  EXPECT_EQ(failClosed.find("setState("), std::string::npos)
      << "fail-close must not claim a state; the Disconnected path resumes advertising";
  EXPECT_EQ(failClosed.find("notifyTo("), std::string::npos) << "no protocol egress on overflow";
  // Dispatch ordering: a Disconnected entry releases its poisoned handle
  // before normal handling; every other entry from a dying peer is skipped.
  EXPECT_NE(loopBody.find("poisonedHandles_.forget(entry.connHandle);"), std::string::npos)
      << "the disconnect lifecycle must release the poisoned handle";
  EXPECT_NE(loopBody.find("poisonedHandles_.poisoned(entry.connHandle)"), std::string::npos)
      << "a dying peer's events must be skipped before product logic";
}

TEST(BleCompanionSourceContractTest, AdvertisingResumesOnlyWhenStarted) {
  const std::string adapter = readAll(fs::path(BLECOMPANION_ROOT_PATH) / "src" / "BleCompanion.cpp");
  // handleDisconnected must claim Advertising only through the canonical
  // resume helper, which starts real advertising before settling the state.
  const size_t disconnectedStart = adapter.find("void handleDisconnected(const Entry& entry)");
  ASSERT_NE(disconnectedStart, std::string::npos);
  const size_t disconnectedEnd = adapter.find("void handleControl(const Entry& entry)", disconnectedStart);
  ASSERT_NE(disconnectedEnd, std::string::npos);
  const std::string handleDisconnected = adapter.substr(disconnectedStart, disconnectedEnd - disconnectedStart);
  EXPECT_NE(handleDisconnected.find("resumeAdvertising()"), std::string::npos)
      << "advertising must restart through the canonical helper";
  EXPECT_EQ(handleDisconnected.find("setState(RadioState::Advertising)"), std::string::npos)
      << "the product state must never claim Advertising without a real start";
  // A live link supersedes a pending resume: handleConnected must clear the
  // retry flag, or a late success could claim Advertising over a session.
  const size_t connectedStart = adapter.find("void handleConnected(const Entry& entry)");
  ASSERT_NE(connectedStart, std::string::npos);
  const size_t connectedEnd = adapter.find("void handleAuthenticated(const Entry& entry)", connectedStart);
  ASSERT_NE(connectedEnd, std::string::npos);
  EXPECT_NE(adapter.substr(connectedStart, connectedEnd - connectedStart).find("advertisingResumePending_ = false;"),
            std::string::npos)
      << "a live link must supersede a pending advertising resume";
  const size_t resumeStart = adapter.find("bool resumeAdvertising()");
  ASSERT_NE(resumeStart, std::string::npos);
  const size_t resumeEnd = adapter.find("void handleConnected(const Entry& entry)", resumeStart);
  ASSERT_NE(resumeEnd, std::string::npos);
  const std::string resume = adapter.substr(resumeStart, resumeEnd - resumeStart);
  const size_t start = resume.find("advertising->start()");
  ASSERT_NE(start, std::string::npos);
  const size_t claim = resume.find("setState(RadioState::Advertising)");
  ASSERT_NE(claim, std::string::npos);
  EXPECT_LT(start, claim) << "the state is claimed only after advertising actually starts";
  const size_t failure = resume.find("return false;");
  ASSERT_NE(failure, std::string::npos);
  EXPECT_LT(failure, claim) << "a failed restart must return before any state claim";
  EXPECT_NE(resume.find("LOG_ERR"), std::string::npos) << "a failed restart must be reported";
  // A failed restart is retried on later loop passes: the link is gone, so no
  // event will otherwise re-trigger the resume.
  const size_t loopStart = adapter.find("void loop() {");
  ASSERT_NE(loopStart, std::string::npos);
  const size_t loopEnd = adapter.find("Status status() const", loopStart);
  ASSERT_NE(loopEnd, std::string::npos);
  EXPECT_NE(adapter.substr(loopStart, loopEnd - loopStart).find("advertisingResumePending_"), std::string::npos)
      << "a failed advertising resume must be retried by the main loop";
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
