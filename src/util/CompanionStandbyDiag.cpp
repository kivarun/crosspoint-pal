#include "CompanionStandbyDiag.h"

#ifdef CROSSPOINT_COMPANION_GATE_STANDBY

#include <BleCompanion.h>
#include <Logging.h>
#include <esp_bt.h>
#include <esp_heap_caps.h>

#include <cstdlib>
#include <cstring>

#include "CompanionGatePower.h"

namespace companionstandbydiag {
namespace {

// The 40 KiB diagnostic buffer: a static array in .bss, compiled only into
// the x4c-bleautols-standby env. No heap allocation, no other env carries it.
Buffer buffer;

// Seconds since the session started (unsigned arithmetic absorbs millis()
// wrap; the session is bounded by the 24 h cap, far below the 49.7 d wrap).
unsigned long sessionStartMs = 0;
bool sessionActive = false;

uint8_t lastRadioState = kRadioStopped;
bool lastSampled = false;
companiongatepower::SleepSample lastSample{};
uint32_t lastSampleSec = 0;
unsigned long lastPmMs = 0;
unsigned long lastHeartbeatMs = 0;
bool exitNoted = false;

constexpr unsigned long kPmPeriodMs = 5UL * 60 * 1000;
constexpr unsigned long kHeartbeatPeriodMs = 60UL * 1000;
constexpr unsigned long kStandbyCapMs = 24UL * 60 * 60 * 1000;

uint32_t sessionSec(const unsigned long nowMs) { return static_cast<uint32_t>((nowMs - sessionStartMs) / 1000); }

// One closed interval: capture the exit sample, classify the span by the
// radio state that held during it, accumulate the wall time and (with valid
// samples) the 64-bit sleep counters, and store the record. A capture or
// counter failure keeps the span in its state class with samplesValid=0 —
// its wall time stays in that class's measurement population, its sleep is
// not counted, so residency and coverage can only drop (C1 blocker 1). A
// transition at the span's end boundary does not matter: the
// sample-at-both-ends contract tags the span with the state that held inside
// it (analysis revision 3 §5.2).
void closeInterval(const unsigned long nowMs, const bool transitionDriven) {
  const uint32_t startSec = lastSampleSec;
  const uint32_t endSec = sessionSec(nowMs);
  const uint32_t wallSec = endSec > startSec ? endSec - startSec : 0;

  companiongatepower::SleepSample sample{};
  const bool sampled = companiongatepower::captureSleepSample(sample);
  bool samplesValid = false;
  IntervalTotals totals{wallSec, 0, 0, 0};
  if (lastSampled && sampled) {
    companiongatepower::WindowDelta delta{};
    if (companiongatepower::windowDelta(lastSample, sample, delta)) {
      totals = IntervalTotals{wallSec, static_cast<uint32_t>(delta.entries), static_cast<uint32_t>(delta.rejects),
                              delta.modeUs};
      samplesValid = true;
    }
  }
  const IntervalClass cls = classifyInterval(lastRadioState);
  accumulateInterval(buffer, cls, totals, samplesValid);

  PmRecord record;
  record.startSec = startSec;
  record.endSec = endSec;
  record.entriesDelta = totals.entriesDelta;
  record.rejectsDelta = totals.rejectsDelta;
  record.modeUsDelta = totals.modeUsDelta;
  record.intervalClass = cls;
  record.samplesValid = samplesValid;
  appendPmSample(buffer, record, transitionDriven);

  lastSample = sample;
  lastSampled = sampled;
  lastSampleSec = endSec;
  lastPmMs = nowMs;
}

void sampleNow(const unsigned long nowMs) { closeInterval(nowMs, false); }

const char* stateName(const uint8_t state) {
  switch (state) {
    case kRadioAdvertising:
      return "advertising";
    case kRadioAwaitingAuth:
      return "awaiting-auth";
    case kRadioConnected:
      return "connected";
    default:
      return "stopped";
  }
}

const char* verdictName(const Verdict verdict) {
  switch (verdict) {
    case Verdict::Pass:
      return "PASS";
    case Verdict::Fail:
      return "FAIL";
    default:
      return "INCONCLUSIVE";
  }
}

const char* clockVerdictName(const ClockVerdict verdict) {
  switch (verdict) {
    case ClockVerdict::Valid:
      return "VALID";
    case ClockVerdict::Invalid:
      return "INVALID";
    default:
      return "NOT-RECORDED";
  }
}

const char* clockSourceName(const uint8_t source) {
  switch (source) {
    case kLpclkMainXtal:
      return "MAIN_XTAL";
    case kLpclkExt32kXtal:
      return "EXT_32K_XTAL";
    default:
      return "other";
  }
}

const char* exitKindName(const uint16_t headerFlags) {
  if ((headerFlags & kFlagExitUsb) != 0) return "usb";
  if ((headerFlags & kFlagExitPower) != 0) return "power";
  return "none";
}

// The once-per-boot readback: dump without destroying, counts and a CRC-32 to
// detect truncation, and a clear only on operator command (C1 blocker 2).
void dumpEvidence() {
  if (!headerValid(buffer) || totalRecords(buffer) == 0) {
    // A reset buffer (cleared or never used) has zero records: a real
    // session always carries at least the startup record and a snapshot.
    logSerial.printf("SBDS| no retained evidence (buffer empty or already discarded)\n");
    return;
  }
  const uint32_t crc = bufferCrc32(buffer);

  logSerial.printf(
      "SBDS|BEGIN pmP=%u pmT=%u hb=%u evt=%u err=%u snap=%u drops: pmP=%u pmT=%u hb=%u evt=%u err=%u "
      "snap=%u flags=0x%x\n",
      static_cast<unsigned>(count(buffer, kHdrPeriodicPmCount)),
      static_cast<unsigned>(count(buffer, kHdrTransitionPmCount)),
      static_cast<unsigned>(count(buffer, kHdrHeartbeatCount)),
      static_cast<unsigned>(count(buffer, kHdrTransitionCount)), static_cast<unsigned>(count(buffer, kHdrErrorCount)),
      static_cast<unsigned>(count(buffer, kHdrSnapshotCount)),
      static_cast<unsigned>(dropped(buffer, kHdrDroppedPeriodicPm)),
      static_cast<unsigned>(dropped(buffer, kHdrDroppedTransitionPm)),
      static_cast<unsigned>(dropped(buffer, kHdrDroppedHeartbeat)),
      static_cast<unsigned>(dropped(buffer, kHdrDroppedTransition)),
      static_cast<unsigned>(dropped(buffer, kHdrDroppedError)),
      static_cast<unsigned>(dropped(buffer, kHdrDroppedSnapshot)), static_cast<unsigned>(flags(buffer)));

  // Two separate results (C1 correction 1): the measured residency and the
  // BLE clock validity. The overall line names their conjunction, so a
  // MAIN_XTAL fallback can never read as B9 acceptance.
  const uint64_t wall = classSeconds(buffer, IntervalClass::Advertising);
  uint64_t covered = 0, advertisingSleepUs = 0;
  storedAdvertisingEvidence(buffer, covered, advertisingSleepUs);
  const uint64_t residency = residencyPermyriad(advertisingSleepUs, wall);
  const uint64_t coverage = advertisingCoveragePermyriad(buffer);
  const Verdict residencyVerdict = advertisingResidencyVerdict(buffer);
  logSerial.printf(
      "SBDS| residency: %s advWall=%llu s measured=%llu s sleepUs=%lld residency=%llu.%02llu%% "
      "coverage=%llu.%02llu%%\n",
      verdictName(residencyVerdict), static_cast<unsigned long long>(wall), static_cast<unsigned long long>(covered),
      static_cast<long long>(advertisingSleepUs), static_cast<unsigned long long>(residency / 100),
      static_cast<unsigned long long>(residency % 100), static_cast<unsigned long long>(coverage / 100),
      static_cast<unsigned long long>(coverage % 100));
  const StartupRecord startup = readStartup(buffer);
  const ClockVerdict clock = clockVerdict(buffer);
  logSerial.printf("SBDS| clock: lpclk=%u (%s) verdict=%s crystal=%u\n", static_cast<unsigned>(startup.lpclk),
                   clockSourceName(startup.lpclk), clockVerdictName(clock), static_cast<unsigned>(startup.crystal));
  logSerial.printf("SBDS| overall: %s (PASS requires residency PASS and lpclk=EXT_32K_XTAL)\n",
                   verdictName(standbyAcceptanceVerdict(buffer)));

  logSerial.printf("SBDS| session end kind=%s advSec=%u connSec=%u otherSec=%u entries=%llu sleepUs=%lld\n",
                   exitKindName(flags(buffer)), static_cast<unsigned>(classSeconds(buffer, IntervalClass::Advertising)),
                   static_cast<unsigned>(classSeconds(buffer, IntervalClass::Connected)),
                   static_cast<unsigned>(classSeconds(buffer, IntervalClass::Other)),
                   static_cast<unsigned long long>(entriesTotal(buffer)), static_cast<long long>(sleepUsTotal(buffer)));

  PmValidity counted{};
  pmValidity(buffer, counted);
  logSerial.printf("SBDS| pm validity: periodic valid=%u invalid=%u transition valid=%u invalid=%u\n",
                   static_cast<unsigned>(counted.periodicValid), static_cast<unsigned>(counted.periodicInvalid),
                   static_cast<unsigned>(counted.transitionValid), static_cast<unsigned>(counted.transitionInvalid));

  const uint32_t snapshotCount = count(buffer, kHdrSnapshotCount);
  for (uint32_t i = 0; i < snapshotCount; ++i) {
    const uint8_t* p = buffer.bytes + kSnapshotOffset + i * kSnapshotSize;
    logSerial.printf("SBDS|SNAP[%u] %u heap=%u largest=%u kind=%s\n", static_cast<unsigned>(i + 1),
                     static_cast<unsigned>(loadLE32(p)), static_cast<unsigned>(loadLE32(p + 4)),
                     static_cast<unsigned>(loadLE32(p + 8)), p[12] == 2 ? "exit" : "start");
  }
  const uint32_t heartbeatCount = count(buffer, kHdrHeartbeatCount);
  for (uint32_t i = 0; i < heartbeatCount; ++i) {
    const uint8_t* p = buffer.bytes + kHeartbeatOffset + i * kHeartbeatSize;
    logSerial.printf("SBDS|HB[%u] %u %s heap=%u largest=%u\n", static_cast<unsigned>(i + 1),
                     static_cast<unsigned>(loadLE32(p)), stateName(p[4]), static_cast<unsigned>(loadLE32(p + 8)),
                     static_cast<unsigned>(loadLE32(p + 12)));
  }
  const uint32_t transitionCount = count(buffer, kHdrTransitionCount);
  for (uint32_t i = 0; i < transitionCount; ++i) {
    const uint8_t* p = buffer.bytes + kTransitionOffset + i * kTransitionSize;
    logSerial.printf("SBDS|EVT[%u] %u %s->%s\n", static_cast<unsigned>(i + 1), static_cast<unsigned>(loadLE32(p)),
                     stateName(p[4]), stateName(p[5]));
  }
  const uint32_t errorCount = count(buffer, kHdrErrorCount);
  for (uint32_t i = 0; i < errorCount; ++i) {
    const uint8_t* p = buffer.bytes + kErrorOffset + i * kErrorSize;
    logSerial.printf("SBDS|ERR[%u] %u code=%u ctx=%u aux=%u\n", static_cast<unsigned>(i + 1),
                     static_cast<unsigned>(loadLE32(p)), static_cast<unsigned>(loadLE16(p + 4)),
                     static_cast<unsigned>(loadLE16(p + 6)), static_cast<unsigned>(loadLE32(p + 8)));
  }
  const uint32_t pmCounts[2] = {count(buffer, kHdrPeriodicPmCount), count(buffer, kHdrTransitionPmCount)};
  for (int bucket = 0; bucket < 2; ++bucket) {
    const uint8_t* base = bucket == 0 ? buffer.bytes + kPeriodicPmOffset : buffer.bytes + kTransitionPmOffset;
    for (uint32_t i = 0; i < pmCounts[bucket]; ++i) {
      const uint8_t* p = base + i * kPmSampleSize;
      logSerial.printf("SBDS|PM[%u] %c %u-%u cls=%u v=%u ent=%u rej=%u us=%lld\n", static_cast<unsigned>(i + 1),
                       bucket == 0 ? 'P' : 'T', static_cast<unsigned>(loadLE32(p)),
                       static_cast<unsigned>(loadLE32(p + 4)), static_cast<unsigned>(p[24]),
                       static_cast<unsigned>(p[25]), static_cast<unsigned>(loadLE32(p + 8)),
                       static_cast<unsigned>(loadLE32(p + 12)), static_cast<long long>(loadLE64(p + 16)));
    }
  }
  logSerial.printf(
      "SBDS|END crc32=%08lx records=%u (re-dump repeats identically; CMD:SBDSACK <crc32> clears on a "
      "match, CMD:SBDSCLR discards)\n",
      static_cast<unsigned long>(crc), static_cast<unsigned>(totalRecords(buffer)));
}

}  // namespace

// The interval attribution keys on blecompanion::RadioState's numeric values;
// a reordering there would silently corrupt every record. Pin it.
static_assert(static_cast<uint8_t>(blecompanion::RadioState::Stopped) == kRadioStopped, "radio state mapping");
static_assert(static_cast<uint8_t>(blecompanion::RadioState::Advertising) == kRadioAdvertising, "radio state mapping");
static_assert(static_cast<uint8_t>(blecompanion::RadioState::AwaitingAuthentication) == kRadioAwaitingAuth,
              "radio state mapping");
static_assert(static_cast<uint8_t>(blecompanion::RadioState::Connected) == kRadioConnected, "radio state mapping");

// The runtime BLE sleep-clock source, read only after the controller is up
// (NimBLEDevice::init inside gate.begin()). The raw esp_bt_sleep_clock_t is
// stored; EXT_32K_XTAL is the only value that validates the electrical
// measurement (analysis revision 3, §5.3b).
uint8_t readLpclkSource() { return static_cast<uint8_t>(esp_bt_get_lpclk_src()); }

unsigned long standbyCapMs() { return kStandbyCapMs; }

void sessionBegin(const unsigned long nowMs) {
  reset(buffer);
  sessionStartMs = nowMs;
  sessionActive = true;
  exitNoted = false;
  lastRadioState = kRadioStopped;
  lastSampleSec = 0;
  lastPmMs = nowMs;
  lastHeartbeatMs = nowMs;
  const uint8_t lpclk = readLpclkSource();
  recordStartup(buffer, lpclk, kCrystalNotRecorded, 0);
  // The crystal preflight is a separate RTCCLK session (analysis §5.3a); the
  // firmware cannot verify it here and stores kCrystalNotRecorded on purpose.
  // The start heap snapshot and the entry PM sample anchor the session's heap
  // timeline and make the first interval measurable (the Guard configured
  // light sleep before this point, so the dump carries the Sleep block).
  (void)appendSnapshot(buffer, 0, heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
                       heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT), false);
  lastSampled = companiongatepower::captureSleepSample(lastSample);
}

// One main-loop pass of the standby session: radio-state transitions close
// intervals (transition-driven PM sample + event record), the 5-minute and
// 1-minute cadences run the periodic samples and the heartbeat. All output
// goes to the RAM buffer: the battery phase issues no console writes.
void tick(const unsigned long nowMs, const uint8_t radioState) {
  if (!sessionActive) return;
  if (radioState != lastRadioState) {
    closeInterval(nowMs, /*transitionDriven=*/true);
    appendTransition(buffer, sessionSec(nowMs), lastRadioState, radioState);
    lastRadioState = radioState;
  }
  if (nowMs - lastHeartbeatMs >= kHeartbeatPeriodMs) {
    lastHeartbeatMs = nowMs;
    appendHeartbeat(buffer, sessionSec(nowMs), radioState, heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
                    heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
  }
  if (nowMs - lastPmMs >= kPmPeriodMs) sampleNow(nowMs);
}

void noteExit(const ExitKind kind) {
  if (!sessionActive || exitNoted) return;
  exitNoted = true;
  sessionActive = false;
  // The final PM capture runs while the Guard's light sleep is still on.
  closeInterval(millis(), false);
  (void)appendSnapshot(buffer, sessionSec(millis()), heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
                       heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT), true);
  switch (kind) {
    case ExitKind::Power:
      setFlags(buffer, kFlagExitPower);
      break;
    case ExitKind::Usb:
      setFlags(buffer, kFlagExitUsb);
      break;
    case ExitKind::Cap:
      // The diagnostic cap ends in deep sleep: the RAM evidence would be lost
      // anyway, and the gate never returns to a console from here. Discard
      // the buffer now so no stale evidence can ever be presented, and the
      // session reads INCONCLUSIVE by absence of evidence (analysis §7).
      reset(buffer);
      break;
  }
}

// One line per boot when retained evidence exists, so the operator knows the
// readback is available; the dump itself is operator-controlled below.
void notifyRetained() {
  static bool announced = false;
  if (announced || sessionActive || !headerValid(buffer)) return;
  announced = true;
  logSerial.printf("SBDS| retained standby evidence present (%u records); CMD:SBDS reads it, CMD:SBDSCLR discards\n",
                   static_cast<unsigned>(totalRecords(buffer)));
}

void handleCommand(const char* command) {
  if (command == nullptr || sessionActive) return;
  if (strcmp(command, "SBDS") == 0) {
    // Repeatable: the dump never modifies the buffer.
    dumpEvidence();
    return;
  }
  if (strcmp(command, "SBDSCLR") == 0) {
    if (!headerValid(buffer) || totalRecords(buffer) == 0) {
      logSerial.printf("SBDS| nothing to discard\n");
      return;
    }
    reset(buffer);
    logSerial.printf("SBDS| evidence discarded by operator command\n");
    return;
  }
  if (strncmp(command, "SBDSACK", 7) == 0) {
    // Clear only on a CRC match: the operator copies the CRC from the END
    // line of a complete dump, so a truncated or missing readback can never
    // be acknowledged.
    const char* arg = command + 7;
    while (*arg == ' ') ++arg;
    char* end = nullptr;
    const unsigned long value = strtoul(arg, &end, 16);
    if (end == arg || (end != nullptr && *end != '\0')) {
      logSerial.printf("SBDS| ACK rejected: usage CMD:SBDSACK <crc32 from the END line>\n");
      return;
    }
    if (!headerValid(buffer) || totalRecords(buffer) == 0) {
      logSerial.printf("SBDS| nothing to acknowledge\n");
      return;
    }
    if (static_cast<uint32_t>(value) != bufferCrc32(buffer)) {
      logSerial.printf("SBDS| ACK rejected: crc mismatch (evidence retained)\n");
      return;
    }
    reset(buffer);
    logSerial.printf("SBDS| acknowledged (crc match); evidence cleared\n");
    return;
  }
}

}  // namespace companionstandbydiag
#endif  // CROSSPOINT_COMPANION_GATE_STANDBY
