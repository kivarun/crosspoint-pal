#pragma once

// Connection handles forced down by a fail-close stay poisoned until their
// disconnect completes: late events from a dying link must never reach
// product logic. Release relies on NimBLE delivering a handle's Disconnected
// before the stack can reuse it; entries lost to ring pressure are purged by
// the next fail-close against the live connection list. Pure and
// host-testable; the adapter pins the capacity against the stack's
// connection limit.

#include <cstddef>
#include <cstdint>

#include "ConnectionHandle.h"

namespace blecompanion {

// Must cover every connection the NimBLE stack allows at once; the adapter
// static_asserts this against NIMBLE_MAX_CONNECTIONS.
inline constexpr size_t kMaxCompanionConnections = 3;

class PoisonedHandles {
 public:
  // Returns false when the handle is the sentinel or the set is full.
  bool poison(uint16_t handle) {
    if (handle == kNoSessionHandle) return false;
    if (poisoned(handle)) return true;
    if (count_ == kMaxCompanionConnections) return false;
    handles_[count_++] = handle;
    return true;
  }

  void forget(uint16_t handle) {
    for (size_t i = 0; i < count_; i++) {
      if (handles_[i] == handle) {
        handles_[i] = handles_[count_ - 1];
        count_--;
        return;
      }
    }
  }

  bool poisoned(uint16_t handle) const {
    for (size_t i = 0; i < count_; i++) {
      if (handles_[i] == handle) return true;
    }
    return false;
  }

  // Forgets every handle absent from the live list: a poisoned handle that is
  // no longer connected finished its teardown, even if its Disconnected entry
  // was lost to ring pressure.
  void forgetNotIn(const uint16_t* live, size_t liveCount) {
    for (size_t i = 0; i < count_;) {
      bool connected = false;
      for (size_t j = 0; j < liveCount; j++) {
        if (live[j] == handles_[i]) {
          connected = true;
          break;
        }
      }
      if (connected) {
        i++;
      } else {
        forget(handles_[i]);
      }
    }
  }

 private:
  uint16_t handles_[kMaxCompanionConnections] = {};
  size_t count_ = 0;
};

}  // namespace blecompanion
