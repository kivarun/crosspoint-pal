#include <gtest/gtest.h>

#include <string>

#include "util/SlideshowState.h"

// The slideshow retained-state validation policy (lib-side pure core of
// src/util/SlideshowState.h). The device-side RTC globals live in
// SlideshowState.cpp and are not exercised here.

namespace {

using slideshow::Mode;
using slideshow::SLIDESHOW_MAGIC;
using slideshow::SlideshowState;

// The proof firmware's pre-v2 state: path-only layout, magic 0x534C4944, no
// mode byte. Its states must never validate under the v2 schema.
constexpr uint32_t PROOF_FIRMWARE_MAGIC = 0x534C4944;

SlideshowState armed(const std::string& path, const Mode mode = Mode::Viewer) {
  SlideshowState state{};
  state.magic = SLIDESHOW_MAGIC;
  state.mode = static_cast<uint8_t>(mode);
  path.copy(state.path, sizeof(state.path) - 1);
  state.path[std::min(path.size(), sizeof(state.path) - 1)] = '\0';
  return state;
}

}  // namespace

TEST(SlideshowStateValid, AcceptsArmedViewerState) {
  EXPECT_TRUE(slideshow::stateValid(armed("/Images/photo.bmp", Mode::Viewer)));
  EXPECT_TRUE(slideshow::stateValid(armed("/", Mode::Viewer)));
}

TEST(SlideshowStateValid, AcceptsArmedSleepState) {
  EXPECT_TRUE(slideshow::stateValid(armed("/.sleep/frame.bmp", Mode::Sleep)));
  EXPECT_TRUE(slideshow::stateValid(armed("/", Mode::Sleep)));
}

// RTC_NOINIT is garbage on a cold boot: a wrong magic fails closed.
TEST(SlideshowStateValid, RejectsWrongMagic) {
  auto state = armed("/Images/photo.bmp");
  state.magic = SLIDESHOW_MAGIC + 1;
  EXPECT_FALSE(slideshow::stateValid(state));
  state.magic = 0;
  EXPECT_FALSE(slideshow::stateValid(state));
}

// Schema bump: a state written by the proof firmware (old magic, no mode
// byte) can never validate under v2.
TEST(SlideshowStateValid, RejectsProofFirmwareMagic) {
  auto state = armed("/Images/photo.bmp");
  state.magic = PROOF_FIRMWARE_MAGIC;
  EXPECT_FALSE(slideshow::stateValid(state));
}

TEST(SlideshowStateValid, RejectsUnknownMode) {
  auto state = armed("/Images/photo.bmp");
  state.mode = 2;  // first value past Mode::Sleep
  EXPECT_FALSE(slideshow::stateValid(state));
  state.mode = 255;
  EXPECT_FALSE(slideshow::stateValid(state));
}

TEST(SlideshowStateValid, RejectsEmptyPath) {
  auto state = armed("/");
  state.path[0] = '\0';
  EXPECT_FALSE(slideshow::stateValid(state));
}

TEST(SlideshowStateValid, RejectsRelativePath) {
  EXPECT_FALSE(slideshow::stateValid(armed("Images/photo.bmp")));
  EXPECT_FALSE(slideshow::stateValid(armed("./Images/photo.bmp")));
}

// An unterminated buffer (no NUL anywhere) must never validate — the path is
// not trustable as a string.
TEST(SlideshowStateValid, RejectsMissingNulTerminator) {
  auto state = armed("/Images/photo.bmp");
  for (char& c : state.path) {
    if (c == '\0') c = 'x';
  }
  EXPECT_FALSE(slideshow::stateValid(state));
}

// Capacity boundary: a path that exactly fills the buffer with its NUL in the
// final byte is valid.
TEST(SlideshowStateValid, AcceptsExactCapacityWithFinalByteNul) {
  auto state = armed("/");
  std::string path("/a");
  path.resize(sizeof(SlideshowState::path) - 1, 'a');  // no NUL yet
  path.copy(state.path, path.size());
  state.path[sizeof(state.path) - 1] = '\0';
  EXPECT_TRUE(slideshow::stateValid(state));
}

// The arm-input predicate rejects empty/relative/over-long paths and accepts
// the exact capacity boundary.
TEST(SlideshowArmInput, CapacityAndShapePredicate) {
  EXPECT_TRUE(slideshow::armInputValid("/Images/photo.bmp"));
  EXPECT_FALSE(slideshow::armInputValid(""));
  EXPECT_FALSE(slideshow::armInputValid("Images/photo.bmp"));

  std::string maxPath("/");
  maxPath.resize(sizeof(SlideshowState::path) - 1, 'a');  // 506 chars + NUL
  EXPECT_TRUE(slideshow::armInputValid(maxPath));
  maxPath.push_back('a');  // would not fit with its NUL
  EXPECT_FALSE(slideshow::armInputValid(maxPath));
}

// Timer policy of the one-frame sleep handoff (slideshow::sleepRequestArmsTimer):
// the slideshow RUNS on Start and Continue; StaticSleep and None arm nothing.
TEST(SleepRequestTimerPolicy, FullContract) {
  EXPECT_TRUE(slideshow::sleepRequestArmsTimer(slideshow::SleepRequest::Start));
  EXPECT_TRUE(slideshow::sleepRequestArmsTimer(slideshow::SleepRequest::Continue));
  EXPECT_FALSE(slideshow::sleepRequestArmsTimer(slideshow::SleepRequest::StaticSleep));
  EXPECT_FALSE(slideshow::sleepRequestArmsTimer(slideshow::SleepRequest::None));
}
