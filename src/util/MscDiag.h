#pragma once
#ifdef CROSSPOINT_USB_MSC_DIAG

// Dev-only USB-MSC diagnostic (p1-b1b). Compiled only in the diagnostic envs
// that define CROSSPOINT_USB_MSC_DIAG (see platformio.ini); production builds
// see none of it. Records the MSC event tail into an RTC_NOINIT ring that
// survives the teardown restart, flushed over the serial log once the USJ
// console is back.

#include <SdFat.h>  // FsBlockDeviceInterface, Sector_t
#include <stdint.h>

#include "MscDiagCore.h"

namespace mscdiag {

// Fixed capacity: 30 events x 16 B plus a 12 B header (492 B of RTC_NOINIT;
// the rtc_slow_seg headroom shrinks with every RTC diagnostic added to the
// diagnostic envs — check the .map before growing any of them).
inline constexpr size_t kRingEntries = 30;

// The RTC_SLOW headroom is a hard linker budget; keep the whole storage
// within it (the link fails otherwise).
static_assert(sizeof(MscDiagRingCore<kRingEntries>) <= 512, "MSCDIAG ring exceeds the rtc_slow_seg headroom");

void bootInit();

// True while an MSC session is running. Scopes the linker-wrap SDMMC error
// hooks to the session so unrelated SD traffic never fills the ring. .bss:
// false after any reset.
void setSessionActive(bool active);
bool sessionActive();

// Publishes one event. Safe from any task; lock-free; never blocks or delays.
void note(EventKind kind, uint8_t ns, uint32_t a, uint32_t b);

// Flushes the retained ring over the serial console once per boot. Line
// grammar for the parser (two shapes only):
//   MSCDIAG|<label> gen=<ring generation> <millis>ms f=<cpu MHz> ns=<payload> a=<payload> b=<payload>
//   MSCDIAG|dropped=<evicted count> gen=<ring generation> total=<events ever>
// Labels are fixed tokens from EventKind; per-kind payload meaning is
// documented on each EventKind in MscDiagCore.h. The flush retries every
// main-loop pass and latches only once the serial console is connected
// (P0 rtcclockdiag pattern). Returns the number of lines printed.
uint32_t flushBufferedLog();

// Pass-through decorator over the raw SD block device: forwards every call
// unchanged (no delays, no state mutation) and records interface-level
// read/write failures, the one thing the esp_err_t-capturing wrapper cannot
// see (a bool failure with esp_err_t == ESP_OK). The esp_err_t of a failed
// SDMMC transfer is captured by the linker-wrap hooks (MscDiagHooks.cpp),
// because the FsBlockDeviceInterface only exposes bool.
class MscDiagBlockDevice final : public FsBlockDeviceInterface {
 public:
  explicit MscDiagBlockDevice(FsBlockDeviceInterface* inner) : inner_(inner) {}

  // Rebind the wrapped device; the decorator is a long-lived static and the
  // raw block device is re-detached on every USB Drive session.
  void setInner(FsBlockDeviceInterface* inner) { inner_ = inner; }

  void end() override { inner_->end(); }
  bool isBusy() override { return inner_->isBusy(); }
  bool readSector(Sector_t sector, uint8_t* dst) override { return readSectors(sector, dst, 1); }

  bool readSectors(Sector_t sector, uint8_t* dst, size_t ns) override {
    const bool ok = inner_->readSectors(sector, dst, ns);
    if (!ok) {
      note(EventKind::SdRead, static_cast<uint8_t>(ns), sector, 0);
    }
    return ok;
  }

  bool writeSector(Sector_t sector, const uint8_t* src) override { return writeSectors(sector, src, 1); }

  bool writeSectors(Sector_t sector, const uint8_t* src, size_t ns) override {
    const bool ok = inner_->writeSectors(sector, src, ns);
    if (!ok) {
      note(EventKind::SdWrite, static_cast<uint8_t>(ns), sector, 0);
    }
    return ok;
  }

  Sector_t sectorCount() override { return inner_->sectorCount(); }
  bool syncDevice() override { return inner_->syncDevice(); }

 private:
  FsBlockDeviceInterface* inner_;
};

}  // namespace mscdiag
#endif
