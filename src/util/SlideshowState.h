#pragma once

#include <cstdint>
#include <cstring>
#include <string>

// Slideshow retained state for the timed deep-sleep lifecycle. The state lives
// in RTC memory: it survives deep sleep and esp_restart() and is garbage on a
// cold boot, so validity is magic-guarded and fails closed — only a validly
// armed path resumes the slideshow on a timer wake. The sleep request is
// ordinary RAM: the main loop consumes it to perform the one-frame sleep
// transition (Start performs the one-time persistence cleanup, Continue does
// none).
namespace slideshow {

enum class SleepRequest : uint8_t {
  None,
  Start,     // slideshow start: main loop performs the one-time persistence cleanup
  Continue,  // timer-resume frame: no per-frame SD writes
};

// 512 B total — a deliberate proof cap sized from the RTC SLOW linker budget
// (~2.6 KB free of 8 KB on both supported targets); longer paths fail closed
// at arm().
struct SlideshowState {
  uint32_t magic;
  char path[508];
};

inline constexpr uint32_t SLIDESHOW_MAGIC = 0x534C4944;  // 'SLID'

// Pure validation policy (host-testable): exact magic, a NUL inside path[],
// non-empty path rooted at '/'.
inline bool stateValid(const SlideshowState& state) {
  if (state.magic != SLIDESHOW_MAGIC) return false;
  if (memchr(state.path, '\0', sizeof(state.path)) == nullptr) return false;
  // Non-empty is implied by the root requirement.
  return state.path[0] == '/';
}

// Pure arm-input predicate: non-empty, rooted, and fits the buffer with its
// terminating NUL.
inline bool armInputValid(const std::string& path) {
  return !path.empty() && path[0] == '/' && path.size() < sizeof(SlideshowState::path);
}

// RTC retained-state accessors (globals live in SlideshowState.cpp).
// arm(): validate, clear the magic FIRST, copy the path with guaranteed NUL
// termination, write the magic LAST — the state is only valid once fully
// written. Returns false (and leaves no valid state) for empty/relative/
// over-long paths.
bool arm(const std::string& path);
void clearRetainedState();
bool hasValidRetainedState();
// Copy of the retained path; reading does not consume or clear the state.
// Empty string when no valid state is armed.
std::string getRetainedPath();

// RAM-only (BSS) one-frame sleep handoff to the main loop; not RTC retained.
void requestSleep(SleepRequest kind);
SleepRequest takeSleepRequest();

}  // namespace slideshow
