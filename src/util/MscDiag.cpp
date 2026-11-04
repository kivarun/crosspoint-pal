#include "MscDiag.h"

#ifdef CROSSPOINT_USB_MSC_DIAG

#include <Arduino.h>  // millis, getCpuFrequencyMhz
#include <Logging.h>

namespace mscdiag {
namespace {

struct Storage {
  MscDiagRingCore<kRingEntries> ring;
};

RTC_NOINIT_ATTR Storage storage;
uint32_t flushStamp = 0;  // .bss: zeroed by any reset, so the ring flushes once per boot
bool sessionState = false;

uint8_t nextSeq() {
  static uint8_t seq = 0;
  do {
    seq += 1;
  } while (seq == kSeqInvalid);  // 0xFF is the "unpublished" marker
  return seq;
}

const char* kindName(uint8_t kind) {
  switch (static_cast<EventKind>(kind)) {
    case EventKind::Boot:
      return "boot";
    case EventKind::MscStart:
      return "start";
    case EventKind::MscInitOk:
      return "init-ok";
    case EventKind::MscInitFail:
      return "init-fail";
    case EventKind::SdRead:
      return "rd-fail";
    case EventKind::SdWrite:
      return "wr-fail";
    case EventKind::SdErr:
      return "sd-err";
    case EventKind::StateChange:
      return "state";
    case EventKind::Mounted:
      return "mounted";
    case EventKind::Suspended:
      return "suspended";
    case EventKind::DisconnectReq:
      return "disc-req";
    case EventKind::DisconnectExec:
      return "disc-exec";
    case EventKind::Umount:
      return "umount";
    case EventKind::Remount:
      return "remount";
    case EventKind::BusSuspend:
      return "bus-susp";
    case EventKind::BusResume:
      return "bus-res";
    case EventKind::EpdSkip:
      return "epd-skip";
    default:
      return "unknown";
  }
}

}  // namespace

void bootInit() {
  if (!storage.ring.contentValid()) {
    storage.ring.reset();
  }
  // A restart keeps the ring and the generation; only a power cycle resets.
  note(EventKind::Boot, 0, storage.ring.generation(), storage.ring.total());
  flushStamp = 0;
}

void note(EventKind kind, uint8_t ns, uint32_t a, uint32_t b) {
  if (!storage.ring.contentValid()) {
    storage.ring.reset();
  }
  const size_t slot = storage.ring.claim();
  Entry& e = storage.ring.slot(slot);
  e.seq = kSeqInvalid;  // unpublished while being filled; stored last below
  e.millis = millis();
  e.kind = static_cast<uint8_t>(kind);
  e.ns = ns;
  e.a = a;
  e.b = b;
  e.freq = static_cast<uint8_t>(getCpuFrequencyMhz());
  const uint8_t seq = nextSeq();
  e.seq = seq;
  storage.ring.publish(slot, seq);
}

void setSessionActive(const bool active) { sessionState = active; }

bool sessionActive() { return sessionState; }

uint32_t flushBufferedLog() {
  // P0 rtcclockdiag pattern: retry every pass and latch only once the serial
  // console is actually connected, so a dead console never consumes the
  // single per-boot dump.
  if (flushStamp != 0 || !logSerial) return 0;
  if (!storage.ring.contentValid()) return 0;
  flushStamp = 1;
  uint32_t lines = 0;
  storage.ring.forEach([&lines](const Entry& e) {
    logSerial.printf("MSCDIAG|%s gen=%u %lums f=%u ns=%u a=%lu b=%lu\n", kindName(e.kind),
                     static_cast<unsigned>(storage.ring.generation()), static_cast<unsigned long>(e.millis), e.freq,
                     e.ns, static_cast<unsigned long>(e.a), static_cast<unsigned long>(e.b));
    lines += 1;
    return true;
  });
  if (storage.ring.dropped() != 0) {
    logSerial.printf("MSCDIAG|dropped=%lu gen=%u total=%lu\n", static_cast<unsigned long>(storage.ring.dropped()),
                     static_cast<unsigned>(storage.ring.generation()),
                     static_cast<unsigned long>(storage.ring.total()));
    lines += 1;
  }
  return lines;
}

}  // namespace mscdiag
#endif
