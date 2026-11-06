#include <Arduino.h>
#include <BleCompanion.h>
#include <BlePageTurner.h>
#include <BoardConfig.h>
#include <Epub.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalFrontlight.h>
#include <HalGPIO.h>
#include <HalMemory.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <HalSystem.h>
#include <HalTiltSensor.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <SPI.h>
#include <TrustedTime.h>
#include <VectorFontSupport.h>
#include <WiFi.h>
#include <XteinkDetect.h>
#include <builtinFonts/all.h>
#include <esp_heap_caps.h>
#include <esp_wifi.h>
#include <time.h>

#include <atomic>
#include <cstdarg>
#include <cstring>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "KOReaderCredentialStore.h"
#include "LcpPassphraseStore.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "OpdsTokenStore.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "WifiCredentialStore.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/boot_sleep/SleepActivity.h"
#include "activities/settings/SdFirmwareUpdateActivity.h"
#include "activities/util/BmpViewerActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "platform/UsbSerialJtagHandoff.h"
#include "util/BleLightSleepDiag.h"
#include "util/ButtonNavigator.h"
#include "util/CompanionGatePilot.h"
#include "util/CompanionGatePolicy.h"
#include "util/CompanionGatePower.h"
#include "util/CompanionGateState.h"
#include "util/CompanionIdentityMirror.h"
#include "util/CompanionStandbyDiag.h"
#include "util/PluginEvents.h"
#include "util/PmOnlyDiag.h"
#include "util/RtcClockDiag.h"
#include "util/ScreenshotUtil.h"
#include "util/SlideSwitch.h"
#include "util/SlideshowCapability.h"
#include "util/SlideshowState.h"
#include "util/Timezones.h"

#if CROSSPOINT_VECTOR_FONTS
// Rendering (incl. FreeType TTF rasterization) runs on the Arduino loop task.
// The default 8 KB stack overflows inside FreeType's FT_Open_Face / variable-font
// parsing. This runtime override applies even with the prebuilt (dio_opi) core,
// where CONFIG_ARDUINO_LOOP_STACK_SIZE from sdkconfig is baked in and ignored.
// Vector-font boards only: without TTF the stock loop stack has always sufficed,
// and non-PSRAM boards need the 16KB back in DRAM.
SET_LOOP_TASK_STACK_SIZE(24 * 1024)
#endif

GfxRenderer renderer(display);
MappedInputManager mappedInputManager(gpio, renderer);
ActivityManager activityManager(renderer, mappedInputManager);
FontDecompressor fontDecompressor;
SdCardFontSystem sdFontSystem;
FontCacheManager fontCacheManager(renderer.getFontMap(), renderer.getSdCardFonts(), renderer.getTtfFonts());
static unsigned long allowSleepAt = 0;
static unsigned long lastX4ProPowerClickAt = 0;

namespace {
constexpr unsigned long X4PRO_POWER_DOUBLE_CLICK_MS = 500;
constexpr unsigned long X4PRO_POWER_CLICK_MAX_HOLD_MS = 300;
}  // namespace

// A wake hold must never become an in-app power-button action.  Boot may continue
// while the button is held; swallow the one release that ends that wake gesture.
static bool wakePowerReleasePending = false;

// Fonts
EpdFont notosans14RegularFont(&notosans_14_regular);
EpdFont notosans14BoldFont(&notosans_14_bold);
EpdFont notosans14ItalicFont(&notosans_14_italic);
EpdFont notosans14BoldItalicFont(&notosans_14_bolditalic);
EpdFontFamily notosans14FontFamily(14, &notosans14RegularFont, &notosans14BoldFont, &notosans14ItalicFont,
                                   &notosans14BoldItalicFont);
#ifndef OMIT_FONTS
EpdFont notosans12RegularFont(&notosans_12_regular);
EpdFont notosans12BoldFont(&notosans_12_bold);
EpdFont notosans12ItalicFont(&notosans_12_italic);
EpdFont notosans12BoldItalicFont(&notosans_12_bolditalic);
EpdFontFamily notosans12FontFamily(12, &notosans12RegularFont, &notosans12BoldFont, &notosans12ItalicFont,
                                   &notosans12BoldItalicFont);
EpdFont notosans16RegularFont(&notosans_16_regular);
EpdFont notosans16BoldFont(&notosans_16_bold);
EpdFont notosans16ItalicFont(&notosans_16_italic);
EpdFont notosans16BoldItalicFont(&notosans_16_bolditalic);
EpdFontFamily notosans16FontFamily(16, &notosans16RegularFont, &notosans16BoldFont, &notosans16ItalicFont,
                                   &notosans16BoldItalicFont);
EpdFont notosans18RegularFont(&notosans_18_regular);
EpdFont notosans18BoldFont(&notosans_18_bold);
EpdFont notosans18ItalicFont(&notosans_18_italic);
EpdFont notosans18BoldItalicFont(&notosans_18_bolditalic);
EpdFontFamily notosans18FontFamily(18, &notosans18RegularFont, &notosans18BoldFont, &notosans18ItalicFont,
                                   &notosans18BoldItalicFont);

#endif  // OMIT_FONTS

EpdFont smallFont(&ubuntu_8_regular);
EpdFontFamily smallFontFamily(8, &smallFont);

EpdFont ui10RegularFont(&ubuntu_10_regular);
EpdFont ui10BoldFont(&ubuntu_10_bold);
EpdFontFamily ui10FontFamily(10, &ui10RegularFont, &ui10BoldFont);

EpdFont ui12RegularFont(&ubuntu_12_regular);
EpdFont ui12BoldFont(&ubuntu_12_bold);
EpdFontFamily ui12FontFamily(12, &ui12RegularFont, &ui12BoldFont);

// Definitions for SilentRestart.h. RTC_NOINIT survives ESP.restart() but not power loss.
RTC_NOINIT_ATTR uint32_t silentRebootMagic;
RTC_NOINIT_ATTR uint32_t silentRebootTarget;
RTC_NOINIT_ATTR uint32_t silentRebootPayload;
constexpr uint32_t SILENT_REBOOT_MAGIC = 0xC1EAB007;
constexpr uint32_t SILENT_REBOOT_TARGET_HOME = 0;
constexpr uint32_t SILENT_REBOOT_TARGET_READER = 1;
constexpr uint32_t SILENT_REBOOT_TARGET_SETTINGS = 2;
constexpr uint32_t SILENT_REBOOT_TARGET_JOIN_NETWORK = 3;
constexpr uint32_t SILENT_REBOOT_TARGET_MAX = SILENT_REBOOT_TARGET_JOIN_NETWORK;
constexpr uint32_t SILENT_REBOOT_LIGHT_ON = 1U << 0;

// How the device is coming back to life, resolved once at boot. Both resume
// flows suppress the splash and leave the panel holding its pre-boot frame; a
// plain boot shows the splash. See setup() for the resolution.
enum class BootResume : uint8_t {
  Splash,          // cold boot, flash, panic, or plain reboot
  Silent,          // heap-defrag ESP.restart() (RTC flag; lost on power loss)
  SplashlessWake,  // wake from deep sleep with the splash suppressed by the SD flag
};

// Latched true once enterDeepSleep() commits to sleeping, before it tears down
// the current activity. WiFi activities call silentRestart() in onExit() to
// clear heap fragmentation on the way out, but deep sleep is a full chip reset
// on wake and already clears the heap, so rebooting here would just power the
// device back up against the user's sleep gesture. Never cleared:
// startDeepSleep() does not return, so a set latch only ends at the wakeup reset.
static bool deepSleepInProgress = false;

// A silent restart is internal maintenance, so the light must come back exactly
// as the user left it. SETTINGS.frontlightOn is the saved preference and
// legitimately diverges from the live state (a wake with Restore Light on Wake
// off leaves the light off while the saved "was on" preference is kept), so
// carry the live state across the reboot instead of re-deriving it from
// settings. Cleared with the magic in setup().
static void armSilentReboot(const uint32_t target) {
  silentRebootTarget = target;
  silentRebootPayload = Frontlight.isOn() ? SILENT_REBOOT_LIGHT_ON : 0;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
}

// Returns instead of rebooting when sleep supersedes the reboot; callers keep
// running in that case.
static void silentRestartTo(const uint32_t target, const char* targetName) {
  if (deepSleepInProgress) return;  // sleeping supersedes the heap-defrag reboot
  armSilentReboot(target);
  LOG_DBG("MAIN", "Silent restart (target=%s)", targetName);
  // E-ink retains the previous frame until the target's first paint lands
  // (~2-3s). Without an overlay, users don't see the reboot and fire input
  // through to the new activity. On Home, Select on the default
  // selectorIndex=0 opens the most-recent book, looking like a trampoline back
  // to the reader they just exited.
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  ESP.restart();
}

void silentRestart() { silentRestartTo(SILENT_REBOOT_TARGET_HOME, "home"); }

void silentRestartToReader() { silentRestartTo(SILENT_REBOOT_TARGET_READER, "reader"); }

void silentRestartToSettings() { silentRestartTo(SILENT_REBOOT_TARGET_SETTINGS, "settings"); }

void silentRestartToJoinNetwork() {
  if (deepSleepInProgress) return;
#if FREEINK_CAP_TOUCH
  // A software reset would cycle touch/frontlight rails; those boards proceed
  // into Join Network without the fresh-heap reboot (return, don't stop WiFi —
  // this runs on the way *in*, unlike the exit-time silentRestart()).
  if (BoardConfig::hasTouch()) return;
#endif
  armSilentReboot(SILENT_REBOOT_TARGET_JOIN_NETWORK);
  LOG_DBG("MAIN", "Silent restart (target=join-network)");
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  ESP.restart();
}

void restartToHomeAfterStorageHandoff() {
  if (deepSleepInProgress) return;  // sleeping supersedes the storage handoff reboot
  armSilentReboot(SILENT_REBOOT_TARGET_HOME);
  LOG_DBG("MAIN", "Restart after storage handoff (target=home)");
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  handoffUsbOtgToSerialJtag();
  ESP.restart();
}

void toggleFrontlight() {
  if (!Frontlight.present()) return;
  const bool lightOn = !Frontlight.isOn();
  Frontlight.setOn(lightOn);
  SETTINGS.frontlightOn = lightOn ? 1 : 0;
  SETTINGS.saveToFile();
  LOG_INF("LIGHT", "Frontlight toggled %s", lightOn ? "on" : "off");
}

// Applies the slide switch's position to the bound function at boot, on each
// change, and when its binding changes; the physical position stays the source
// of truth, as on stock.
void handleSlideSwitch() {
  static int appliedAction = -1;
  static int appliedOn = -1;
  if (!gpio.hasToggleSwitch()) return;
  // Compare the position, not the change edge: transfer loops poll input and
  // clear the edge before this runs.
  const bool on = gpio.isToggleSwitchOn();
  if (appliedAction == SETTINGS.slideSwitchAction && appliedOn == static_cast<int>(on)) return;
  appliedAction = SETTINGS.slideSwitchAction;
  appliedOn = on;
  switch (SETTINGS.slideSwitchAction) {
    case CrossPointSettings::SLIDE_SWITCH_NIGHT_MODE:
      if ((SETTINGS.screenInverted != 0) == on) return;
      SETTINGS.screenInverted = on ? 1 : 0;
      break;
    case CrossPointSettings::SLIDE_SWITCH_TOUCH_CONTROLS:
      if ((SETTINGS.touchReaderControls != CrossPointSettings::TOUCH_READER_OFF) == on) return;
      SETTINGS.touchReaderControls = on ? CrossPointSettings::TOUCH_READER_ON : CrossPointSettings::TOUCH_READER_OFF;
      break;
    default:
      // Also compare the saved state: setup() lit the light from the switch
      // without updating it.
      if (!Frontlight.present() || (Frontlight.isOn() == on && (SETTINGS.frontlightOn != 0) == on)) return;
      Frontlight.setOn(on);
      SETTINGS.frontlightOn = on ? 1 : 0;
      break;
  }
  SETTINGS.saveToFile();
  activityManager.requestUpdate();
  LOG_INF("SWITCH", "Slide switch %s", on ? "on" : "off");
}

bool handleX4ProFrontlightDoubleClick() {
  if (!BoardConfig::isX4Pro() || !SETTINGS.doubleClickPwrLight || !gpio.wasReleased(HalGPIO::BTN_POWER)) {
    return false;
  }

  const unsigned long now = millis();
  if (gpio.getPowerButtonHeldTime() > X4PRO_POWER_CLICK_MAX_HOLD_MS) {
    lastX4ProPowerClickAt = 0;
    return false;
  }

  if (lastX4ProPowerClickAt == 0 || now - lastX4ProPowerClickAt > X4PRO_POWER_DOUBLE_CLICK_MS) {
    lastX4ProPowerClickAt = now;
    return false;
  }

  lastX4ProPowerClickAt = 0;
  toggleFrontlight();
  return true;
}

// Bluetooth page turner (lib/BlePageTurner): what the module may ask of this firmware. The module
// owns the radio; loop() tells it what is on screen, and the screen changes and sleep ask it to stop.
namespace pageturner {
std::atomic<bool> holdsCpu{false};

bleturner::Heap heap() {
  const auto h = HalMemory::getInternalHeap();
  return {h.freeBytes, h.largestBlockBytes};
}

// Runs on the radio's start task. Only TRIES the render lock: the render task can hold it through a
// chapter build that is waiting for this very radio.
bool releaseCaches() {
  RenderLock lock(RenderLock::Mode::Try);
  if (!lock.ownsLock()) return false;
  fontCacheManager.releaseSdFontCaches();
  return true;
}

bool deliver(const bleturner::Action action) {
  switch (action) {
    case bleturner::Action::NextPage:
      return activityManager.remoteTurn(true, false);
    case bleturner::Action::PrevPage:
      return activityManager.remoteTurn(false, false);
    case bleturner::Action::NextChapter:
      return activityManager.remoteTurn(true, true);
    case bleturner::Action::PrevChapter:
      return activityManager.remoteTurn(false, true);
    case bleturner::Action::Confirm:
      return activityManager.remoteClick(RemoteButton::Confirm);
    case bleturner::Action::Back:
      return activityManager.remoteClick(RemoteButton::Back);
    case bleturner::Action::ReaderMenu:
      return activityManager.remoteShortcut(true);
    case bleturner::Action::SaveQuote:
      return activityManager.remoteShortcut(false);
    case bleturner::Action::None:
      return false;
  }
  return false;
}

bool yieldForRadio() { return true; }        // the reader keeps no parser alive between pages
bool fileTransferActive() { return false; }  // file transfer is a screen of its own: the radio is off there
void restartIntoBook() { silentRestartToReader(); }

// The radio needs a steady clock while it is up or changing state.
void holdFullSpeed(const bool hold) {
  holdsCpu.store(hold);
  if (hold) powerManager.setPowerSaving(false);
}

void log(const bool error, const char* format, va_list args) {
#ifdef ENABLE_SERIAL_LOG
  if (!error && LOG_LEVEL < 1) return;
  char line[160];
  vsnprintf(line, sizeof(line), format, args);
  logPrintf(error ? "ERR" : "INF", "BLE", "%s", line);
#else
  (void)error;
  (void)format;
  (void)args;
#endif
}

const bleturner::Host host{heap,          releaseCaches, deliver, yieldForRadio, fileTransferActive, restartIntoBook,
                           holdFullSpeed, log,           nullptr};
}  // namespace pageturner

constexpr char SLEEP_FRAME_FILE[] = "/.crosspoint/sleep_frame.bin";

static void saveSleepFrameBuffer() {
  HalFile file;
  if (!Storage.openFileForWrite("SLP", SLEEP_FRAME_FILE, file)) return;
  file.write(renderer.getFrameBuffer(), renderer.getBufferSize());
  file.close();
}

static bool loadSleepFrameBuffer() {
  HalFile file;
  if (!Storage.openFileForRead("SLP", SLEEP_FRAME_FILE, file)) return false;
  const size_t bufferSize = display.getBufferSize();
  const size_t bytesRead = file.read(display.getFrameBuffer(), bufferSize);
  file.close();
  if (bytesRead != bufferSize) {
    Storage.remove(SLEEP_FRAME_FILE);
    return false;
  }
  Storage.remove(SLEEP_FRAME_FILE);
  return true;
}

// Plugin-event delivery on the way into deep sleep. sleep.enter is delivered
// now — over the live connection, or by bringing WiFi up when a plugin
// subscribes (e.g. fetching a fresh /sleep.bmp so THIS sleep shows it — the
// drain runs before goToSleep() renders the sleep screen). The connect path
// is bounded (join deadline + drain event budget), skipped on low battery,
// and sleep is never blocked on the network: a failed join or delivery just
// sleeps with the previous image and the queued events retry on the next
// drain (at-least-once). The caller's WiFi shutdown tears the radio down
// either way. Deferrable events already queued (reader.exit) ride along in
// the same drain.
static void deliverSleepPluginEvents(const bool allowNetwork) {
  // Activity-owned state must be queued before sleep.enter and before this
  // same-sleep drain. The hook is idempotent with ordinary activity teardown.
  activityManager.prepareForSleep();

  // Sleeping straight out of a book is the common flow, but the reader's own
  // reader.exit only fires later, inside goToSleep() — after this drain. Carry
  // the book and progress on sleep.enter itself so a sync handler bound to it
  // pushes current progress on THIS connection, not the next one.
  pluginevents::Var vars[2];
  size_t varCount = 0;
  char percent[8];
  const ScreenshotInfo info = activityManager.getScreenshotInfo();
  if (info.readerType != ScreenshotInfo::ReaderType::None && !APP_STATE.openEpubPath.empty()) {
    snprintf(percent, sizeof(percent), "%d", info.progressPercent);
    vars[varCount++] = {"book", APP_STATE.openEpubPath.c_str()};
    vars[varCount++] = {"percent", percent};
  }
  pluginevents::emit(pluginevents::Event::SleepEnter, vars, varCount);
  if (!allowNetwork) return;
  if (WiFi.status() == WL_CONNECTED) {
    pluginevents::drain(&renderer);
    return;
  }
  // Any connect-flagged queued event justifies the join, not only
  // sleep.enter: reader.session is queued while reading and delivered on this
  // same sleep, and a progress-sync plugin usually subscribes to it alone.
  if (!pluginevents::wantsConnectAny()) return;
  if (powerManager.getBatteryPercentage() < 20) return;
  // Sleep can be the first WiFi use since boot or deep-sleep wake.
  WIFI_STORE.loadFromFile();
  const auto cred = WIFI_STORE.findCredential(WIFI_STORE.getLastConnectedSsid());
  if (!cred) return;

  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  WiFi.mode(WIFI_STA);
  WiFi.begin(cred->ssid.c_str(), cred->password.c_str());
  const unsigned long joinDeadline = millis() + 10000;
  while (WiFi.status() != WL_CONNECTED && millis() < joinDeadline) {
    delay(100);
  }
  if (WiFi.status() == WL_CONNECTED) {
    trustedtime::startSync();  // snap the clock floor while the network is up
    pluginevents::drain(&renderer);
  } else {
    LOG_DBG("MAIN", "Sleep-event WiFi join timed out; deferring delivery");
  }
}

// Shared final hardware shutdown for EVERY deep-sleep entry: the one owner of
// the WiFi/tilt/display/storage tear-down and the HAL sleep handover. No power
// lock inside — each caller holds its own HalPowerManager::Lock. timerWakeUs is
// PR1's optional timer wake (0 = power button only).
void enterHardwareDeepSleep(uint64_t timerWakeUs = 0);
#if FREEINK_DEVICE_PICCO
// Charge strip along the bottom of the retained sleep screen.
static void drawSleepChargeStrip(const bool charging, const uint16_t percent) {
  RenderLock lock;
  const int h = renderer.getLineHeight(SMALL_FONT_ID) + 12;
  const int y = renderer.getScreenHeight() - h;
  renderer.fillRect(0, y, renderer.getScreenWidth(), h, false);
  char text[48];
  snprintf(text, sizeof(text), "%s  %u%%", charging ? tr(STR_CHARGING) : tr(STR_CHARGED), percent);
  renderer.drawCenteredText(SMALL_FONT_ID, y + 6, text);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

// Shows the charge strip while the power HAL holds sleep on USB power, and
// removes it again if USB goes away. Returns true when the power button ended
// the hold.
static bool holdSleepWhileUsbPowered() {
  if (!gpio.isUsbConnected()) return false;
  // Keep the sleep frame so unplugging can remove the strip. 48 KB of PSRAM,
  // freed when the hold ends.
  const size_t frameSize = renderer.getBufferSize();
  auto* const sleepFrame = static_cast<uint8_t*>(heap_caps_malloc(frameSize, MALLOC_CAP_SPIRAM));
  if (sleepFrame) memcpy(sleepFrame, renderer.getFrameBuffer(), frameSize);
  const bool buttonPressed = powerManager.holdWhileUsbPowered(gpio, drawSleepChargeStrip);
  if (sleepFrame) {
    if (!buttonPressed) {
      RenderLock lock;
      memcpy(renderer.getFrameBuffer(), sleepFrame, frameSize);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    }
    heap_caps_free(sleepFrame);
  }
  return buttonPressed;
}
#endif

// Enter deep sleep mode
#ifdef CROSSPOINT_COMPANION_PILOT_AUTOWAKE
// P1-B8 (experimental): the one-shot autonomous BLE wake pilot. An ordinary
// sleep entry may arm ONE delayed headless gate window. The hardware timer is
// pre-armed BEFORE the RTC arm record is written, so a rejected timer request
// can never leave an armed record without its wake source (fail closed); the
// record is written only once the timer is installed. startDeepSleep re-issues
// the identical arming request at sleep entry, and the IDF result for one
// request is deterministic. The pilot wake dispatches through the unchanged
// companion-gate block; no exit of the window re-arms.
static uint64_t armCompanionPilot(const bool slideshowSleep) {
  companionpilot::Inputs pilotInputs{};
  pilotInputs.boardSupported = BoardConfig::isX4Classic();
  pilotInputs.usbConnected = gpio.isUsbConnected();
  // The same slideshow condition the wake dispatch pre-empts on, plus the
  // interval timer this very sleep may arm.
  pilotInputs.slideshowSleep = slideshowSleep || slideshow::hasValidRetainedState();
  companionidentity::StoredIdentity pilotIdentity{};
  pilotInputs.identityValid = companionidentity::load(pilotIdentity);
  pilotInputs.batteryOk = slideshow::allowedByBattery(powerManager.getBatteryPercentage(), pilotInputs.usbConnected);
  const companionpilot::Decision pilotDecision = companionpilot::evaluate(pilotInputs);
  if (pilotDecision.action != companionpilot::Action::Arm) {
    LOG_INF("GATE", "pilot not armed: %s", companionpilot::skipReasonName(pilotDecision.reason));
    return 0;
  }
  if (!powerManager.armTimerWake(companionpilot::kPilotTimerSeconds * 1000000ULL)) {
    LOG_ERR("GATE", "pilot timer wake rejected; the pilot stays off");
    return 0;
  }
  companiongate::arm(false);  // one non-diagnostic arm; overwrites the shared RTC verdict bytes
  LOG_INF("GATE", "pilot armed: one BLE window in %u s", static_cast<unsigned>(companionpilot::kPilotTimerSeconds));
  return static_cast<uint64_t>(companionpilot::kPilotTimerSeconds) * 1000000ULL;
}
#endif

void enterDeepSleep(bool fromTimeout = false) {
  HalPowerManager::Lock powerLock;  // Ensure we are at normal CPU frequency for sleep preparation
  APP_STATE.lastSleepFromReader = activityManager.isReaderActivity();

  // Sleep may end in a power-off (battery death, latch); persist the clock
  // floor now so a later cold boot resumes from it.
  trustedtime::note();

  // Stop Bluetooth before plugin delivery can start WiFi; a timeout defers delivery.
  deepSleepInProgress = true;
  const bool bluetoothStopped = bleturner::beforeSleep(2000);
  if (!bluetoothStopped) LOG_ERR("SLP", "Bluetooth did not stop; deferring sleep-event delivery");
  deliverSleepPluginEvents(bluetoothStopped);

  // ONE timeout-quick-resume precedence policy (slideshow::timeoutQuickResume,
  // host-tested): Sleep Screen = Slideshow keeps priority over the
  // after-timeout upgrade, so no stale Quick Resume frame is saved for a
  // sleep that will render a slideshow frame instead.
  const bool isQuickResumeSleep =
      slideshow::timeoutQuickResume(SETTINGS.sleepScreen, fromTimeout, SETTINGS.quickResumeSleepScreen);
  // Every sleep mode leaves a complete retained frame on the e-ink panel. Keep
  // it visible until the first useful reader or home paint replaces it.
  APP_STATE.showBootScreen = false;

  APP_STATE.saveToFile();

  activityManager.goToSleep(fromTimeout);

  if (isQuickResumeSleep) {
    saveSleepFrameBuffer();
  } else if (Storage.exists(SLEEP_FRAME_FILE)) {
    // A stale Quick Resume frame must not replace the selected sleep screen during wake.
    Storage.remove(SLEEP_FRAME_FILE);
  }

  // Sleep Screen = Slideshow: the sleep activity prepared (and armed) the
  // slideshow frame during its render phase when the source was usable; a
  // valid Sleep-mode state now arms the persisted interval timer. Anything
  // else — a broken/empty slideshow source or another screen mode — sleeps
  // the ordinary way with no timer.
  const bool slideshowSleep = slideshow::sleepSlideshowSupported() &&
                              SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::SLIDESHOW &&
                              slideshow::hasValidRetainedState() &&
                              slideshow::getRetainedMode() == slideshow::Mode::Sleep;
  uint64_t pilotTimerUs = 0;
#ifdef CROSSPOINT_COMPANION_PILOT_AUTOWAKE
  pilotTimerUs = armCompanionPilot(slideshowSleep);
#endif
  enterHardwareDeepSleep(slideshowSleep ? slideshow::intervalMicros(SETTINGS.slideshowInterval) : pilotTimerUs);
}

void enterHardwareDeepSleep(uint64_t timerWakeUs) {
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }

#if FREEINK_DEVICE_PICCO
  // The I2C LED driver keeps its state through deep sleep; switch it off here
  // (SETTINGS.frontlightOn keeps the preference for restore-on-wake).
  Frontlight.setOn(false);
  const bool wakeOnHeldButton = holdSleepWhileUsbPowered();
#else
  const bool wakeOnHeldButton = false;
#endif

  halTiltSensor.deepSleep();
  display.deepSleep();
  Storage.prepareForDeepSleep();
  LOG_DBG("MAIN", "Entering deep sleep");

  powerManager.startDeepSleep(gpio, wakeOnHeldButton, timerWakeUs);
}

// Diagnostic cap for the headless companion gate window. The production
// deadline and wake schedule come from the M1–M8 measurements
// (p1-companion-gate preflight §4); nothing here is a production interval.
constexpr unsigned long GATE_WINDOW_MS = 15000;

// The ghost-wake sleep shape, safe before Storage.begin() and any display
// init: SDCard.shutdown() no-ops when never mounted, the display controller
// stays in its retained deep sleep, and the ESP rail keep-alive keys on the
// (zero) timer argument. Never returns.
// P1-B9 zero-console rule (analysis revision 3 §5.5): the standby battery
// phase must issue no console bytes, so the gate's own routine lines are
// compiled out of the standby env — the diagnostic buffer records the
// equivalent events (transitions, snapshots, exit flags). Error paths keep
// their LOG_ERR; non-standby envs are unchanged.
#ifdef CROSSPOINT_COMPANION_GATE_STANDBY
#define GATE_LOGI(...) ((void)0)
#else
#define GATE_LOGI(...) LOG_INF("GATE", __VA_ARGS__)
#endif

static void sleepAfterGate() {
  Storage.prepareForDeepSleep();
  powerManager.startDeepSleep(gpio, false, 0);
}

// The bounded BLE window between a timer wake and the next deep sleep. Runs
// before Storage.begin(): no SD, no display, no fonts, no ActivityManager, no
// Wi-Fi, no page-turner runtime. Slice A performs no work — the window exists
// to measure wake→session latency and heap cost, and every exit is
// deterministic: back to sleep (timeout / error / no identity / no bond), or
// a Power abort that continues the ordinary interactive boot. The arm was
// already consumed by the dispatcher; no exit ever re-arms a timer.
static void runCompanionGate(const bool diagnostic) {
  companionidentity::StoredIdentity identity{};
  if (!companionidentity::load(identity)) {
    GATE_LOGI("No stored companion identity; the radio stays off");
    companiongate::updateRunOutcome(companiongate::RunOutcome::NoIdentity);
    sleepAfterGate();
  }

  blecompanion::BleCompanion gate;
  // Persistence no-ops: a headless window must never write SETTINGS (the SD
  // is not mounted here); identity changes happen only in real sessions.
  if (!gate.begin(identity.addr, identity.type, [](const char*, uint8_t) {}, []() {})) {
    LOG_ERR("GATE", "The companion radio did not start; returning to sleep");
    companiongate::updateRunOutcome(companiongate::RunOutcome::RadioStartFailed);
    gate.stop();
    sleepAfterGate();
  }

  // mirror ∩ bond: a named peer without a secure bond record cannot
  // authenticate — end the window instead of advertising pointlessly.
  if (!companionidentity::gateMayProceed(true, gate.storedIdentityBonded())) {
    GATE_LOGI("No secure bond for the stored identity; returning to sleep");
    companiongate::updateRunOutcome(companiongate::RunOutcome::NoBond);
    gate.stop();
    sleepAfterGate();
  }

#ifdef CROSSPOINT_COMPANION_GATE_STANDBY
  GATE_LOGI("Gate window open (standby, cap %lu ms)", standbydiag::standbyCapMs());
#else
  GATE_LOGI("Gate window open for %lu ms%s", GATE_WINDOW_MS, diagnostic ? " (diagnostic USB override)" : "");
#endif
  // P1-B2a (experimental build flag): automatic light sleep across the
  // window. The guard pairs the configure with the sample-and-restore on
  // every exit; the poll period below follows whether the window can sleep.
  companiongatepower::Guard gateWindow(gate);
#ifdef CROSSPOINT_COMPANION_GATE_AUTOLS
  GATE_LOGI("Gate window automatic light sleep %s, poll %u ms",
            companiongatepower::autoLightSleepEnabled() ? "on" : "OFF (control build)",
            companiongatepower::gatePollMs(gateWindow.configured()));
#endif
#ifdef CROSSPOINT_COMPANION_GATE_STANDBY
  // P1-B9: the standby session replaces the bounded window. The loop runs
  // until an explicit exit — power button (existing), USB plug (approved
  // policy: USB reclaims the machine for interactive use) — or the 24 h
  // diagnostic cap. No exit re-arms anything; the arm was consumed above.
  // The deadline arithmetic is safe only because the gate always runs on a
  // reset-fresh millis() (every entry follows a deep-sleep wake).
  const unsigned long deadline = millis() + standbydiag::standbyCapMs();
  standbydiag::sessionBegin(millis());
#else
  const unsigned long deadline = millis() + GATE_WINDOW_MS;
#endif
  bool sessionLogged = false;
  while (millis() < deadline) {
    gate.loop();
    if (!sessionLogged && gate.status().sessionActive) {
      sessionLogged = true;
      // The M3 fact must outlive the deep sleep that ends this window: every
      // exit path below (power abort, timeout) must find it already stored.
      companiongate::markSessionEstablished();
      GATE_LOGI("Session established (M3 measurement point)");
    }
#ifdef CROSSPOINT_COMPANION_GATE_STANDBY
    standbydiag::tick(millis(), static_cast<uint8_t>(gate.status().state));
    if (gpio.isUsbConnected()) {
      GATE_LOGI("USB connected: exiting standby into the ordinary boot");
      // The USB exit records the same RunOutcome::PowerAbort shape as the
      // power exit (the RTC verdict keeps its layout; the true driver lives
      // in the standby buffer's exit flag) and returns into the ordinary
      // boot.
      standbydiag::noteExit(standbydiag::ExitKind::Usb);
      companiongate::updateRunOutcome(companiongate::RunOutcome::PowerAbort);
      gateWindow.finish(companiongatepower::WindowPowerAbort);
      gate.stop();
      return;
    }
#endif
    if (gpio.isPowerButtonPhysicallyPressed()) {
      GATE_LOGI("Power pressed: aborting the gate window");
#ifdef CROSSPOINT_COMPANION_GATE_STANDBY
      standbydiag::noteExit(standbydiag::ExitKind::Power);
#endif
      companiongate::updateRunOutcome(companiongate::RunOutcome::PowerAbort);
      gateWindow.finish(companiongatepower::WindowPowerAbort);
      gate.stop();
      // The ordinary boot consumes the release frame (the loop's wake-release
      // handling); the press must never act as a button event.
      wakePowerReleasePending = true;
      return;
    }
    delay(companiongatepower::gatePollMs(gateWindow.configured()));
  }

#ifdef CROSSPOINT_COMPANION_GATE_STANDBY
  // The loop ended on the 24 h cap. The physical Power re-check below still
  // allows the ordinary boot, where the buffered evidence stays recoverable,
  // so the cap exit is noted only once the sleep handoff actually proceeds.
#endif
  companiongate::updateRunOutcome(companiongate::RunOutcome::WindowTimeout);
  gateWindow.finish(companiongatepower::WindowTimeout);
  gate.stop();
  // Physical Power re-check immediately before the sleep handoff, to shrink
  // the race between the last poll and startDeepSleep().
  if (gpio.isPowerButtonPhysicallyPressed()) {
    GATE_LOGI("Power pressed at the sleep handoff: aborting the gate window");
#ifdef CROSSPOINT_COMPANION_GATE_STANDBY
    standbydiag::noteExit(standbydiag::ExitKind::Power);
#endif
    companiongate::updateRunOutcome(companiongate::RunOutcome::PowerAbort);
    wakePowerReleasePending = true;
    return;
  }
#ifdef CROSSPOINT_COMPANION_GATE_STANDBY
  // The 24 h cap ends in deep sleep: the RAM evidence is expected to be lost
  // (analysis revision 3) — the session is INCONCLUSIVE unless a console is
  // actually attached here, and no stale evidence can ever be flushed later.
  standbydiag::noteExit(standbydiag::ExitKind::Cap);
#endif
  GATE_LOGI("Gate window closed; returning to sleep");
  sleepAfterGate();
}

void setupDisplayAndFonts(bool seamless = false) {
#if !FREEINK_MCU_C3
  // C3 resolves its controller in HalGPIO::begin() before SPI claims the
  // display pins. X4 Pro skips that C3-only path, so probe here before
  // display.begin() selects and initializes its panel driver.
  static bool controllerResolved = false;
  if (!controllerResolved) {
    controllerResolved = true;
    if (freeink::applyXteinkDisplayController()) {
      LOG_DBG("MAIN", "Panel controller: UltraChip UC81xx variant detected");
    }
  }
#endif

  display.begin(seamless);
  renderer.begin();
  activityManager.begin();
  LOG_DBG("MAIN", "Display initialized");

  // Initialize font decompressor for compressed reader fonts
  if (!fontDecompressor.init()) {
    LOG_ERR("MAIN", "Font decompressor init failed");
  }
  fontCacheManager.setFontDecompressor(&fontDecompressor);
  renderer.setFontCacheManager(&fontCacheManager);
  renderer.insertFont(NOTOSANS_14_FONT_ID, notosans14FontFamily);
#ifndef OMIT_FONTS
  renderer.insertFont(NOTOSANS_12_FONT_ID, notosans12FontFamily);
  renderer.insertFont(NOTOSANS_16_FONT_ID, notosans16FontFamily);
  renderer.insertFont(NOTOSANS_18_FONT_ID, notosans18FontFamily);
#endif  // OMIT_FONTS
  renderer.insertFont(UI_10_FONT_ID, ui10FontFamily);
  renderer.insertFont(UI_12_FONT_ID, ui12FontFamily);
  renderer.insertFont(SMALL_FONT_ID, smallFontFamily);

  // Discover and load SD card fonts
  sdFontSystem.begin(renderer);

  LOG_DBG("MAIN", "Fonts setup");
}

void setup() {
  BoardConfig::holdPowerRails();

#ifdef ENABLE_SERIAL_LOG
#ifdef CROSSPOINT_WAIT_FOR_USB_SERIAL
  // Development builds preserve reliable early CDC logs; release builds let
  // enumeration proceed asynchronously so users do not pay this startup cost.
  delay(250);
#endif
  Serial.begin(115200);
#if LOG_SERIAL_HAS_TX_TIMEOUT
  logSerial.setTxTimeoutMs(1);  // This is a load-bearing 1. Do not modify.
#endif
#endif

  HalSystem::begin();
  // checkPanic() clears the watchdog capture marker after a successful SD
  // dump, so retain the boot classification for the later activity route.
  const bool rebootedFromPanic = HalSystem::isRebootFromPanic();

  // Read-and-clear so a panic later in setup() doesn't loop into silent reboot.
  // Bound the target range too — RTC_NOINIT memory is uninitialized on cold boot.
  const bool isSilentReboot = (silentRebootMagic == SILENT_REBOOT_MAGIC);
  const uint32_t snapshotTarget =
      (isSilentReboot && silentRebootTarget <= SILENT_REBOOT_TARGET_MAX) ? silentRebootTarget : 0;
  const bool silentRebootLightOn = isSilentReboot && (silentRebootPayload & SILENT_REBOOT_LIGHT_ON) != 0;
  silentRebootMagic = 0;
  silentRebootTarget = 0;
  silentRebootPayload = 0;

  gpio.begin();
  powerManager.begin();

  // Board TX-power ceiling. It can only be set once WiFi has started, so it is
  // applied on every STA/AP start rather than at each place that brings WiFi up.
  if (BoardConfig::ACTIVE.wifiMaxTxPower != 0) {
    const auto applyTxCap = [](arduino_event_id_t, arduino_event_info_t) {
      const esp_err_t err = esp_wifi_set_max_tx_power(static_cast<int8_t>(BoardConfig::ACTIVE.wifiMaxTxPower));
      if (err != ESP_OK) LOG_ERR("WIFI", "TX power cap failed: %d", err);
    };
    WiFi.onEvent(applyTxCap, ARDUINO_EVENT_WIFI_STA_START);
    WiFi.onEvent(applyTxCap, ARDUINO_EVENT_WIFI_AP_START);
  }

  const auto wakeupReason = gpio.getWakeupReason();
  // Sample the wake hold now — a click wake is released within milliseconds of
  // boot — but defer the sleep-or-boot decision until SETTINGS is loaded below:
  // click-to-wake is a setting, and an X4 battery power-off cuts all power, so
  // only SD state survives to the next boot.
  // The Metalio boots after the button is released, so it skips the check.
  const bool wakeHoldVerified = wakeupReason != HalGPIO::WakeupReason::PowerButton || BoardConfig::isMetalioEInk4() ||
                                gpio.verifyPowerButtonWakeup();

  // X4 Pro and X4 Classic both map BTN_UP to GPIO0 — an ESP32-S3 boot strap — so
  // gate recovery on the non-strap Down key (GPIO7) to avoid a stuck-in-recovery loop.
  const auto recoveryButton = (BoardConfig::isX4Pro() || BoardConfig::isX4Classic()) ? MappedInputManager::Button::Down
                                                                                     : MappedInputManager::Button::Up;
  // The Picco has no second app slot to flash into (partitions_picco.csv).
  const bool recoveryFirmwareMode = wakeupReason == HalGPIO::WakeupReason::PowerButton && !BoardConfig::isPaperMono() &&
                                    !BoardConfig::isPicco() && mappedInputManager.isPressed(recoveryButton);

  // Sleep Companion gate (p1-companion-gate preflight, Slice A): the single
  // early dispatch decision, taken before Storage.begin() so a dispatching
  // wake never mounts the SD, touches the display, or starts any radio except
  // the gate's own BLE window. The policy is pure and host-tested; the arm is
  // one-shot and every non-dispatching wake clears it (fail closed).
  // companion-gate-begin
  const companiongate::WakeKind gateWake =
      wakeupReason == HalGPIO::WakeupReason::Timer ? companiongate::WakeKind::Timer : companiongate::WakeKind::Other;
  const bool gateArmed = companiongate::hasArmedWork();
  const bool gateUsbConnected = gpio.isUsbConnected();
  // The cheap-input guard below is deliberately BROADER than the policy's Run
  // precondition (it omits slideshowHeld and boardSupported): drifting apart
  // can then only waste an NVS read and an ADC sample, never misclassify a
  // runnable gate as a sleep.
  bool gateIdentityValid = false;
  bool gateBatteryOk = false;
  if (gateArmed && gateWake == companiongate::WakeKind::Timer && !recoveryFirmwareMode) {
    companionidentity::StoredIdentity gateIdentity{};
    gateIdentityValid = companionidentity::load(gateIdentity);
    gateBatteryOk = slideshow::allowedByBattery(powerManager.getBatteryPercentage(), gateUsbConnected);
  }
  companiongate::Inputs gateInputs{};
  gateInputs.wake = gateWake;
  gateInputs.recoveryMode = recoveryFirmwareMode;
  gateInputs.boardSupported = BoardConfig::isX4Classic();
  gateInputs.slideshowHeld = slideshow::hasValidRetainedState();
  gateInputs.gateArmed = gateArmed;
  gateInputs.identityValid = gateIdentityValid;
  gateInputs.batteryOk = gateBatteryOk;
  gateInputs.usbConnected = gateUsbConnected;
#ifdef CROSSPOINT_WAIT_FOR_USB_SERIAL
  gateInputs.diagnosticOverride = companiongate::armedIsDiagnostic();
#else
  gateInputs.diagnosticOverride = false;
#endif
  const companiongate::Action gateAction = companiongate::dispatch(gateInputs);
  // One-line dispatch trace: every input the policy saw plus its decision, so
  // a refused wake names its cause instead of returning to sleep unexplained.
  LOG_INF("GATE", "dispatch: wake=%s armed=%d diag=%d recovery=%d usb=%d slideshow=%d identity=%d battery=%d -> %s",
          gateWake == companiongate::WakeKind::Timer ? "timer" : "other", gateArmed, gateInputs.diagnosticOverride,
          gateInputs.recoveryMode, gateInputs.usbConnected, gateInputs.slideshowHeld, gateInputs.identityValid,
          gateInputs.batteryOk, companiongate::gateActionName(gateAction));
  // The verdict rides in the consumed arm record and is only stored on timer
  // wakes, so the last timer-wake decision survives the interactive boots that
  // follow it. The stores compile in every build; GATESTAT, the only reader,
  // exists in the AUTOLS diagnostic builds.
  const companiongate::DispatchVerdict verdict{gateWake == companiongate::WakeKind::Timer,
                                               gateArmed,
                                               gateInputs.diagnosticOverride,
                                               gateInputs.recoveryMode,
                                               gateInputs.usbConnected,
                                               gateInputs.slideshowHeld,
                                               gateInputs.identityValid,
                                               gateInputs.batteryOk,
                                               gateAction,
                                               companiongate::RunOutcome::NotRun,
                                               false};
  if (gateAction == companiongate::Action::Run) {
    bool gateDiagnostic = false;
    companiongate::takeArmedWork(gateDiagnostic);
    companiongate::storeDispatchVerdict(verdict);
    runCompanionGate(gateDiagnostic);
  } else if (gateAction == companiongate::Action::SleepNoRadio) {
    // Policy contract (p1-companion-gate): armed but unsafe to run — no
    // identity or a low battery. Consume the arm and go straight back to
    // sleep, power-button-only; never the ordinary boot.
    companiongate::clearArm();
    companiongate::storeDispatchVerdict(verdict);
    sleepAfterGate();
  } else {
    if (gateArmed) companiongate::clearArm();
    // StandDown/SlideshowWins/Ordinary: only an ARMED timer wake stores here —
    // an unarmed timer wake (e.g. the sleep-screen slideshow) must not
    // overwrite the preserved verdict of the last armed gate, and a button or
    // USB wake never stores at all.
    if (gateArmed && gateWake == companiongate::WakeKind::Timer) companiongate::storeDispatchVerdict(verdict);
  }
  // companion-gate-end

  // Dev-only RTC slow-clock diagnostic (p0-rtc-clock): re-applies the
  // scratch-selected clock on every boot and, when a measurement loop is
  // armed, logs the finished cycle and sleeps again instead of booting.
#ifdef CROSSPOINT_RTC_CLK_DIAG
  rtcclockdiag::begin(powerManager, gpio, wakeupReason);
#endif

  // Dev-only BLE light-sleep diagnostic (P1-B0a): controller sleep clock,
  // RTC confirmation, and esp_pm automatic light sleep.
#ifdef CROSSPOINT_BLE_LIGHT_SLEEP_DIAG
  blelightsleepdiag::begin();
#endif

  // Dev-only PM diagnostic (p1-b1k/b1l/b1n): DFS range with automatic light
  // sleep per CROSSPOINT_PM_AUTOLS — the minimal PM initialization.
#ifdef CROSSPOINT_PM_ONLY_DIAG
  pmonlydiag::begin();
#endif

  halTiltSensor.begin();
  halClock.begin();

#if FREEINK_DEVICE_X4 || FREEINK_DEVICE_X3
  LOG_INF("MAIN", "Hardware detect: %s", gpio.deviceIsX3() ? "X3" : "X4");
#else
  LOG_INF("MAIN", "Device: %s", BoardConfig::ACTIVE.name);
#endif

  // SD Card Initialization
  // We need 6 open files concurrently when parsing a new chapter
  if (!Storage.begin()) {
    LOG_ERR("MAIN", "SD card initialization failed");
    setupDisplayAndFonts(isSilentReboot);
    activityManager.goToFullScreenMessage("SD card error", EpdFontFamily::BOLD);
    return;
  }

  HalSystem::checkPanic();

  APP_STATE.loadFromFile();
  const bool isSleepWake = wakeupReason == HalGPIO::WakeupReason::PowerButton;
  const bool isPersistedSleepWake = isSleepWake && !APP_STATE.showBootScreen;

  if (recoveryFirmwareMode) {
    LOG_INF("MAIN", "Recovery firmware mode (%s + POWER held at boot)",
            (BoardConfig::isX4Pro() || BoardConfig::isX4Classic()) ? "DOWN" : "UP");
  }

  // Touch boards default the reader menu to the toolbar overlay instead of the
  // full-screen list. Seeded before the load: fromJson() falls back to the
  // in-memory value only when the file carries no readerMenuStyle key, so a
  // user's saved choice (either style) still wins.
  if (gpio.hasTouch()) {
    SETTINGS.readerMenuStyle = CrossPointSettings::READER_MENU_TOOLBAR;
  }
  SETTINGS.loadFromFile();
  // Push the saved timezone's POSIX rule into the clock (migrating the legacy
  // UTC-offset setting on first boot after the update).
  timezones::applyToClock();
  RECENT_BOOKS.loadFromFile();
  I18N.setLanguage(static_cast<Language>(SETTINGS.language));
  KOREADER_STORE.loadFromFile();
  OPDS_STORE.loadFromFile();
  OPDS_TOKENS.loadFromFile();
  LCP_PASSPHRASES.loadFromFile();
  UITheme::getInstance().reload();
  ButtonNavigator::setMappedInputManager(mappedInputManager);
  pluginevents::refreshSubscriptions();
  // Sleep Companion identity mirror: migrate mirror-if-absent (the gate reads
  // NVS only; SETTINGS stays the user-visible record — a valid mirror is
  // never overwritten from it).
  companionidentity::migrateFromSettings(SETTINGS.companionPeerAddr, SETTINGS.companionPeerAddrType);
  // Seed the system clock from the battery-backed RTC: on RTC boards it is the
  // only real clock across power-off, and time() is what plugin event
  // timestamps, TLS and file dates read. Placed before trustedtime::init(),
  // whose floor restore vetoes a lagging RTC and restores the floor instead.
  struct tm rtcLocal;
  if (halClock.localTime(rtcLocal)) {
    // localTime() renders the RTC's UTC epoch under the TZ rule applied above,
    // so mktime() inverts it exactly (tm_isdst included).
    trustedtime::adopt(static_cast<int64_t>(mktime(&rtcLocal)));
    LOG_INF("CLK", "RTC clock read: %04d-%02d-%02d %02d:%02d", rtcLocal.tm_year + 1900, rtcLocal.tm_mon + 1,
            rtcLocal.tm_mday, rtcLocal.tm_hour, rtcLocal.tm_min);
  }
  // Restore the monotonic clock floor before anything reads time() (event
  // timestamps, loan-expiry checks).
  trustedtime::init();
  bleturner::begin(pageturner::host, SETTINGS.ble);

  // Brightness and warmth are always restored. A normal wake starts with the
  // light off unless Restore Light on Wake is enabled; silent maintenance
  // reboots replay the live state captured at restart, so they neither go dark
  // nor light up against the user's wake preference. A slide switch bound to
  // the light decides it from its position instead.
  const bool restoreLightOn = isSilentReboot ? silentRebootLightOn
                              : slideSwitchOwnsLight()
                                  ? gpio.isToggleSwitchOn()
                                  : (SETTINGS.frontlightOn != 0 && SETTINGS.frontlightRestoreOnWake != 0);
  Frontlight.begin(SETTINGS.frontlightBrightness, SETTINGS.frontlightWarmth, restoreLightOn);

  switch (wakeupReason) {
    case HalGPIO::WakeupReason::PowerButton:
      // With Short Power Button Press = Sleep, a single click wakes on any
      // device; otherwise the button must still be held (ghost-wake debounce).
      // Picco: plugging in USB pulses the wake line; power on, as stock does.
      if (!wakeHoldVerified && SETTINGS.shortPwrBtn != CrossPointSettings::SHORT_PWRBTN::SLEEP &&
          !(FREEINK_DEVICE_PICCO && gpio.isUsbConnected())) {
        LOG_DBG("MAIN", "Power-button wake not held through verification, sleeping");
        Storage.prepareForDeepSleep();
        powerManager.startDeepSleep(gpio);
      }
      wakePowerReleasePending = true;
      break;
    case HalGPIO::WakeupReason::Timer:
      // A timer wake is classified information only: it boots normally like
      // any non-power-button wake until a consumer is added.
      LOG_DBG("MAIN", "Wakeup reason: Timer");
      break;
    case HalGPIO::WakeupReason::AfterUSBPower:
      // Most devices return to sleep after a USB-powered cold boot.
      LOG_DBG("MAIN", "Wakeup reason: After USB Power");
#if FREEINK_DEVICE_X4PRO || FREEINK_DEVICE_X4CLASSIC || FREEINK_DEVICE_PAPERMONO || FREEINK_DEVICE_EEGO_A4 || \
    FREEINK_DEVICE_METALIO_EINK4 || FREEINK_DEVICE_PICCO
      // X4 Pro must stay awake so USB Serial/JTAG remains available after leaving
      // USB Drive and reconnecting the cable. Paper Mono has no armable GPIO wake
      // (its button is behind the PMIC). Metalio also needs native USB available
      // after a USB-powered boot, as does the Picco. EEGO A4's post-flash reset reads as
      // POWERON (native-USB), so a flash would otherwise be misclassified as a
      // USB-power cold boot and sleep. Sleeping any of these here would strand
      // the device in a USB-replug boot loop (or sleep right after a flash).
      break;
#else
      Storage.prepareForDeepSleep();
      powerManager.startDeepSleep(gpio);
      break;
#endif
    case HalGPIO::WakeupReason::AfterFlash:
      // After flashing, just proceed to boot
    case HalGPIO::WakeupReason::Other:
    default:
      break;
  }

  LOG_DBG("MAIN", "Starting CrossPoint version " CROSSPOINT_VERSION);

  // Slideshow lifecycle: only a genuine timer wake with valid retained state
  // resumes it; every other boot cancels any armed slideshow (fail closed).
  const bool slideshowTimerWake = wakeupReason == HalGPIO::WakeupReason::Timer && slideshow::hasValidRetainedState();
  if (wakeupReason != HalGPIO::WakeupReason::Timer) {
    slideshow::clearRetainedState();
  }

  // Managed slideshow resume: construct the activity BEFORE display init so an
  // allocation failure can fall back to an ordinary Timer boot and the
  // seamless display decision can account for a successful construction. The
  // retained mode picks the continuation: Viewer re-opens the image viewer
  // (Power exits Home, proven); Sleep continues the sleep-screen slideshow
  // inside SleepActivity (Power wake stays the ordinary wake contract). The
  // constructors only store refs/paths; neither renders nor needs
  // ActivityManager::begin().
  std::unique_ptr<Activity> slideshowResumeActivity;
  if (slideshowTimerWake) {
    if (slideshow::getRetainedMode() == slideshow::Mode::Viewer) {
      slideshowResumeActivity =
          makeUniqueNoThrow<BmpViewerActivity>(renderer, mappedInputManager, slideshow::getRetainedPath(),
                                               /*slideshowResume=*/true);
    } else {
      slideshowResumeActivity = makeUniqueNoThrow<SleepActivity>(renderer, mappedInputManager, /*fromTimeout=*/false,
                                                                 /*slideshowContinue=*/true);
    }
    if (!slideshowResumeActivity) {
      LOG_ERR("MAIN", "OOM: slideshow resume activity");
      slideshow::clearRetainedState();
    }
  }

  // Apply a switch moved during sleep before the first paint, so the wake
  // screen does not draw in the old night mode and then repaint.
  handleSlideSwitch();

  // Resolve the single boot-presentation decision. Skipping the splash also
  // skips the panel-clearing pass and the X3 initial-full-sync arming (see
  // HalDisplay::begin), so the first paint is FAST_REFRESH (~500ms) over the
  // retained frame and input dispatches against a visible UI.
  // Only a verified deep-sleep wake may use the one-shot persisted flag.
  // Otherwise a stale flag could suppress the splash on a cold boot.
  const BootResume resume = isSilentReboot         ? BootResume::Silent
                            : isPersistedSleepWake ? BootResume::SplashlessWake
                                                   : BootResume::Splash;
  bool allowFastInitialReaderRefresh = false;
  bool needsWakeRefresh = false;

  setupDisplayAndFonts(resume != BootResume::Splash || slideshowResumeActivity != nullptr);

  if (slideshowResumeActivity) {
    // Managed slideshow resume: no splash, no Home routing. The activity's
    // onEnter() picks the next frame, re-arms the retained state and renders
    // it through the canonical path; the main loop performs the sleep.
    activityManager.replaceActivity(std::move(slideshowResumeActivity));
    allowSleepAt = millis() + 2000;
    return;
  }

  switch (resume) {
    case BootResume::Silent:
      // Splash skipped: the routing block below picks the target activity; the
      // panel keeps showing the pre-reboot popup until that first paint lands.
      break;
    case BootResume::SplashlessWake:
      // One-shot flag: re-arm the splash for the next ordinary boot. Save
      // before any painting so a hang in the blocking paint path can't strand
      // us in a splashless-with-no-frame loop on the next boot.
      APP_STATE.showBootScreen = true;
      APP_STATE.saveToFile();
      if (Storage.exists(SLEEP_FRAME_FILE) && loadSleepFrameBuffer()) {
        if (gpio.deviceIsX3()) {
          // begin() clears the X3 controller RAM, so restore the saved frame as
          // the baseline for the first reader paint without refreshing the panel.
          renderer.cleanupGrayscaleWithFrameBuffer();
          allowFastInitialReaderRefresh = true;
        }
      } else {
        // Clean the retained sleep image as part of the first Home paint.
        needsWakeRefresh = true;
      }
      break;
    case BootResume::Splash:
      activityManager.goToBoot();
      break;
  }

  // Output polarity is resolved per render by ActivityManager (night mode
  // inverts only the reading surfaces), so nothing to restore here.

  if (recoveryFirmwareMode) {
    // Skip normal home/reader routing: jump straight into the SD firmware picker.
    activityManager.replaceActivity(
        std::make_unique<SdFirmwareUpdateActivity>(renderer, mappedInputManager, /*recoveryMode=*/true));
  } else if (rebootedFromPanic) {
    // If we rebooted from a panic, go to crash report screen to show the panic info
    activityManager.goToCrashReport();
  } else if (resume == BootResume::Silent && snapshotTarget == SILENT_REBOOT_TARGET_READER &&
             !APP_STATE.openEpubPath.empty()) {
    activityManager.goToReader(APP_STATE.openEpubPath);
  } else if (resume == BootResume::Silent && snapshotTarget == SILENT_REBOOT_TARGET_JOIN_NETWORK) {
    // Rebooted on the way *into* File Transfer > Join Network for a fresh heap;
    // resume that flow directly instead of landing on home.
    activityManager.goToJoinNetwork();
  } else if (resume == BootResume::Silent && snapshotTarget == SILENT_REBOOT_TARGET_SETTINGS) {
    // Back out of the WiFi rows and the user is where they left off, not on Home.
    activityManager.goToSettings(SettingsSection::Network);
  } else if (resume == BootResume::Silent) {
    // target == home (or reader with no open book): land on home — don't fall
    // through to the sleep-wake "resume reader" logic, which fires on stale
    // openEpubPath + lastSleepFromReader from a prior session.
    activityManager.goHome();
  } else if (APP_STATE.openEpubPath.empty() || !APP_STATE.lastSleepFromReader ||
             mappedInputManager.isPressed(MappedInputManager::Button::Back) || APP_STATE.readerActivityLoadCount > 0) {
    // Boot to home screen if no book is open, last sleep was not from reader, back button is held, or reader activity
    // crashed (indicated by readerActivityLoadCount > 0)
    activityManager.goHome(HomeMenuItem::NONE, needsWakeRefresh);
  } else {
    // Clear app state to avoid getting into a boot loop if the epub doesn't load
    const auto path = APP_STATE.openEpubPath;
    APP_STATE.openEpubPath = "";
    APP_STATE.readerActivityLoadCount++;
    APP_STATE.saveToFile();
    activityManager.goToReader(path, allowFastInitialReaderRefresh);
  }

  if (resume == BootResume::Silent) {
    // Block until the first paint physically completes. refreshDisplay()
    // waits on the panel BUSY pin so when this returns the user can see the
    // new activity. Without the wait, an edge captured by gpio.update()
    // during boot dispatches against an invisible Home and the default
    // selectorIndex=0 opens the most-recent book.
    activityManager.requestUpdateAndWait();
    // Absorb any button held at this point into currentState as a non-edge:
    // two gpio.update() calls separated by > InputManager's 5ms debounce
    // transition the held bit through lastDebounceTime into currentState
    // without setting pressedEvents, so the first loop()'s own gpio.update()
    // sees state == currentState and emits nothing.
    gpio.update();
    delay(10);
    gpio.update();
  }

  allowSleepAt = millis() + 2000;
}

void loop() {
  static unsigned long maxLoopDuration = 0;
  const unsigned long loopStartTime = millis();
  static unsigned long lastMemPrint = 0;

  gpio.setSharedConfirmPowerShortPressEmitsPower(SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP);
  mappedInputManager.update();

  // Bluetooth page turner, before the USB drive's early return: a card handed to USB stops the radio
  // before anything else.
  bleturner::Scene scene{};
  // Home counts only for builds whose radio may start there (the S3 profile, RadioPolicy.h).
  const bool homeShown = activityManager.isForegroundHomeShown();
  const bool remoteContext = activityManager.isRemoteReaderContext();
  scene.where = remoteContext ? bleturner::Where::Reader
                : homeShown   ? bleturner::Where::Home
                              : bleturner::Where::Elsewhere;
  scene.visit = activityManager.visit();
  scene.pageShown = homeShown || remoteContext;
  scene.storageBusy = activityManager.requiresExclusiveStorageLoop();
  scene.wifiOn = WiFi.getMode() != WIFI_MODE_NULL;
  const bool remoteActed = bleturner::tick(scene);

  if (activityManager.requiresExclusiveStorageLoop()) {
    // USB Drive handed the raw SD card to the host. Do not run screenshots,
    // sleep, shortcuts, or normal navigation while its filesystem is detached.
    activityManager.loop();
    if (activityManager.preventAutoSleep()) {
      powerManager.setPowerSaving(false);
      delay(10);
    } else {
      // No host is active, so a slower loop is safe. The activity itself times
      // out the raw-storage handoff rather than entering deep sleep detached.
      powerManager.setPowerSaving(true);
      delay(50);
    }
    return;
  }

  // After the exclusive-storage return: it saves settings to the SD card.
  handleSlideSwitch();
  halTiltSensor.update(SETTINGS.tiltPageTurn, SETTINGS.orientation, activityManager.isReaderActivity());

  // Under the render lock, so a step never overlaps the themes' gauge reads.
  static bool gaugeCapacityPending = true;
  if (gaugeCapacityPending) {
    RenderLock lock(RenderLock::Mode::Try);
    if (lock.ownsLock()) gaugeCapacityPending = BatteryMonitor::loadDesignCapacity();
  }

  renderer.setFadingFix(SETTINGS.fadingFix);

  // The ROM console does not depend on Arduino USB CDC's connection state.
  if ((Serial || FREEINK_LOG_TRANSPORT == FREEINK_LOG_TRANSPORT_ROM_PRINTF) && millis() - lastMemPrint >= 10000) {
    const auto heap = HalMemory::getInternalHeap();
    LOG_INF("MEM", "Free: %zu bytes, Total: %zu bytes, Min Free: %zu bytes, MaxAlloc: %zu bytes", heap.freeBytes,
            heap.totalBytes, heap.minFreeBytes, heap.largestBlockBytes);
#ifdef BOARD_HAS_PSRAM
    const auto psram = HalMemory::getPsramHeap();
    LOG_INF("MEM", "PSRAM: Free: %zu bytes, Total: %zu bytes, Min Free: %zu bytes, MaxAlloc: %zu bytes",
            psram.freeBytes, psram.totalBytes, psram.minFreeBytes, psram.largestBlockBytes);
#endif
    lastMemPrint = millis();
  }

#ifdef CROSSPOINT_PM_LSTEST
  // Own 10 s tick, 5 s off the MEM tick (mechanism and history in
  // PmOnlyDiag.cpp): RX1's heartbeat died as the third line of the MEM
  // burst, the only one that had to wait for TX-ring drain. 0UL-5000UL
  // starts the phase ~5 s after boot; a >5 s loop stall coalesces the
  // ticks, which txShort makes visible.
  static unsigned long lastRxHbPrint = 0UL - 5000UL;
  if (millis() - lastRxHbPrint >= 10000) {
    lastRxHbPrint = millis();
    pmonlydiag::rxHeartbeat();
  }
#endif

  // Handle incoming serial commands,
  // nb: we use logSerial from logging to avoid deprecation warnings
#ifdef CROSSPOINT_RTC_CLK_DIAG
  // p0-rtc-clock: drain the RTC-retained diagnostic history once the USB
  // console is back after a wake.
  rtcclockdiag::flushBufferedLog();
#endif
#ifdef CROSSPOINT_COMPANION_GATE_STANDBY
  // P1-B9: retained standby evidence is announced once per boot and read
  // back only through the operator's SBDS serial commands (review C1): the
  // dump is repeatable and never destroys the RAM evidence. A diagnostic-cap
  // exit discarded the buffer, so nothing is announced after a cap.
  standbydiag::notifyRetained();
#endif
  if (logSerial.available() > 0) {
    String line = logSerial.readStringUntil('\n');
    if (line.startsWith("CMD:")) {
      String cmd = line.substring(4);
      cmd.trim();
#ifdef CROSSPOINT_WAIT_FOR_USB_SERIAL
      if (cmd.startsWith("GATEARM")) {
        // Dev-only diagnostic arm (p1-companion-gate §5): sleep with an
        // explicit test timer so the next timer wake dispatches the gate.
        // Usage: CMD:GATEARM <seconds in [5,3600]> [diag]. The diag byte
        // rides in the RTC arm record and disables the production USB bypass
        // for that single run; plain builds never read it.
        String args = cmd.length() > 7 ? cmd.substring(7) : String();
        args.trim();
        const int spaceIndex = args.indexOf(' ');
        const String secondsToken = spaceIndex < 0 ? args : args.substring(0, spaceIndex);
        const bool diagnostic = spaceIndex >= 0 && args.substring(spaceIndex + 1) == "diag";
        const long seconds = secondsToken.toInt();
        if (seconds < 5 || seconds > 3600) {
          logSerial.printf("GATEARM ERR: seconds must be in [5, 3600]\n");
        } else {
          companiongate::arm(diagnostic);
          logSerial.printf("GATEARM OK: %ld s%s\n", seconds, diagnostic ? " diag" : "");
          // Minimal diagnostic sleep: no sleep-screen render and no plugin
          // events — the point is a clean timer wake into the gate.
          bleturner::beforeSleep(2000);
          enterHardwareDeepSleep(static_cast<uint64_t>(seconds) * 1000000ULL);
        }
      }
#endif
#ifdef CROSSPOINT_RTC_CLK_DIAG
      if (cmd.startsWith("RTCCLK") || cmd.startsWith("RTCLOOP") || cmd.startsWith("RTCCAL")) {
        // p0-rtc-clock diagnostic.
        rtcclockdiag::handleCommand(powerManager, gpio, cmd.c_str());
      }
#endif
#ifdef CROSSPOINT_BLE_LIGHT_SLEEP_DIAG
      if (cmd == "PMSTAT") {
        // P1-B0a diagnostic: esp_pm lock dump.
        blelightsleepdiag::handleCommand(cmd.c_str());
      }
#endif
#ifdef CROSSPOINT_COMPANION_GATE_AUTOLS
      if (cmd == "GATESTAT") {
        // P1-B2a: gate-window sleep accounting and a PM-lock snapshot.
        companiongatepower::handleCommand(cmd.c_str());
      }
#endif
#ifdef CROSSPOINT_COMPANION_GATE_STANDBY
      // P1-B9 experimental readback: "SBDS" (repeatable dump), "SBDSACK
      // <crc32>" (clear on a CRC match), "SBDSCLR" (explicit discard). Only
      // reachable from an attached terminal; handled by the diag module.
      standbydiag::handleCommand(cmd.c_str());
#endif
#ifdef CROSSPOINT_PM_ONLY_DIAG
      if (cmd == "PMSTAT") {
        // PM diagnostic (p1-b1k/b1l/b1n): esp_pm lock and mode dump.
        pmonlydiag::handleCommand(cmd.c_str());
      }
#endif
#ifdef CROSSPOINT_PM_LSTEST
      if (cmd == "RXSTAT") {
        // p1-b1o: RX-path counters and USJ register snapshot.
        pmonlydiag::handleCommand(cmd.c_str());
      }
      if (cmd == "LSTEST") {
        // p1-b1o: the controlled automatic-light-sleep window. The USB-drive
        // exclusive-storage state decides the precondition inside the test.
        pmonlydiag::handleLsTest(activityManager.requiresExclusiveStorageLoop());
      }
      pmonlydiag::ackUnknownCommand(cmd.c_str());
#endif
      if (cmd == "SCREENSHOT") {
        const uint32_t bufferSize = display.getBufferSize();
        logSerial.printf("SCREENSHOT_START:%d\n", bufferSize);
        uint8_t* buf = display.getFrameBuffer();
        logSerial.write(buf, bufferSize);
        logSerial.printf("SCREENSHOT_END\n");
      }
    }
  }

  // Check for any user activity (button press or release) or active background work
  static unsigned long lastActivityTime = millis();
  if (gpio.wasAnyPressed() || gpio.wasAnyReleased() || gpio.wasTouchActivity() || halTiltSensor.hadActivity() ||
      remoteActed || activityManager.preventAutoSleep()) {
    lastActivityTime = millis();         // Reset inactivity timer
    powerManager.setPowerSaving(false);  // Restore normal CPU frequency on user activity
  }

  // Slideshow lifecycle: the single consumer of the RAM-only sleep request.
  // Start performs the one-time persistence cleanup (the APP_STATE a normal
  // sleep-from-viewer would produce); Continue performs none — no SD writes
  // between frames. Both sleep through the shared tail with the persisted
  // slideshow interval armed alongside the power button.
  const auto slideshowRequest = slideshow::takeSleepRequest();
  if (slideshowRequest != slideshow::SleepRequest::None) {
    if (slideshowRequest == slideshow::SleepRequest::Start) {
      // A later power-button exit must route Home (never a stale book) and
      // wake splashless with the last frame visible.
      APP_STATE.lastSleepFromReader = false;
      APP_STATE.showBootScreen = false;
      APP_STATE.saveToFile();
      if (Storage.exists(SLEEP_FRAME_FILE)) {
        // A stale Quick Resume frame must not restore over the slideshow
        // frame on a later power-button exit.
        Storage.remove(SLEEP_FRAME_FILE);
      }
    }
    HalPowerManager::Lock powerLock;
    // Timer policy of the handoff (slideshow::sleepRequestArmsTimer,
    // host-tested): Start/Continue RUN the slideshow and arm the persisted
    // interval; StaticSleep (battery cutoff) re-rendered the ordinary static
    // sleep screen — power-button only, no next timer wake.
    enterHardwareDeepSleep(
        slideshow::sleepRequestArmsTimer(slideshowRequest) ? slideshow::intervalMicros(SETTINGS.slideshowInterval) : 0);
    return;
  }

  // Let wake continue as soon as its hold has been verified. The release can
  // arrive after setup, so consume that one input frame rather than making it
  // a page turn, refresh, or other short power-button action.
  if (wakePowerReleasePending && !gpio.isPressed(HalGPIO::BTN_POWER)) {
    wakePowerReleasePending = false;
    return;
  }

  static bool screenshotButtonsReleased = true;
  static bool screenshotComboActive = false;
  if (gpio.isPressed(HalGPIO::BTN_POWER) && gpio.isPressed(HalGPIO::BTN_DOWN)) {
    screenshotComboActive = true;
    if (screenshotButtonsReleased) {
      screenshotButtonsReleased = false;
      {
        RenderLock lock;
        ScreenshotUtil::takeScreenshot(renderer);
      }
    }
    return;
  }
  if (screenshotComboActive) {
    if (gpio.isPressed(HalGPIO::BTN_POWER)) return;
    if (gpio.wasReleased(HalGPIO::BTN_POWER)) {
      screenshotButtonsReleased = true;
      screenshotComboActive = false;
      return;
    }
    screenshotButtonsReleased = true;
    screenshotComboActive = false;
  }

  // Consume the second X4 Pro power-button release so it does not also run a
  // configured short-power action after toggling the frontlight.
  if (handleX4ProFrontlightDoubleClick()) {
    return;
  }

  const bool x4ProDoubleClickPwrLight = BoardConfig::isX4Pro() && SETTINGS.doubleClickPwrLight;

#if FREEINK_CAP_TOUCH
  // A single X4 Pro power click becomes Confirm only after the frontlight
  // double-click window expires without a second click.
  mappedInputManager.setPowerConfirmClickFrame(false);
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::PWR_CONFIRM && x4ProDoubleClickPwrLight) {
    if (lastX4ProPowerClickAt != 0 && millis() - lastX4ProPowerClickAt > X4PRO_POWER_DOUBLE_CLICK_MS) {
      lastX4ProPowerClickAt = 0;
      mappedInputManager.setPowerConfirmClickFrame(true);
    }
    // A release held too long to be a double-click candidate (but still within
    // the normal Confirm press duration) never reaches handleX4ProFrontlightDoubleClick's
    // click tracking above, so it needs its own Confirm check here.
    if (mappedInputManager.wasReleased(MappedInputManager::Button::Power) &&
        gpio.getPowerButtonHeldTime() > X4PRO_POWER_CLICK_MAX_HOLD_MS &&
        gpio.getPowerButtonHeldTime() <= SETTINGS.getPowerButtonDuration()) {
      mappedInputManager.setPowerConfirmClickFrame(true);
    }
  }
#endif

  // Same deferral for SLEEP: getPowerButtonDuration() drops to 10ms so a quick
  // tap sleeps the device, which otherwise fires on button-down and never lets
  // a second click land. Sleep only once the double-click window has passed.
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP && x4ProDoubleClickPwrLight &&
      lastX4ProPowerClickAt != 0 && millis() - lastX4ProPowerClickAt > X4PRO_POWER_DOUBLE_CLICK_MS) {
    lastX4ProPowerClickAt = 0;
    enterDeepSleep();
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

  const unsigned long sleepTimeoutMs = SETTINGS.getSleepTimeoutMs();
  if (sleepTimeoutMs > 0 && millis() - lastActivityTime >= sleepTimeoutMs) {
    LOG_DBG("SLP", "Auto-sleep triggered after %lu ms of inactivity", sleepTimeoutMs);
    enterDeepSleep(true);
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

  // A hold that woke the device must be released before it can count as a new
  // in-app long press. Otherwise a user who keeps holding after wake would put
  // the device straight back to sleep once allowSleepAt expires.
  static bool powerReleasedSinceWake = false;
  if (!gpio.isPressed(HalGPIO::BTN_POWER)) powerReleasedSinceWake = true;

  // On X4 Pro with SLEEP, a press still within the click window is a
  // double-click candidate — let it be released and evaluated above instead
  // of sleeping on button-down.
  const bool x4ProAwaitingClickWindow = x4ProDoubleClickPwrLight &&
                                        SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP &&
                                        gpio.getPowerButtonHeldTime() <= X4PRO_POWER_CLICK_MAX_HOLD_MS;

  if (!x4ProAwaitingClickWindow && powerReleasedSinceWake && millis() >= allowSleepAt &&
      gpio.isPressed(HalGPIO::BTN_POWER) && gpio.getPowerButtonHeldTime() > SETTINGS.getPowerButtonDuration()) {
    // If the screenshot combination is potentially being pressed, don't sleep
    if (gpio.isPressed(HalGPIO::BTN_DOWN)) {
      return;
    }
    LOG_DBG("MAIN", "Power button held %lums, sleeping", gpio.getPowerButtonHeldTime());
    enterDeepSleep();
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

#if FREEINK_DEVICE_PAPERMONO
  // Paper Mono reports the PMIC power button as a one-tick click, so the held
  // path above cannot fire. With the default Ignore action, retain the normal
  // power-button meaning and shut down; explicit alternate bindings still win.
  if ((SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP ||
       SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::IGNORE) &&
      millis() >= allowSleepAt && mappedInputManager.wasReleased(MappedInputManager::Button::Power)) {
    enterDeepSleep();
    return;
  }
#endif

  // Refresh screen when power button is short-pressed with FORCE_REFRESH setting.
  if (mappedInputManager.homeButtonAction() == HomeButtonAction::ToggleFrontlight) {
    toggleFrontlight();
  }
  if (mappedInputManager.homeButtonAction() == HomeButtonAction::Refresh ||
      (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::FORCE_REFRESH &&
       mappedInputManager.wasReleased(MappedInputManager::Button::Power))) {
    LOG_DBG("MAIN", "Manual screen refresh triggered");
    if (!activityManager.handleForcedRefresh()) {
      RenderLock lock;
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }
  }

  // Refresh the battery icon when USB is plugged or unplugged.
  // Placed after sleep guards so we never queue a render that won't be processed.
  // Not while reading: there a repaint is a full page re-render (visible
  // flash, the AA pass re-running, and a frontlight dip under the refresh
  // load); the reader's status bar picks the charging state up on the next
  // page turn instead.
  if (gpio.wasUsbStateChanged() && !activityManager.isReaderActivity()) {
    activityManager.requestUpdate();
  }

  const unsigned long activityStartTime = millis();
  activityManager.loop();
  const unsigned long activityDuration = millis() - activityStartTime;

  const unsigned long loopDuration = millis() - loopStartTime;
  if (loopDuration > maxLoopDuration) {
    maxLoopDuration = loopDuration;
    if (maxLoopDuration > 50) {
      LOG_DBG("LOOP", "New max loop duration: %lu ms (activity: %lu ms)", maxLoopDuration, activityDuration);
    }
  }

  bool skipLoopDelay = false;
  {
    RenderLock lock(RenderLock::Mode::Try);
    if (!lock.ownsLock()) {
      // Let rendering advance without treating lock contention as idle.
      delay(10);
      return;
    }
    skipLoopDelay = activityManager.skipLoopDelay();
  }

  // Add delay at the end of the loop to prevent tight spinning
  // When an activity requests skip loop delay (e.g., webserver running), use yield() for faster response
  // Otherwise, use longer delay to save power
  if (skipLoopDelay) {
    powerManager.setPowerSaving(false);  // Make sure we're at full performance when skipLoopDelay is requested
    yield();                             // Give FreeRTOS a chance to run tasks, but return immediately
  } else {
    if (millis() - lastActivityTime >= HalPowerManager::IDLE_POWER_SAVING_MS && !pageturner::holdsCpu.load()) {
      // If we've been inactive for a while, increase the delay to save power
      powerManager.setPowerSaving(true);  // Lower CPU frequency after extended inactivity
      // Sleep in short slices and wake the poll as soon as a button contact closes.
      // InputManager commits a press only when two consecutive polls agree, so a
      // press shorter than one 50 ms sleep could land in a single sample and be lost.
      const unsigned long idleStart = millis();
      while (millis() - idleStart < 50) {
        delay(10);
        if (gpio.rawInputActive()) break;
      }
    } else {
      // Short delay to prevent tight loop while still being responsive
      delay(10);
    }
  }
}
