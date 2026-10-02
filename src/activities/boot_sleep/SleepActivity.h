#pragma once
#include <string>

#include "activities/Activity.h"

class Bitmap;
class HalFile;

class SleepActivity final : public Activity {
 public:
  // slideshowContinue: managed timer-wake continuation of the SLEEP slideshow
  // (main.cpp constructs this activity directly before display init): onEnter
  // advances to the next image in the retained frame's directory, re-arms the
  // retained state, renders it image-only with the sleep render profile and
  // requests the frame sleep. Normal sleep behavior is unchanged when false.
  explicit SleepActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool fromTimeout = false,
                         bool slideshowContinue = false)
      : Activity("Sleep", renderer, mappedInput), fromTimeout(fromTimeout), slideshowContinue(slideshowContinue) {}
  void onEnter() override;

 private:
  void renderDefaultSleepScreen() const;
  void renderCustomSleepScreen() const;
  void renderCoverSleepScreen() const;
  void renderBitmapSleepScreen(const Bitmap& bitmap, bool preserveBackground = false) const;
  bool renderSleepOverlayFile(HalFile& file, const char* pathForLog) const;
  bool renderTransparentOverlayPng(const std::string& path) const;
  bool renderSleepOverlayPath(const std::string& path) const;
  void renderLastScreenSleepScreen() const;
  void renderTransparentCustomSleepScreen() const;
  void renderBlankSleepScreen() const;
  // Sleep Screen = Slideshow: prepare the first frame from the fixed
  // /.sleep -> /sleep source during the normal sleep render phase (render
  // image-only with the sleep render profile, arm the exact path as
  // slideshow::Mode::Sleep). The persisted ORDER policy picks the initial
  // frame. Nothing usable fails closed: the retained state is cleared and
  // the ordinary default sleep screen renders (no timer).
  void renderSlideshowSleepScreen() const;
  // Managed timer-wake continuation: battery cutoff check, then advance,
  // re-arm, render, request the frame sleep. Fail closed on a broken source:
  // clear the retained state and route to the ordinary wake state (no timer
  // loop). The battery cutoff routes to the static-sleep fallback instead.
  void continueSlideshow();
  // Fail-closed exit from a broken sleep slideshow: clear the retained state
  // and route Home (clean refresh replaces the last frame).
  void endSlideshowToOrdinary();
  // Battery-cutoff exit mid-slideshow: clear the retained state, repaint the
  // ordinary static sleep screen once and request a power-button-only sleep
  // (no next timer wake — the slideshow must not wake-loop a low battery).
  void endSlideshowToStaticSleep();

  bool fromTimeout = false;
  bool slideshowContinue = false;
};
