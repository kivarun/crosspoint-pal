#pragma once

// P1-B9 continuous standby diagnostics (experimental): the pure record format,
// interval attribution, accounting and acceptance logic for the standby soak
// (analysis revision 3, corrected by review C1). No device headers: host tests
// include this file directly. The device side — the static buffer instance, PM
// sampling, the radio-state feed and the operator-controlled readback — lives
// in the .cpp behind CROSSPOINT_COMPANION_GATE_STANDBY.
//
// Measurement contract (revision 3, as corrected):
// - authoritative duration is the 64-bit running total in the header, never
//   the wrapped 32-bit GatePowerRecord.sleepUs;
// - a span is attributed to the radio state that held during it, whether or
//   not its PM capture succeeded: an invalid capture keeps its state class
//   (samplesValid=0), so its wall time stays in that class's measurement
//   population — it lowers both coverage and measured residency instead of
//   disappearing from the denominator (C1 blocker 1);
// - residency is judged over the advertising class wall time, and the >=90%
//   criterion requires >=98% valid-sample coverage of that wall time;
// - the residency verdict and the BLE clock-source verdict are separate
//   results: only EXT_32K_XTAL validates the electrical claim, and a
//   MAIN_XTAL fallback can never read as overall acceptance (C1 correction 1);
// - the buffer is a strict 40 KiB static budget with per-class drop counters;
//   incomplete evidence yields INCONCLUSIVE, never a false PASS.

#include <cstddef>
#include <cstdint>

namespace companionstandbydiag {

// --- Record geometry ---------------------------------------------------------
// PM sample packed fields: 4+4+4+4+8+1+1 = 26 bytes.
inline constexpr size_t kPmSampleSize = 26;
inline constexpr size_t kHeartbeatSize = 16;
inline constexpr size_t kTransitionSize = 8;
inline constexpr size_t kErrorSize = 12;
inline constexpr size_t kStartupSize = 8;
// Heap snapshot (session start/exit, C1 correction 2): 4+4+4+1+3 pad = 16.
inline constexpr size_t kSnapshotSize = 16;
inline constexpr size_t kHeaderSize = 84;

// 24 h capacities: one 5-min PM sample, one 1-min heartbeat, a bounded
// transition-driven share (one PM sample per recorded transition), and room
// for the start/exit heap snapshots (two used, headroom for retry paths).
inline constexpr uint32_t kPeriodicPmCapacity = 288;
inline constexpr uint32_t kTransitionPmCapacity = 128;
inline constexpr uint32_t kHeartbeatCapacity = 1440;
inline constexpr uint32_t kTransitionCapacity = 128;
inline constexpr uint32_t kErrorCapacity = 32;
inline constexpr uint32_t kStartupCapacity = 1;
inline constexpr uint32_t kSnapshotCapacity = 4;

// The strict static budget (analysis revision 3): 40 KiB.
inline constexpr size_t kBufferBudget = 40960;

// Region offsets (header first, then the record classes).
inline constexpr size_t kPeriodicPmOffset = kHeaderSize;
inline constexpr size_t kTransitionPmOffset = kPeriodicPmOffset + kPeriodicPmCapacity * kPmSampleSize;
inline constexpr size_t kHeartbeatOffset = kTransitionPmOffset + kTransitionPmCapacity * kPmSampleSize;
inline constexpr size_t kTransitionOffset = kHeartbeatOffset + kHeartbeatCapacity * kHeartbeatSize;
inline constexpr size_t kErrorOffset = kTransitionOffset + kTransitionCapacity * kTransitionSize;
inline constexpr size_t kStartupOffset = kErrorOffset + kErrorCapacity * kErrorSize;
inline constexpr size_t kSnapshotOffset = kStartupOffset + kStartupCapacity * kStartupSize;
inline constexpr size_t kBufferNeeded = kSnapshotOffset + kSnapshotCapacity * kSnapshotSize;

static_assert(kPmSampleSize == 26, "PM sample layout contract: 26 packed bytes");
static_assert(kHeaderSize == 84, "header layout contract: 84 bytes");
static_assert(kBufferNeeded <= kBufferBudget, "standby diagnostic buffer exceeds the 40 KiB budget");
static_assert(kPeriodicPmCapacity == 288 && kHeartbeatCapacity == 1440, "24 h cadence contract");

inline constexpr uint32_t kBufferMagic = 0x53424453u;  // 'SBDS'
inline constexpr uint16_t kBufferVersion = 2;

// Header field offsets inside the 84-byte header.
inline constexpr size_t kHdrMagic = 0;
inline constexpr size_t kHdrVersion = 4;
inline constexpr size_t kHdrFlags = 6;
inline constexpr size_t kHdrPeriodicPmCount = 8;
inline constexpr size_t kHdrTransitionPmCount = 12;
inline constexpr size_t kHdrHeartbeatCount = 16;
inline constexpr size_t kHdrTransitionCount = 20;
inline constexpr size_t kHdrErrorCount = 24;
inline constexpr size_t kHdrDroppedPeriodicPm = 28;
inline constexpr size_t kHdrDroppedTransitionPm = 32;
inline constexpr size_t kHdrDroppedHeartbeat = 36;
inline constexpr size_t kHdrDroppedTransition = 40;
inline constexpr size_t kHdrDroppedError = 44;
inline constexpr size_t kHdrEntriesTotal = 48;  // u64
inline constexpr size_t kHdrSleepUsTotal = 56;  // i64
inline constexpr size_t kHdrAdvertisingSec = 64;
inline constexpr size_t kHdrConnectedSec = 68;
inline constexpr size_t kHdrOtherSec = 72;
inline constexpr size_t kHdrSnapshotCount = 76;
inline constexpr size_t kHdrDroppedSnapshot = 80;

// Header flag bits.
inline constexpr uint16_t kFlagTransitionOverflow = 1u << 0;  // the transition log ran out of slots
inline constexpr uint16_t kFlagClockRecorded = 1u << 1;       // the LPCLK source was read and stored
inline constexpr uint16_t kFlagExitPower = 1u << 2;
inline constexpr uint16_t kFlagExitUsb = 1u << 3;

// Interval classes (record tag values are stable across builds). The retired
// "Mixed" class is gone: an invalid capture keeps its state class and is
// marked through samplesValid instead, so unmeasurable time never leaves the
// measurement population of the state that held (C1 blocker 1).
enum class IntervalClass : uint8_t { Advertising = 1, Connected = 2, Other = 3 };

// Radio states as fed by the caller (blecompanion::RadioState values).
inline constexpr uint8_t kRadioStopped = 0;
inline constexpr uint8_t kRadioAdvertising = 1;
inline constexpr uint8_t kRadioAwaitingAuth = 2;
inline constexpr uint8_t kRadioConnected = 3;

// esp_bt_sleep_clock_t values of interest (esp_bt.h); the raw value is stored.
inline constexpr uint8_t kLpclkMainXtal = 1;
inline constexpr uint8_t kLpclkExt32kXtal = 2;
// The firmware cannot verify the separate RTCCLK crystal preflight; the field
// is reserved and the operator's preflight record is the hardware evidence.
inline constexpr uint8_t kCrystalNotRecorded = 0xFF;

// Exit kinds. Power and USB exits keep the RAM evidence intact into the
// ordinary boot that follows, where the operator reads or clears it. The
// diagnostic cap ends in deep sleep, so its implementation discards the
// buffer at the exit: no stale evidence can ever be presented, and the
// session reads as INCONCLUSIVE by absence of evidence.
enum class ExitKind : uint8_t { Power = 1, Usb = 2, Cap = 3 };

// --- Little-endian field helpers (memcpy-based: no unaligned access) ---------
inline void storeLE16(uint8_t* p, const uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}
inline void storeLE32(uint8_t* p, const uint32_t v) {
  storeLE16(p, static_cast<uint16_t>(v));
  storeLE16(p + 2, static_cast<uint16_t>(v >> 16));
}
inline void storeLE64(uint8_t* p, const uint64_t v) {
  storeLE32(p, static_cast<uint32_t>(v));
  storeLE32(p + 4, static_cast<uint32_t>(v >> 32));
}
inline uint16_t loadLE16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline uint32_t loadLE32(const uint8_t* p) {
  return static_cast<uint32_t>(loadLE16(p)) | (static_cast<uint32_t>(loadLE16(p + 2)) << 16);
}
inline uint64_t loadLE64(const uint8_t* p) {
  return static_cast<uint64_t>(loadLE32(p)) | (static_cast<uint64_t>(loadLE32(p + 4)) << 32);
}

// Standard CRC-32 (reflected, poly 0xEDB88320, init/final 0xFFFFFFFF). Used
// for the readback integrity check: the dump repeats identically, and the
// clear-on-ack command only accepts the CRC of the evidence it acknowledges.
inline uint32_t crc32(const uint8_t* data, const size_t len) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      const uint32_t mask = 0u - (crc & 1u);
      crc = (crc >> 1) ^ (0xEDB88320u & mask);
    }
  }
  return crc ^ 0xFFFFFFFFu;
}

// The buffer is a plain byte array: the layout lives in the offsets above and
// the accessors below, never in packed structs.
struct Buffer {
  uint8_t bytes[kBufferBudget];
};

// --- Header accessors --------------------------------------------------------
inline void reset(Buffer& b) {
  for (size_t i = 0; i < kBufferNeeded; ++i) b.bytes[i] = 0;
  storeLE32(b.bytes + kHdrMagic, kBufferMagic);
  storeLE16(b.bytes + kHdrVersion, kBufferVersion);
}

inline bool headerValid(const Buffer& b) {
  return loadLE32(b.bytes + kHdrMagic) == kBufferMagic && loadLE16(b.bytes + kHdrVersion) == kBufferVersion;
}

inline uint32_t count(const Buffer& b, const size_t offset) { return loadLE32(b.bytes + offset); }
inline uint32_t dropped(const Buffer& b, const size_t offset) { return loadLE32(b.bytes + offset); }
inline uint16_t flags(const Buffer& b) { return loadLE16(b.bytes + kHdrFlags); }
// cppcheck-suppress constParameterReference // the write goes through the LE field helpers
inline void setFlags(Buffer& b, const uint16_t mask) { storeLE16(b.bytes + kHdrFlags, flags(b) | mask); }
inline uint64_t entriesTotal(const Buffer& b) { return loadLE64(b.bytes + kHdrEntriesTotal); }
inline int64_t sleepUsTotal(const Buffer& b) { return static_cast<int64_t>(loadLE64(b.bytes + kHdrSleepUsTotal)); }
inline uint32_t classSeconds(const Buffer& b, const IntervalClass cls) {
  switch (cls) {
    case IntervalClass::Advertising:
      return loadLE32(b.bytes + kHdrAdvertisingSec);
    case IntervalClass::Connected:
      return loadLE32(b.bytes + kHdrConnectedSec);
    default:
      return loadLE32(b.bytes + kHdrOtherSec);
  }
}

inline uint32_t totalRecords(const Buffer& b) {
  return count(b, kHdrPeriodicPmCount) + count(b, kHdrTransitionPmCount) + count(b, kHdrHeartbeatCount) +
         count(b, kHdrTransitionCount) + count(b, kHdrErrorCount) + count(b, kHdrSnapshotCount);
}

// CRC over the whole used region (header + record areas): deterministic for a
// finished session, changes with any recorded byte.
inline uint32_t bufferCrc32(const Buffer& b) { return crc32(b.bytes, kBufferNeeded); }

// --- Interval attribution -----------------------------------------------------
// The device side samples at every state transition and at the periodic
// cadence, so a valid delta always spans a single radio state; the span is
// tagged with the state that held during it. An invalid capture (a failed
// dump, backwards counters) keeps its state class with samplesValid=0: the
// wall time stays in that class's population, the sleep is not counted.
inline IntervalClass classifyInterval(const uint8_t stateAtSampleStart) {
  switch (stateAtSampleStart) {
    case kRadioAdvertising:
      return IntervalClass::Advertising;
    case kRadioAwaitingAuth:
    case kRadioConnected:
      return IntervalClass::Connected;
    default:
      return IntervalClass::Other;
  }
}

// Wall time and 64-bit sleep accounting for one closed interval. Wall time is
// always accumulated into the span's state class; the sleep counters move only
// with valid samples, so an invalid span lowers both coverage and residency
// (conservative) instead of fabricating sleep or leaving the population.
struct IntervalTotals {
  uint32_t wallSec = 0;
  uint32_t entriesDelta = 0;
  uint32_t rejectsDelta = 0;
  int64_t modeUsDelta = 0;
};

// cppcheck-suppress constParameterReference // the write goes through the LE field helpers
inline void accumulateInterval(Buffer& b, const IntervalClass cls, const IntervalTotals& in, const bool samplesValid) {
  size_t offset;
  switch (cls) {
    case IntervalClass::Advertising:
      offset = kHdrAdvertisingSec;
      break;
    case IntervalClass::Connected:
      offset = kHdrConnectedSec;
      break;
    default:
      offset = kHdrOtherSec;
      break;
  }
  storeLE32(b.bytes + offset, loadLE32(b.bytes + offset) + in.wallSec);
  if (samplesValid) {
    storeLE64(b.bytes + kHdrEntriesTotal, entriesTotal(b) + in.entriesDelta);
    storeLE64(b.bytes + kHdrSleepUsTotal, static_cast<uint64_t>(sleepUsTotal(b) + in.modeUsDelta));
  }
}

// --- Record appends (bounded, drop-counting) ---------------------------------
struct PmRecord {
  uint32_t startSec = 0;
  uint32_t endSec = 0;
  uint32_t entriesDelta = 0;
  uint32_t rejectsDelta = 0;
  int64_t modeUsDelta = 0;
  IntervalClass intervalClass = IntervalClass::Other;
  bool samplesValid = false;
};

inline void encodePmSample(uint8_t* p, const PmRecord& r) {
  storeLE32(p + 0, r.startSec);
  storeLE32(p + 4, r.endSec);
  storeLE32(p + 8, r.entriesDelta);
  storeLE32(p + 12, r.rejectsDelta);
  storeLE64(p + 16, static_cast<uint64_t>(r.modeUsDelta));
  p[24] = static_cast<uint8_t>(r.intervalClass);
  p[25] = r.samplesValid ? 1 : 0;
}

// cppcheck-suppress constParameterReference // the write goes through the LE field helpers
inline bool appendPmSample(Buffer& b, const PmRecord& r, const bool transitionDriven) {
  const size_t offset = transitionDriven ? kTransitionPmOffset : kPeriodicPmOffset;
  const size_t droppedOffset = transitionDriven ? kHdrDroppedTransitionPm : kHdrDroppedPeriodicPm;
  const uint32_t capacity = transitionDriven ? kTransitionPmCapacity : kPeriodicPmCapacity;
  const uint32_t index = loadLE32(b.bytes + (transitionDriven ? kHdrTransitionPmCount : kHdrPeriodicPmCount));
  if (index >= capacity) {
    storeLE32(b.bytes + droppedOffset, loadLE32(b.bytes + droppedOffset) + 1);
    return false;
  }
  encodePmSample(b.bytes + offset + index * kPmSampleSize, r);
  storeLE32(b.bytes + (transitionDriven ? kHdrTransitionPmCount : kHdrPeriodicPmCount), index + 1);
  return true;
}

inline bool appendHeartbeat(Buffer& b, const uint32_t sec, const uint8_t radioState, const uint32_t freeHeap,
                            const uint32_t largestBlock) {
  const uint32_t index = loadLE32(b.bytes + kHdrHeartbeatCount);
  if (index >= kHeartbeatCapacity) {
    storeLE32(b.bytes + kHdrDroppedHeartbeat, loadLE32(b.bytes + kHdrDroppedHeartbeat) + 1);
    return false;
  }
  uint8_t* p = b.bytes + kHeartbeatOffset + index * kHeartbeatSize;
  storeLE32(p + 0, sec);
  p[4] = radioState;
  p[5] = 0;
  storeLE32(p + 8, freeHeap);
  storeLE32(p + 12, largestBlock);
  storeLE32(b.bytes + kHdrHeartbeatCount, index + 1);
  return true;
}

inline bool appendTransition(Buffer& b, const uint32_t sec, const uint8_t fromState, const uint8_t toState) {
  const uint32_t index = loadLE32(b.bytes + kHdrTransitionCount);
  if (index >= kTransitionCapacity) {
    storeLE32(b.bytes + kHdrDroppedTransition, loadLE32(b.bytes + kHdrDroppedTransition) + 1);
    setFlags(b, kFlagTransitionOverflow);
    return false;
  }
  uint8_t* p = b.bytes + kTransitionOffset + index * kTransitionSize;
  storeLE32(p + 0, sec);
  p[4] = fromState;
  p[5] = toState;
  p[6] = 0;
  p[7] = 0;
  storeLE32(b.bytes + kHdrTransitionCount, index + 1);
  return true;
}

inline bool appendError(Buffer& b, const uint32_t sec, const uint16_t code, const uint16_t context,
                        const uint32_t aux) {
  const uint32_t index = loadLE32(b.bytes + kHdrErrorCount);
  if (index >= kErrorCapacity) {
    storeLE32(b.bytes + kHdrDroppedError, loadLE32(b.bytes + kHdrDroppedError) + 1);
    return false;
  }
  uint8_t* p = b.bytes + kErrorOffset + index * kErrorSize;
  storeLE32(p + 0, sec);
  storeLE16(p + 4, code);
  storeLE16(p + 6, context);
  storeLE32(p + 8, aux);
  storeLE32(b.bytes + kHdrErrorCount, index + 1);
  return true;
}

inline void recordStartup(Buffer& b, const uint8_t lpclkSource, const uint8_t crystalVerdict,
                          const uint32_t configEcho) {
  uint8_t* p = b.bytes + kStartupOffset;
  p[0] = lpclkSource;
  p[1] = crystalVerdict;
  p[2] = 0;
  p[3] = 0;
  storeLE32(p + 4, configEcho);
  if (lpclkSource != 0) setFlags(b, kFlagClockRecorded);
}

struct StartupRecord {
  uint8_t lpclk = 0;
  uint8_t crystal = kCrystalNotRecorded;
  uint32_t configEcho = 0;
};

inline StartupRecord readStartup(const Buffer& b) {
  const uint8_t* p = b.bytes + kStartupOffset;
  StartupRecord out;
  out.lpclk = p[0];
  out.crystal = p[1];
  out.configEcho = loadLE32(p + 4);
  return out;
}

// Heap snapshot at the actual session boundaries (C1 correction 2): one at
// sessionBegin, one at noteExit, independent of the 1-minute heartbeat ticks.
// Tag: 1 = session start, 2 = session exit.
inline bool appendSnapshot(Buffer& b, const uint32_t sec, const uint32_t freeHeap, const uint32_t largestBlock,
                           const bool atExit) {
  const uint32_t index = loadLE32(b.bytes + kHdrSnapshotCount);
  if (index >= kSnapshotCapacity) {
    storeLE32(b.bytes + kHdrDroppedSnapshot, loadLE32(b.bytes + kHdrDroppedSnapshot) + 1);
    return false;
  }
  uint8_t* p = b.bytes + kSnapshotOffset + index * kSnapshotSize;
  storeLE32(p + 0, sec);
  storeLE32(p + 4, freeHeap);
  storeLE32(p + 8, largestBlock);
  p[12] = atExit ? 2 : 1;
  p[13] = 0;
  p[14] = 0;
  p[15] = 0;
  storeLE32(b.bytes + kHdrSnapshotCount, index + 1);
  return true;
}

// --- Acceptance logic ---------------------------------------------------------
// Residency in per-ten-thousand: sleepUs over the class wall time. Computed
// in 64-bit; no floating point, no current inference.
inline uint64_t residencyPermyriad(const uint64_t sleepUs, const uint64_t wallSec) {
  if (wallSec == 0) return 0;
  return sleepUs / (wallSec * 100);  // sleepUs / (wallSec * 1e6) * 1e4
}

// Wall-time coverage of the stored VALID advertising samples against the
// advertising class wall time. Invalid advertising spans and dropped records
// stay in the denominator (their wall time was accumulated into the class) but
// never in the numerator, so unmeasured time lowers coverage — it can never
// inflate the reported coverage (C1 blocker 1). The same iteration sums the
// advertising-class sleep: the per-class sleep lives in the records (the
// header carries only the overall 64-bit total), so a dropped or invalid
// record lowers both coverage and measured sleep — conservative, never a
// false PASS.
inline void storedAdvertisingEvidence(const Buffer& b, uint64_t& coveredSec, uint64_t& sleepUs) {
  coveredSec = 0;
  sleepUs = 0;
  const uint32_t periodic = count(b, kHdrPeriodicPmCount);
  for (uint32_t i = 0; i < periodic; ++i) {
    const uint8_t* p = b.bytes + kPeriodicPmOffset + i * kPmSampleSize;
    if (p[24] == static_cast<uint8_t>(IntervalClass::Advertising) && p[25] != 0) {
      coveredSec += static_cast<uint64_t>(loadLE32(p + 4)) - loadLE32(p);
      sleepUs += static_cast<uint64_t>(loadLE64(p + 16));
    }
  }
  const uint32_t transitioned = count(b, kHdrTransitionPmCount);
  for (uint32_t i = 0; i < transitioned; ++i) {
    const uint8_t* p = b.bytes + kTransitionPmOffset + i * kPmSampleSize;
    if (p[24] == static_cast<uint8_t>(IntervalClass::Advertising) && p[25] != 0) {
      coveredSec += static_cast<uint64_t>(loadLE32(p + 4)) - loadLE32(p);
      sleepUs += static_cast<uint64_t>(loadLE64(p + 16));
    }
  }
}

inline uint64_t storedAdvertisingSec(const Buffer& b) {
  uint64_t covered = 0, sleep = 0;
  storedAdvertisingEvidence(b, covered, sleep);
  return covered;
}

// Coverage in per-ten-thousand of the advertising class wall time.
inline uint64_t advertisingCoveragePermyriad(const Buffer& b) {
  const uint64_t wall = classSeconds(b, IntervalClass::Advertising);
  if (wall == 0) return 0;
  return storedAdvertisingSec(b) * 10000 / wall;
}

// Sample-validity census of the recovered evidence (C1 correction 4): how
// much of each PM bucket was measured at all.
struct PmValidity {
  uint32_t periodicValid = 0;
  uint32_t periodicInvalid = 0;
  uint32_t transitionValid = 0;
  uint32_t transitionInvalid = 0;
};

inline void pmValidity(const Buffer& b, PmValidity& out) {
  out = PmValidity{};
  const uint32_t periodic = count(b, kHdrPeriodicPmCount);
  for (uint32_t i = 0; i < periodic; ++i) {
    const uint8_t* p = b.bytes + kPeriodicPmOffset + i * kPmSampleSize;
    if (p[25] != 0) {
      ++out.periodicValid;
    } else {
      ++out.periodicInvalid;
    }
  }
  const uint32_t transitioned = count(b, kHdrTransitionPmCount);
  for (uint32_t i = 0; i < transitioned; ++i) {
    const uint8_t* p = b.bytes + kTransitionPmOffset + i * kPmSampleSize;
    if (p[25] != 0) {
      ++out.transitionValid;
    } else {
      ++out.transitionInvalid;
    }
  }
}

// The residency verdict judges ONLY the measured advertising residency: a
// valid measurement below 90% is a FAIL; insufficient measured evidence
// (no advertising wall time, coverage below 98%, an overflowed transition
// history) is INCONCLUSIVE — never a false PASS. The BLE clock-source
// validity is a separate verdict below (C1 correction 1).
enum class Verdict : uint8_t { Pass, Fail, Inconclusive };

inline Verdict advertisingResidencyVerdict(const Buffer& b) {
  if ((flags(b) & kFlagTransitionOverflow) != 0) return Verdict::Inconclusive;
  const uint64_t wall = classSeconds(b, IntervalClass::Advertising);
  if (wall == 0) return Verdict::Inconclusive;
  uint64_t covered = 0, advertisingSleepUs = 0;
  storedAdvertisingEvidence(b, covered, advertisingSleepUs);
  if (covered * 10000 < wall * 9800) return Verdict::Inconclusive;
  return residencyPermyriad(advertisingSleepUs, wall) >= 9000 ? Verdict::Pass : Verdict::Fail;
}

// The BLE sleep-clock verdict, separate from residency: only EXT_32K_XTAL
// validates the electrical measurement (analysis revision 3, §5.3b). A
// MAIN_XTAL fallback is Invalid — a session with it can never read as
// overall acceptance, no matter the measured residency.
enum class ClockVerdict : uint8_t { Valid, Invalid, NotRecorded };

inline ClockVerdict clockVerdict(const Buffer& b) {
  if ((flags(b) & kFlagClockRecorded) == 0) return ClockVerdict::NotRecorded;
  return readStartup(b).lpclk == kLpclkExt32kXtal ? ClockVerdict::Valid : ClockVerdict::Invalid;
}

// Overall B9 acceptance: residency PASS and a validated EXT_32K_XTAL clock.
// Anything less — including a MAIN_XTAL fallback behind a 95% residency — is
// INCONCLUSIVE, never a PASS.
inline Verdict standbyAcceptanceVerdict(const Buffer& b) {
  if (clockVerdict(b) != ClockVerdict::Valid) return Verdict::Inconclusive;
  return advertisingResidencyVerdict(b);
}

#ifdef CROSSPOINT_COMPANION_GATE_STANDBY
// --- device API (implemented in the .cpp; compiled only in the standby env).
// The standby session owns one buffer and one interval timeline; sessionBegin
// must follow the radio's controller initialization (gate.begin()).
uint8_t readLpclkSource();
unsigned long standbyCapMs();
void sessionBegin(unsigned long nowMs);
void tick(unsigned long nowMs, uint8_t radioState);
void noteExit(ExitKind kind);
// One line per boot when retained evidence exists: the readback itself is
// operator-controlled through handleCommand (C1 blocker 2).
void notifyRetained();
// Experimental serial commands: "SBDS" (repeatable dump with counts, record
// indices and a CRC-32), "SBDSACK <crc32>" (clear only on a CRC match),
// "SBDSCLR" (explicit discard). Evidence is retained until one of these
// clears it; the dump never destroys it.
void handleCommand(const char* command);
#endif

}  // namespace companionstandbydiag

// Short alias for the flag-gated call sites in main.cpp.
namespace standbydiag = companionstandbydiag;
