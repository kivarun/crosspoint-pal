#include "MscDiag.h"

#ifdef CROSSPOINT_USB_MSC_DIAG

// Link-time interposition hooks for the p1-b1b MSCDIAG. The production
// sources stay untouched: every observation point is a --wrap on the
// callee's (mangled) symbol, enabled only in the diagnostic envs. Each hook
// calls the real function exactly once and returns its result unmodified,
// so MSC and SD behavior cannot change — the two exceptions are the
// UsbMassStorage::begin hook, which substitutes the pass-through decorator
// for the raw block-device argument (the designed observation point), and
// the p1-b1g EPD byte-mover hooks, which skip the real call while a session
// is active (the designed suppression point).
//
// Register ABI note: the wrappers mirror the C++ member signatures on the
// Xtensa windowed ABI (this in a2, further args in a3+; the wrapper returns
// through its own a2, which the caller reads as its a10), which a plain
// extern "C" function with the same parameter/return types compiles to.

#include <Arduino.h>
#include <esp_err.h>
#include <sdmmc_cmd.h>

#include "MscDiag.h"

namespace {

// Set by the MSC-begin hook when it has already published init-ok/init-fail.
bool initNoted = false;
// Last observed values for the flip-detecting polls.
uint8_t lastState = 0xFF;  // 0xFF: nothing observed yet
bool lastSuspended = false;
// Raw TinyUSB flag baselines for the p1-b1d wraps; -1: nothing observed yet.
int8_t lastRawMounted = -1;
int8_t lastRawSuspended = -1;
// p1-b1g EPD byte-mover suppression: total suppressed SPI calls since the
// session started, and the debounce state of the epd-skip recording.
uint32_t epdSkipTotal = 0;
uint8_t epdSkipRecorded = 0;
bool epdSkipNoted = false;
uint32_t lastEpdSkipMs = 0;

constexpr uint8_t kEpdSkipRecordMax = 8;
constexpr unsigned long kEpdSkipDebounceMs = 200;
// EpdSkip payload a: which byte mover was suppressed.
constexpr uint8_t kEpdSkipOpByte = 1;
constexpr uint8_t kEpdSkipOpBulk = 2;

// One event per burst of suppressed calls (a redraw loop re-attempting every
// few ms would otherwise flood the 30-slot ring and evict the session's own
// state events). The first call of a session always records.
void epdSkipNote(uint8_t op) {
  epdSkipTotal += 1;
  if (epdSkipRecorded >= kEpdSkipRecordMax) {
    return;
  }
  const uint32_t now = millis();
  if (epdSkipNoted && now - lastEpdSkipMs < kEpdSkipDebounceMs) {
    return;
  }
  epdSkipNoted = true;
  epdSkipRecorded += 1;
  lastEpdSkipMs = now;
  mscdiag::note(mscdiag::EventKind::EpdSkip, 0, op, epdSkipTotal);
}

}  // namespace

extern "C" {

// --- HalStorage::begin() — one call from setup: stamp the retained ring. ---
bool __real__ZN10HalStorage5beginEv(void* self);
bool __wrap__ZN10HalStorage5beginEv(void* self) {
  const bool ok = __real__ZN10HalStorage5beginEv(self);
  mscdiag::bootInit();
  return ok;
}

// --- HalStorage::beginUsbDrive() — the USB Drive session entry. ---
bool __real__ZN10HalStorage13beginUsbDriveEv(void* self);
bool __wrap__ZN10HalStorage13beginUsbDriveEv(void* self) {
  mscdiag::note(mscdiag::EventKind::MscStart, 0, 0, 0);
  initNoted = false;
  lastState = 0xFF;
  lastSuspended = false;
  lastRawMounted = -1;
  lastRawSuspended = -1;
  epdSkipTotal = 0;
  epdSkipRecorded = 0;
  epdSkipNoted = false;
  lastEpdSkipMs = 0;
  const bool ok = __real__ZN10HalStorage13beginUsbDriveEv(self);
  if (!ok && !initNoted) {
    // No raw block device: MSC begin was never reached (stage hint 1).
    mscdiag::note(mscdiag::EventKind::MscInitFail, 0, 0, 1);
  }
  return ok;
}

// --- freeink::UsbMassStorage::begin(FsBlockDeviceInterface*) — installs the
// pass-through decorator so the session's block traffic is observable. ---
bool __real__ZN7freeink14UsbMassStorage5beginEP22FsBlockDeviceInterface(void* self, void* dev);
bool __wrap__ZN7freeink14UsbMassStorage5beginEP22FsBlockDeviceInterface(void* self, void* dev) {
  static mscdiag::MscDiagBlockDevice diagDevice(nullptr);
  diagDevice.setInner(static_cast<FsBlockDeviceInterface*>(dev));
  mscdiag::setSessionActive(true);
  const bool ok = __real__ZN7freeink14UsbMassStorage5beginEP22FsBlockDeviceInterface(self, &diagDevice);
  initNoted = true;
  if (ok) {
    mscdiag::note(mscdiag::EventKind::MscInitOk, 0, 0, 0);
  } else {
    mscdiag::note(mscdiag::EventKind::MscInitFail, 0, 0, 2);
    mscdiag::setSessionActive(false);
  }
  return ok;
}

// --- HalStorage::disconnectUsbDriveHost() ---
bool __real__ZN10HalStorage22disconnectUsbDriveHostEv(void* self);
bool __wrap__ZN10HalStorage22disconnectUsbDriveHostEv(void* self) {
  mscdiag::note(mscdiag::EventKind::DisconnectReq, 0, 0, 0);
  return __real__ZN10HalStorage22disconnectUsbDriveHostEv(self);
}

// --- HalStorage::endUsbDrive() — tud_disconnect + drain. The session stays
// marked active until the real teardown returns, so SDMMC failures inside
// the drain window are still captured. ---
void __real__ZN10HalStorage11endUsbDriveEv(void* self);
void __wrap__ZN10HalStorage11endUsbDriveEv(void* self) {
  mscdiag::note(mscdiag::EventKind::DisconnectExec, 0, 0, 0);
  __real__ZN10HalStorage11endUsbDriveEv(self);
  mscdiag::setSessionActive(false);
}

// --- HalStorage::usbDriveState() — polled once per activity loop pass. ---
uint8_t __real__ZNK10HalStorage13usbDriveStateEv(void* self);
uint8_t __wrap__ZNK10HalStorage13usbDriveStateEv(void* self) {
  const uint8_t state = __real__ZNK10HalStorage13usbDriveStateEv(self);
  if (mscdiag::sessionActive() && state != lastState) {
    if (lastState != 0xFF) {
      mscdiag::note(mscdiag::EventKind::StateChange, 0,
                    (static_cast<uint32_t>(lastState) << 8) | static_cast<uint32_t>(state), 0);
      if (lastState == 1 && state == 2) {  // WaitingForHost -> Connected: host mounted
        mscdiag::note(mscdiag::EventKind::Mounted, 1, 0, 0);
      }
    }
    lastState = state;
  }
  return state;
}

// --- HalStorage::usbDriveHostSuspended() — polled while Connected. ---
bool __real__ZNK10HalStorage21usbDriveHostSuspendedEv(void* self);
bool __wrap__ZNK10HalStorage21usbDriveHostSuspendedEv(void* self) {
  const bool suspended = __real__ZNK10HalStorage21usbDriveHostSuspendedEv(self);
  if (mscdiag::sessionActive() && suspended != lastSuspended) {
    lastSuspended = suspended;
    mscdiag::note(mscdiag::EventKind::Suspended, suspended ? 1 : 0, 0, 0);
  }
  return suspended;
}

// --- tud_mounted()/tud_suspended() — the raw TinyUSB flags (p1-b1d). Every
// call site is outside usbd.c (the SDK's state()/end()/hostSuspended() polls
// and any core-internal callers), so the wraps see them all. Flip-only
// recording; the real function runs exactly once before any observation and
// its result is returned unmodified. The baselines are plain bytes shared by
// whichever tasks call these functions: concurrent callers can duplicate or,
// in a flag-bounce interleaving, miss a flip. In this binary every call site
// runs on the app task (verified in the linked ELF), so neither can occur;
// re-check if a future cross-task caller appears. ---
bool __real_tud_mounted(void);
bool __wrap_tud_mounted(void) {
  const bool mounted = __real_tud_mounted();
  if (mscdiag::sessionActive()) {
    const int8_t value = mounted ? 1 : 0;
    if (lastRawMounted == 0 && value == 1) {
      mscdiag::note(mscdiag::EventKind::Remount, 0, lastState, 0);
    } else if (lastRawMounted == 1 && value == 0) {
      mscdiag::note(mscdiag::EventKind::Umount, 0, lastState, 0);
    }
    lastRawMounted = value;
  }
  return mounted;
}

bool __real_tud_suspended(void);
bool __wrap_tud_suspended(void) {
  const bool suspended = __real_tud_suspended();
  if (mscdiag::sessionActive()) {
    const int8_t value = suspended ? 1 : 0;
    if (lastRawSuspended == 0 && value == 1) {
      mscdiag::note(mscdiag::EventKind::BusSuspend, 0, lastState, 0);
    } else if (lastRawSuspended == 1 && value == 0) {
      mscdiag::note(mscdiag::EventKind::BusResume, 0, lastState, 0);
    }
    lastRawSuspended = value;
  }
  return suspended;
}

// --- SPIClass::transfer(uint8_t) / writeBytes(const uint8_t*, uint32_t) —
// the p1-b1g EPD refresh isolation. The ELF call-site census (p1-b1g report)
// shows these two symbols carry every SPI byte in the firmware: their only
// callers are EpdBus's transaction internals (Transaction::transfer /
// writeBytes, EpdBus::data / cmdData), and every other byte mover is
// garbage-collected. While a session is active the real call is skipped, so
// no byte reaches the panel: no RAM write, no LUT, no waveform activation,
// so BUSY stays at its idle level and every wait returns immediately.
// Everything above the bus (facade pending state, driver bookkeeping, the
// bus's PM locks and CS/DC framing) runs unchanged. The gate is a plain bool
// written once per session by the app task and read here from the render
// task: a flip can be observed a tick late, which only shifts suppression
// start/end by that tick. The suppressed transfer returns 0; every caller
// today discards the return (command writes only) — re-check if a driver
// ever reads back over SPI. ---
uint8_t __real__ZN8SPIClass8transferEh(void* self, uint8_t data);
uint8_t __wrap__ZN8SPIClass8transferEh(void* self, uint8_t data) {
  if (mscdiag::sessionActive()) {
    epdSkipNote(kEpdSkipOpByte);
    return 0;
  }
  return __real__ZN8SPIClass8transferEh(self, data);
}

void __real__ZN8SPIClass10writeBytesEPKhm(void* self, const uint8_t* data, uint32_t size);
void __wrap__ZN8SPIClass10writeBytesEPKhm(void* self, const uint8_t* data, uint32_t size) {
  if (mscdiag::sessionActive()) {
    epdSkipNote(kEpdSkipOpBulk);
    return;
  }
  __real__ZN8SPIClass10writeBytesEPKhm(self, data, size);
}

// --- ActivityManager::loop() — the main loop heartbeat: flush the ring once
// the serial console is back after the teardown restart. ---
void __real__ZN15ActivityManager4loopEv(void* self);
void __wrap__ZN15ActivityManager4loopEv(void* self) {
  __real__ZN15ActivityManager4loopEv(self);
  mscdiag::flushBufferedLog();
}

// --- sdmmc block layer — captures the esp_err_t the FsBlockDeviceInterface
// (bool only) hides. Gated on an active MSC session so unrelated SD traffic
// never fills the ring; the real driver is still called exactly once. ---
esp_err_t __real_sdmmc_read_sectors(sdmmc_card_t* card, void* dst, size_t start_sector, size_t sector_count);
esp_err_t __wrap_sdmmc_read_sectors(sdmmc_card_t* card, void* dst, size_t start_sector, size_t sector_count) {
  const esp_err_t err = __real_sdmmc_read_sectors(card, dst, start_sector, sector_count);
  if (err != ESP_OK && mscdiag::sessionActive()) {
    mscdiag::note(mscdiag::EventKind::SdErr, static_cast<uint8_t>(sector_count), start_sector, err);
  }
  return err;
}

esp_err_t __real_sdmmc_write_sectors(sdmmc_card_t* card, const void* src, size_t start_sector, size_t sector_count);
esp_err_t __wrap_sdmmc_write_sectors(sdmmc_card_t* card, const void* src, size_t start_sector, size_t sector_count) {
  const esp_err_t err = __real_sdmmc_write_sectors(card, src, start_sector, sector_count);
  if (err != ESP_OK && mscdiag::sessionActive()) {
    mscdiag::note(mscdiag::EventKind::SdErr, static_cast<uint8_t>(sector_count), start_sector, err);
  }
  return err;
}

}  // extern "C"

#endif