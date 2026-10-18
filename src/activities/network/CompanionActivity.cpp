#include "CompanionActivity.h"

#include <BlePageTurner.h>
#include <HalMemory.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>

#include "components/UITheme.h"
#include "fontIds.h"

namespace {

constexpr unsigned long PAIRING_RESET_HOLD_MS = 1500;

// The input edges the session forwards, in MappedInputManager order. The
// list order is also the protocol's wire value order (Button enum in
// BleCompanionJson.h, mapped by index in protocolButton): reordering any of
// the three changes the values the phone receives.
const MappedInputManager::Button FORWARDED[] = {
    MappedInputManager::Button::Back,  MappedInputManager::Button::Confirm, MappedInputManager::Button::Left,
    MappedInputManager::Button::Right, MappedInputManager::Button::Up,      MappedInputManager::Button::Down,
};

}  // namespace

bool CompanionActivity::protocolButton(const MappedInputManager::Button button, uint8_t& out) {
  for (size_t i = 0; i < sizeof(FORWARDED) / sizeof(FORWARDED[0]); i++) {
    if (FORWARDED[i] == button) {
      out = static_cast<uint8_t>(i);
      return true;
    }
  }
  return false;
}

void CompanionActivity::logHeap(const char* when) {
  const auto heap = HalMemory::getInternalHeap();
  LOG_INF("BLECOMP", "Heap %s: free %u, largest %u", when, static_cast<unsigned>(heap.freeBytes),
          static_cast<unsigned>(heap.largestBlockBytes));
}

// Main-task only: deep-copies the session state into the pending snapshot.
// The BleCompanion's own storage is never handed to the render task.
void CompanionActivity::buildSnapshot(const blecompanion::Status& status) {
  pendingSnapshot_.compiledIn = status.compiledIn;
  pendingSnapshot_.state = status.state;
  pendingSnapshot_.sessionActive = status.sessionActive;
  pendingSnapshot_.contentPresent = status.contentPresent;
  pendingSnapshot_.passkey = status.passkey;
  pendingSnapshot_.revision = status.revision;
  if (status.contentPresent) {
    const blecompanion::ContentView view = ble_.content();
    strncpy(pendingSnapshot_.title, view.title != nullptr ? view.title : "", sizeof(pendingSnapshot_.title) - 1);
    pendingSnapshot_.title[sizeof(pendingSnapshot_.title) - 1] = '\0';
    strncpy(pendingSnapshot_.body, view.body != nullptr ? view.body : "", sizeof(pendingSnapshot_.body) - 1);
    pendingSnapshot_.body[sizeof(pendingSnapshot_.body) - 1] = '\0';
  } else {
    pendingSnapshot_.title[0] = '\0';
    pendingSnapshot_.body[0] = '\0';
  }
  snapshotDirty_ = true;
}

// Publication takes the same rendering mutex the render task holds for the
// whole paint — with a try-lock, so the main loop never stalls behind a long
// e-ink refresh; the retry happens on the next loop pass.
bool CompanionActivity::publishSnapshot() {
  if (!snapshotDirty_) return false;
  RenderLock lock(RenderLock::Mode::Try);
  if (!lock.ownsLock()) return false;
  snapshot_ = pendingSnapshot_;
  snapshotDirty_ = false;
  return true;
}

void CompanionActivity::beginSession() {
  heapBeforeBegin_ = HalMemory::getInternalHeap().freeBytes;
  largestBeforeBegin_ = HalMemory::getInternalHeap().largestBlockBytes;
  // One radio owner: the page turner must be off before the companion starts,
  // and its own policy resumes when this activity leaves.
  if (!bleturner::stopNow(1000)) {
    LOG_ERR("BLECOMP", "Could not stop the BLE page turner; the companion radio stays off");
  } else {
    // Persistence callbacks run on the main loop (the adapter defers them
    // there), so touching SETTINGS here is safe.
    const bool started = ble_.begin(
        SETTINGS.companionPeerAddr, SETTINGS.companionPeerAddrType,
        [](const char* addr, uint8_t type) {
          strncpy(SETTINGS.companionPeerAddr, addr, sizeof(SETTINGS.companionPeerAddr) - 1);
          SETTINGS.companionPeerAddr[sizeof(SETTINGS.companionPeerAddr) - 1] = '\0';
          SETTINGS.companionPeerAddrType = type;
          SETTINGS.saveToFile();
        },
        []() {
          SETTINGS.companionPeerAddr[0] = '\0';
          SETTINGS.companionPeerAddrType = blecompanion::CompanionAddrTypeUnset;
          SETTINGS.saveToFile();
        });
    if (!started) {
      LOG_ERR("BLECOMP", "Could not start the companion radio");
    } else {
      logHeap("after BLE init");
    }
  }
  // Unconditional snapshot rebuild: a restart creates a fresh Impl whose
  // revision space restarts at 1, so the caller's revision check cannot be
  // trusted to notice the new passkey/state.
  observed_ = ble_.status();
  buildSnapshot(observed_);
}

void CompanionActivity::onEnter() {
  Activity::onEnter();
  resetInputArmed_ = false;
  beginSession();
  // Publish the first snapshot with a blocking lock: one bounded wait for the
  // previous activity's paint, so the first frame of this activity is correct.
  {
    RenderLock lock;
    snapshot_ = pendingSnapshot_;
    snapshotDirty_ = false;
  }
  requestUpdate();
}

void CompanionActivity::onExit() {
  ble_.stop();
  const auto heap = HalMemory::getInternalHeap();
  LOG_INF("BLECOMP", "Heap after teardown: free %u, largest %u (before init: free %u, largest %u)",
          static_cast<unsigned>(heap.freeBytes), static_cast<unsigned>(heap.largestBlockBytes),
          static_cast<unsigned>(heapBeforeBegin_), static_cast<unsigned>(largestBeforeBegin_));
  Activity::onExit();
}

void CompanionActivity::loop() {
  ble_.loop();

  // Main-task read: BleCompanion state is main-loop-owned. The render task
  // never sees this path; it reads the published snapshot only.
  const blecompanion::Status status = ble_.status();
  if (status.revision != observed_.revision) {
    observed_ = status;
    if (observed_.state == blecompanion::RadioState::Connected) {
      logHeap("after connect");
    }
    buildSnapshot(status);
  }
  if (publishSnapshot()) {
    // The new snapshot is visible; only now ask for the repaint.
    requestUpdate();
  }

  // Pairing reset: hold Down or Confirm, armed only after a neutral frame so
  // the release that opened this activity cannot trigger it.
  const bool anyForwarded = std::any_of(
      std::begin(FORWARDED), std::end(FORWARDED),
      [this](const MappedInputManager::Button b) { return mappedInput.isPressed(b) || mappedInput.wasReleased(b); });
  if (!resetInputArmed_ && !anyForwarded) {
    resetInputArmed_ = true;
  }
  const bool resetHeld = mappedInput.isPressed(MappedInputManager::Button::Down) ||
                         mappedInput.isPressed(MappedInputManager::Button::Confirm);
  const bool resetReleased = mappedInput.wasReleased(MappedInputManager::Button::Down) ||
                             mappedInput.wasReleased(MappedInputManager::Button::Confirm);
  if (resetInputArmed_ && (resetHeld || resetReleased) && mappedInput.getHeldTime() >= PAIRING_RESET_HOLD_MS) {
    // ONE coherent reset: the adapter deletes only the companion bond and
    // clears the persisted identity (with saveToFile) on success; a fresh
    // pairing session starts only after that succeeded.
    if (ble_.forgetBond()) {
      LOG_INF("BLECOMP", "Companion pairing reset");
      ble_.stop();
      beginSession();
    } else {
      LOG_ERR("BLECOMP", "Companion pairing reset failed; nothing was deleted");
      requestUpdate();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Power)) {
    // Power leaves the session without forwarding an edge to the phone.
    Activity::finish();
    return;
  }

  // Physical button edges to the phone, one per pass per button.
  for (const auto b : FORWARDED) {
    uint8_t button = 0;
    if (!protocolButton(b, button)) continue;
    if (mappedInput.wasPressed(b)) ble_.sendButtonEvent(button, true);
    if (mappedInput.wasReleased(b)) ble_.sendButtonEvent(button, false);
  }
}

void CompanionActivity::render(RenderLock&&) {
  // Render task: reads ONLY the published snapshot. BleCompanion state is
  // main-task-owned and must never be touched from this context (enforced by
  // the source-contract test).
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int top = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight}, tr(STR_COMPANION_TITLE));

  const RenderSnapshot& snapshot = snapshot_;
  if (!snapshot.compiledIn || snapshot.state == blecompanion::RadioState::Stopped) {
    renderer.drawCenteredText(UI_12_FONT_ID, top + 40, tr(STR_COMPANION_START_FAILED), true, EpdFontFamily::BOLD);
  } else if (snapshot.sessionActive && snapshot.contentPresent) {
    int y = top + 20;
    renderer.drawText(UI_12_FONT_ID, metrics.contentSidePadding, y, snapshot.title, true, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(UI_12_FONT_ID) * 2;
    const int bodyWidth = width - metrics.contentSidePadding * 2;
    const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    const int maxLines = 6;
    const auto lines = renderer.wrappedText(UI_10_FONT_ID, snapshot.body, bodyWidth, maxLines);
    for (const auto& line : lines) {
      renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y, line.c_str());
      y += lineHeight;
    }
  } else if (snapshot.state == blecompanion::RadioState::Connected || snapshot.sessionActive) {
    char code[16];
    snprintf(code, sizeof(code), "%06u", static_cast<unsigned>(snapshot.passkey));
    renderer.drawCenteredText(UI_12_FONT_ID, top + 40, tr(STR_COMPANION_CONNECTED), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, top + 80, code);
  } else if (snapshot.state == blecompanion::RadioState::AwaitingAuthentication) {
    renderer.drawCenteredText(UI_12_FONT_ID, top + 40, tr(STR_COMPANION_PAIRING), true, EpdFontFamily::BOLD);
    char code[16];
    snprintf(code, sizeof(code), "%06u", static_cast<unsigned>(snapshot.passkey));
    renderer.drawCenteredText(UI_12_FONT_ID, top + 80, code, true, EpdFontFamily::BOLD);
  } else {
    renderer.drawCenteredText(UI_12_FONT_ID, top + 40, tr(STR_COMPANION_WAITING), true, EpdFontFamily::BOLD);
    char code[16];
    snprintf(code, sizeof(code), "%06u", static_cast<unsigned>(snapshot.passkey));
    renderer.drawCenteredText(UI_10_FONT_ID, top + 80, code);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
