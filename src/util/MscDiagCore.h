#pragma once

#include <stddef.h>
#include <stdint.h>

#include <atomic>

// Pure support core for the dev-only MSCDIAG (p1-b1b): the RTC-retained event
// ring for the USB-MSC failure investigation. No Arduino or ESP headers, so
// the host tests compile this directly.
//
// The whole ring must fit the RTC_SLOW headroom left over from the P0
// diagnostic (~1 KB at link time), hence the packed 16-byte entries.
//
// Concurrency model: writers claim a slot with an atomic fetch-add on the
// shared event counter, so every writer owns its slot exclusively; the entry
// is marked unpublished before being filled and published by storing its
// sequence byte last. A torn or half-filled entry is never handed out. Slot
// allocation order is chronological; the millis field is the display order.
namespace mscdiag {

enum class EventKind : uint8_t {
  Boot = 0,        // a = ring generation, b = events ever recorded (may exceed the ring)
  MscStart,        // beginUsbDrive entered
  MscInitOk,       // usbMassStorage.begin() succeeded
  MscInitFail,     // begin() failed; b = stage hint (1 = no raw device, 2 = begin false)
  SdRead,          // interface read failed; a = lba, ns = sectors, b = 0 (err unknown here)
  SdWrite,         // interface write failed; same fields
  SdErr,           // sdmmc esp_err_t captured by the wrapper; a = lba, ns = sectors, b = err
  StateChange,     // a = (from << 8) | to
  Mounted,         // WaitingForHost -> Connected observed; ns = 1
  Suspended,       // host suspend hint flip; ns = new value
  DisconnectReq,   // disconnectUsbDriveHost() requested
  DisconnectExec,  // endUsbDrive() reached (tud_disconnect + drain)
  // p1-b1d: raw TinyUSB flag flips (linker-wrapped tud_mounted/tud_suspended).
  // Flip-only recording; a = last HAL-polled UsbDriveState (0xFF = no poll
  // yet) to correlate the raw drop with the app view. These timestamps are
  // call-time observations, not the exact hardware event instants.
  Umount,      // raw tud_mounted() flipped true -> false
  Remount,     // raw tud_mounted() flipped false -> true
  BusSuspend,  // raw tud_suspended() flipped false -> true
  BusResume,   // raw tud_suspended() flipped true -> false
  // p1-b1g: EPD refresh isolation. The SPI byte movers (SPIClass::transfer /
  // SPIClass::writeBytes, the only byte paths to the panel) return without
  // touching the bus while a session is active. Recording is debounced, so b
  // is the meaningful total, not the event count.
  EpdSkip,  // a = 1 (byte mover) / 2 (bulk mover); b = suppressed calls so far
};

struct Entry {
  uint32_t millis;
  uint32_t a;
  uint32_t b;
  uint8_t kind;  // EventKind
  uint8_t ns;
  uint8_t freq;  // CPU MHz at the event
  uint8_t seq;   // publication stamp, stored last
};

static_assert(sizeof(Entry) == 16, "the RTC_SLOW headroom depends on the 16-byte entry");

inline constexpr uint8_t kSeqInvalid = 0xFF;

// Fixed-capacity lossy ring over caller-provided storage. kEntries entries of
// 16 B each; oldest overwritten first; the monotonically increasing event
// counter doubles as the total.
template <size_t kEntries>
class MscDiagRingCore {
 public:
  static constexpr uint32_t kMagic = 0x4D534331;  // "MSC1"

  // Reset is invoked from a single task in practice (boot) or lazily from the
  // first note() after a power cycle; two racing resets would only waste one
  // generation bump, which is acceptable for a diagnostic label. Magic is
  // written LAST so a power cut mid-reset cannot leave a valid magic over
  // garbage counters.
  void reset() {
    const uint16_t gen = generation_.load(std::memory_order_relaxed) + 1;
    generation_.store(gen == 0 ? 1 : gen, std::memory_order_relaxed);
    absolute_.store(0, std::memory_order_relaxed);
    for (size_t i = 0; i < kEntries; ++i) {
      entries_[i] = Entry{};
      entries_[i].seq = kSeqInvalid;
    }
    magic_ = kMagic;
  }

  // Content is valid only if the magic survived; RTC_NOINIT is garbage after
  // a power cycle.
  bool contentValid() const { return magic_ == kMagic && generation_.load(std::memory_order_relaxed) != 0; }

  uint16_t generation() const { return generation_.load(std::memory_order_relaxed); }
  uint32_t total() const { return absolute_.load(std::memory_order_relaxed); }
  // Events evicted from the ring (no longer in the dump).
  uint32_t dropped() const {
    const uint32_t n = total();
    return n > kEntries ? n - kEntries : 0;
  }

  // Claim exclusive ownership of the next slot. Returns the slot index; the
  // caller must mark the slot unpublished (seq = kSeqInvalid), fill it, then
  // publish it by storing the real sequence last.
  size_t claim() { return absolute_.fetch_add(1, std::memory_order_relaxed) % kEntries; }

  // Publish a filled slot. seq is stored last with a plain store: safe while
  // the boot-time flush (before any session can start) is the only reader.
  // A reader added during a live session would need release/acquire pairs.
  void publish(size_t slot, uint8_t seq) { entries_[slot].seq = seq; }

  Entry& slot(size_t slot) { return entries_[slot]; }
  const Entry& slot(size_t slot) const { return entries_[slot]; }
  static constexpr size_t entryCount() { return kEntries; }

  // Oldest-first walk over published entries; fn returns false to stop.
  // Slots are walked from the oldest absolute index; entries whose seq marks
  // them unpublished (a writer died mid-fill, or storage garbage) are skipped.
  template <typename Fn>
  void forEach(Fn fn) const {
    if (!contentValid()) return;
    const uint32_t n = total();
    const uint32_t oldest = n > kEntries ? n - kEntries : 0;
    for (uint32_t i = oldest; i < n; ++i) {
      const Entry& e = entries_[i % kEntries];
      if (e.seq == kSeqInvalid) continue;
      if (!fn(e)) return;
    }
  }

 private:
  uint32_t magic_ = 0;
  std::atomic<uint16_t> generation_{0};
  std::atomic<uint32_t> absolute_{0};
  Entry entries_[kEntries] = {};
};

}  // namespace mscdiag
