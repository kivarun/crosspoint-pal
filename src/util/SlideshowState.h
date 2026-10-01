#pragma once

#include <cstdint>
#include <cstring>
#include <string>

#include "util/SlideshowPolicy.h"

// Slideshow retained state for the timed deep-sleep lifecycle. The state lives
// in RTC memory: it survives deep sleep and esp_restart() and is garbage on a
// cold boot, so validity is magic-guarded and fails closed — only a validly
// armed path resumes the slideshow on a timer wake. The mode byte separates
// the two use cases (Viewer re-opens the image viewer; Sleep continues the
// sleep-screen slideshow inside SleepActivity). The sleep request is ordinary
// RAM: the main loop consumes it to perform the one-frame sleep transition
// (Start performs the one-time persistence cleanup, Continue does none).
namespace slideshow {

enum class SleepRequest : uint8_t {
  None,
  Start,     // slideshow start: main loop performs the one-time persistence cleanup
  Continue,  // timer-resume frame: no per-frame SD writes
};

// 512 B total — a deliberate cap sized from the RTC SLOW linker budget (~2.6
// KB free of 8 KB on both supported targets); longer paths fail closed at
// arm(). Schema v2: the mode byte sits between the magic and the path
// (shrunken to 507), and the magic was bumped so a retained state written by
// the proof firmware (0x534C4944 'SLID', path-only layout) can never validate.
struct SlideshowState {
  uint32_t magic;
  uint8_t mode;  // Mode, validated by stateValid
  char path[507];
};
static_assert(sizeof(SlideshowState) == 512, "retained slideshow state must stay within its RTC budget");

inline constexpr uint32_t SLIDESHOW_MAGIC = 0x534C4945;  // 'SLID' schema v2 (mode byte added)

// Pure validation policy (host-testable): exact magic, a known mode, a NUL
// inside path[], non-empty path rooted at '/'.
inline bool modeValid(const uint8_t mode) { return mode <= static_cast<uint8_t>(Mode::Sleep); }

inline bool stateValid(const SlideshowState& state) {
  if (state.magic != SLIDESHOW_MAGIC) return false;
  if (!modeValid(state.mode)) return false;
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
// arm(): invalidate the retained state FIRST, then validate; on any false
// return no valid retained state remains. A successful arm copies the mode
// and the path with guaranteed NUL termination and writes the magic LAST —
// the state is only valid once fully written. Empty/relative/over-long paths
// fail.
bool arm(const std::string& path, Mode mode);
void clearRetainedState();
bool hasValidRetainedState();
// Armed use case; meaningful only while hasValidRetainedState() is true.
Mode getRetainedMode();
// Copy of the retained path; reading does not consume or clear the state.
// Empty string when no valid state is armed.
std::string getRetainedPath();

// RAM-only (BSS) one-frame sleep handoff to the main loop; not RTC retained.
void requestSleep(SleepRequest kind);
SleepRequest takeSleepRequest();

}  // namespace slideshow
