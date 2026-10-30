#pragma once

#include <BleCompanion.h>

#include "activities/Activity.h"

// Manual phone-companion session: the reader advertises, a bonded phone
// connects, pushes a short text, and receives button events. A short Back
// press is forwarded as one coherent press/release pair on its release;
// holding Back (EXIT_HOLD_MS in the .cpp) leaves the session via the normal
// onExit() and the BLE teardown, with no Back edge sent to the phone. Power
// keeps the firmware's stock semantics (sleep, refresh, ignore): main.cpp
// handles it before the activity loop, and a sleep entered through
// enterDeepSleep still runs this activity's onExit, because goToSleep
// replaces this activity and processes the swap in its synchronous loop
// pass. Started from Settings, never at boot. Protocol contract:
// docs/ble-companion-protocol.md.
//
// Render isolation: the render task reads ONLY the published RenderSnapshot
// below. The BleCompanion's own state is main-task-owned (loop()/status()/
// content()); render() never touches it. Publication happens under the same
// rendering mutex the render task holds, via a non-blocking try-lock that
// retries on the next loop pass.
class CompanionActivity final : public Activity {
 public:
  CompanionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Companion", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }

 private:
  // Everything the render task needs, deep-copied: no pointers into the
  // companion's session storage.
  struct RenderSnapshot {
    bool compiledIn = false;
    blecompanion::RadioState state = blecompanion::RadioState::Stopped;
    bool sessionActive = false;
    bool contentPresent = false;
    uint32_t passkey = 0;
    uint32_t revision = 0;
    char title[blecompanion::MAX_TITLE_BYTES + 1] = {};
    char body[blecompanion::MAX_BODY_BYTES + 1] = {};
  };

  // Maps a MappedInputManager button to the protocol's Button value; returns
  // false for buttons the session does not forward (Power, nav duplicates).
  static bool protocolButton(MappedInputManager::Button button, uint8_t& out);

  void buildSnapshot(const blecompanion::Status& status);
  // Publishes the pending snapshot under the rendering mutex; false when a
  // paint is in progress (retry on the next loop pass).
  bool publishSnapshot();
  void logHeap(const char* when);
  void beginSession();

  blecompanion::BleCompanion ble_;
  blecompanion::Status observed_;
  RenderSnapshot pendingSnapshot_;  // built on the main task
  RenderSnapshot snapshot_;         // published for the render task
  bool snapshotDirty_ = false;
  bool resetInputArmed_ = false;
  // Heap figures for the memory audit log (before / after init / teardown).
  unsigned long heapBeforeBegin_ = 0;
  unsigned long largestBeforeBegin_ = 0;
};
