#include "SlideshowState.h"

#include <Logging.h>
#include <cstring>
#include <esp_attr.h>

namespace slideshow {

// RTC_NOINIT survives deep sleep and esp_restart(); it is garbage on a cold
// boot, so validity is magic-guarded (same pattern as main.cpp's silentReboot*
// state and Logging's RTC ring).
RTC_NOINIT_ATTR SlideshowState retainedState;
// RAM/BSS only: the one-frame sleep handoff to the main loop.
SleepRequest pendingSleepRequest = SleepRequest::None;

bool arm(const std::string& path) {
  if (!armInputValid(path)) {
    LOG_ERR("SLD", "Slideshow arm rejected (empty/relative/too long)");
    return false;
  }
  // The state is invalid while the copy is in flight; write the magic last so
  // a torn write can never leave a valid-looking path.
  clearRetainedState();
  memcpy(retainedState.path, path.c_str(), path.size() + 1);  // includes the NUL
  retainedState.magic = SLIDESHOW_MAGIC;
  return true;
}

void clearRetainedState() { retainedState.magic = 0; }

bool hasValidRetainedState() { return stateValid(retainedState); }

std::string getRetainedPath() { return hasValidRetainedState() ? std::string(retainedState.path) : std::string(); }

void requestSleep(const SleepRequest kind) { pendingSleepRequest = kind; }

SleepRequest takeSleepRequest() {
  const SleepRequest request = pendingSleepRequest;
  pendingSleepRequest = SleepRequest::None;
  return request;
}

}  // namespace slideshow
