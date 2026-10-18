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

void CompanionActivity::beginSession() {
  heapBeforeBegin_ = HalMemory::getInternalHeap().freeBytes;
  largestBeforeBegin_ = HalMemory::getInternalHeap().largestBlockBytes;
  // One radio owner: the page turner must be off before the companion starts,
  // and its own policy resumes when this activity leaves.
  if (!bleturner::stopNow(1000)) {
    LOG_ERR("BLECOMP", "Could not stop the BLE page turner; the companion radio stays off");
    observed_ = ble_.status();
    requestUpdate();
    return;
  }
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
  observed_ = ble_.status();
  requestUpdate();
}

void CompanionActivity::onEnter() {
  Activity::onEnter();
  resetInputArmed_ = false;
  beginSession();
}

void CompanionActivity::onExit() {
  ble_.stop();
  const auto heap = HalMemory::getInternalHeap();
  LOG_INF("BLECOMP", "Heap after teardown: free %u, largest %u (before init: free %u, largest %u)",
          static_cast<unsigned>(heap.freeBytes), static_cast<unsigned>(heap.largestBlockBytes),
          static_cast<unsigned>(heapBeforeBegin_), static_cast<unsigned>(largestBeforeBegin_));
  Activity::onExit();
}

void CompanionActivity::handleRevisionChanged() {
  observed_ = ble_.status();
  if (observed_.state == blecompanion::RadioState::Connected) {
    logHeap("after connect");
  }
  requestUpdate();
}

void CompanionActivity::loop() {
  ble_.loop();

  if (ble_.status().revision != observed_.revision) {
    handleRevisionChanged();
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
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int top = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight}, tr(STR_COMPANION_TITLE));

  const blecompanion::Status status = ble_.status();
  if (!status.compiledIn || status.state == blecompanion::RadioState::Stopped) {
    renderer.drawCenteredText(UI_12_FONT_ID, top + 40, tr(STR_COMPANION_START_FAILED), true, EpdFontFamily::BOLD);
  } else if (status.sessionActive && status.contentPresent) {
    const blecompanion::ContentView view = ble_.content();
    int y = top + 20;
    renderer.drawText(UI_12_FONT_ID, metrics.contentSidePadding, y, view.title, true, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(UI_12_FONT_ID) * 2;
    const int bodyWidth = width - metrics.contentSidePadding * 2;
    const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    const int maxLines = 6;
    const auto lines = renderer.wrappedText(UI_10_FONT_ID, view.body, bodyWidth, maxLines);
    for (const auto& line : lines) {
      renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y, line.c_str());
      y += lineHeight;
    }
  } else if (status.state == blecompanion::RadioState::Connected || status.sessionActive) {
    char code[16];
    snprintf(code, sizeof(code), "%06u", status.passkey);
    renderer.drawCenteredText(UI_12_FONT_ID, top + 40, tr(STR_COMPANION_CONNECTED), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, top + 80, code);
  } else if (status.state == blecompanion::RadioState::AwaitingAuthentication) {
    renderer.drawCenteredText(UI_12_FONT_ID, top + 40, tr(STR_COMPANION_PAIRING), true, EpdFontFamily::BOLD);
    char code[16];
    snprintf(code, sizeof(code), "%06u", status.passkey);
    renderer.drawCenteredText(UI_12_FONT_ID, top + 80, code, true, EpdFontFamily::BOLD);
  } else {
    renderer.drawCenteredText(UI_12_FONT_ID, top + 40, tr(STR_COMPANION_WAITING), true, EpdFontFamily::BOLD);
    char code[16];
    snprintf(code, sizeof(code), "%06u", status.passkey);
    renderer.drawCenteredText(UI_10_FONT_ID, top + 80, code);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
