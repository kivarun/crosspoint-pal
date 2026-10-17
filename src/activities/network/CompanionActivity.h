#pragma once

#include <BleCompanion.h>

#include "activities/Activity.h"

// Manual phone-companion session: the reader advertises, a bonded phone
// connects, pushes a short text, and receives button events. Back leaves the
// session; the radio is fully torn down on exit. Started from Settings, never
// at boot.
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
  // Maps a MappedInputManager button to the protocol's Button value; returns
  // false for buttons the session does not forward (Power, nav duplicates).
  static bool protocolButton(MappedInputManager::Button button, uint8_t& out);

  void handleRevisionChanged();
  void logHeap(const char* when);
  void beginSession();

  blecompanion::BleCompanion ble_;
  blecompanion::Status observed_;
  bool resetInputArmed_ = false;
  // Heap figures for the memory audit log (before / after init / teardown).
  unsigned long heapBeforeBegin_ = 0;
  unsigned long largestBeforeBegin_ = 0;
};
