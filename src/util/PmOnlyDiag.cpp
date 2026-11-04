#include "PmOnlyDiag.h"

#ifdef CROSSPOINT_PM_ONLY_DIAG

#include <Logging.h>
#include <esp_pm.h>
#include <sys/types.h>

#include <cstdio>
#include <cstring>

#if defined(CROSSPOINT_PM_LSTEST) && CROSSPOINT_PM_LSTEST
#include <HalStorage.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <hal/usb_serial_jtag_ll.h>
#endif

// DFS floor per experiment: 40 MHz = B1k (the XTAL-floor state), 80 MHz =
// B1l leg (b) and B1n (PLL-only).
#ifndef CROSSPOINT_PM_MIN_FREQ
#define CROSSPOINT_PM_MIN_FREQ 40
#endif

// Automatic light sleep: off in B1k/B1l, on in B1n (tickless required).
#ifndef CROSSPOINT_PM_AUTOLS
#define CROSSPOINT_PM_AUTOLS 0
#endif

namespace pmonlydiag {
namespace {

constexpr const char* TAG = "PMONLY";

// esp_pm_dump_locks writes to a stdio FILE*, and this core's stdio routes to
// the primary console, not the monitor the UAT reads. Wrap the serial log
// reference in a FILE* so the dump reaches the operator on demand.
uint32_t sDumpBytes = 0;

ssize_t logCookieWrite(void*, const char* buf, size_t len) {
  sDumpBytes += len;
  logSerial.write(reinterpret_cast<const uint8_t*>(buf), len);
  return static_cast<ssize_t>(len);
}

FILE* dumpFile() {
  static FILE* file = nullptr;
  if (file == nullptr) {
    static const cookie_io_functions_t io = {nullptr, logCookieWrite, nullptr, nullptr};
    file = fopencookie(nullptr, "w", io);
  }
  return file;
}

#if defined(CROSSPOINT_PM_LSTEST) && CROSSPOINT_PM_LSTEST

// p1-b1o RX-path diagnostics. The UAT saw TX flow while CMD:PMSTAT never
// answered, and every static link of the RX chain verified correct in the
// ELF (HWCDC::begin from setup, the ISR's OUT_RECV_PKT branch, the queue
// push loop, the loop()-side available()/readStringUntil dispatch). These
// counters and the USJ register snapshot discriminate the remaining runtime
// states: bytes never reach the ISR (int_raw bit2 stays clear and the RX
// event counter stays 0), the ISR never drains them (int_raw bit2 and
// serial_out_ep_data_avail stay set), or the queue/dispatch side drops.
volatile uint32_t sRxEventCount = 0;
volatile uint32_t sRxEventBytes = 0;
// HWCDC::write() silently discards the tail of a write once 20 consecutive
// 1 ms ring-space timeouts expire (setTxTimeoutMs(1), main.cpp). RX1 lost
// exactly its heartbeat this way. Count every short write the diagnostics
// perform so a repeat is visible in RXSTAT instead of invisible.
volatile uint32_t sTxShortWrites = 0;

void onHwCdcRxEvent(void*, esp_event_base_t, int32_t, void* data) {
  const auto* event = static_cast<const arduino_hw_cdc_event_data_t*>(data);
  if (event != nullptr) {
    sRxEventBytes = sRxEventBytes + event->rx.len;
  }
  sRxEventCount = sRxEventCount + 1;
}

void logUsjState(const char* what) {
  const uint32_t intEna = USB_SERIAL_JTAG.int_ena.val;
  const uint32_t intRaw = USB_SERIAL_JTAG.int_raw.val;
  const uint32_t intSt = USB_SERIAL_JTAG.int_st.val;
  const uint32_t ep1Conf = USB_SERIAL_JTAG.ep1_conf.val;
  const uint32_t conf0 = USB_SERIAL_JTAG.conf0.val;
  // bit2 of int_raw/int_ena and serial_out_ep_data_avail (ep1_conf bit2) are
  // the RX markers; SOF (bit1) is owned by the IDF tick hook and is
  // incidental in a snapshot.
  LOG_INF(TAG, "%s: intEna=0x%lx intRaw=0x%lx intSt=0x%lx outDataAvail=%lu conf0=0x%lx rxEvents=%lu rxBytes=%lu", what,
          static_cast<unsigned long>(intEna), static_cast<unsigned long>(intRaw), static_cast<unsigned long>(intSt),
          static_cast<unsigned long>((ep1Conf >> 2) & 1), static_cast<unsigned long>(conf0),
          static_cast<unsigned long>(sRxEventCount), static_cast<unsigned long>(sRxEventBytes));
}

// Direct write with delivery accounting: Print::print/printf discard
// HWCDC::write()'s return value, so a bounded-send short write would stay
// invisible. Used for the periodic lines the UAT protocol depends on;
// on-demand command answers stay on printf — a bounded wait is acceptable
// there, and an answer only exists once RX already works.
void writeChecked(const char* buf, size_t len) {
  size_t written = logSerial.write(reinterpret_cast<const uint8_t*>(buf), len);
  if (written < len) {
    sTxShortWrites = sTxShortWrites + 1;
  }
}

#endif  // CROSSPOINT_PM_LSTEST

}  // namespace

void begin() {
  esp_pm_config_t pm = {};
  pm.max_freq_mhz = 240;
  pm.min_freq_mhz = CROSSPOINT_PM_MIN_FREQ;
  constexpr bool kAutoLightSleep = CROSSPOINT_PM_AUTOLS != 0;
  pm.light_sleep_enable = kAutoLightSleep;
  const esp_err_t err = esp_pm_configure(&pm);
  LOG_INF(TAG, "esp_pm_configure(240/%d MHz, automatic light sleep %s): %s", CROSSPOINT_PM_MIN_FREQ,
          kAutoLightSleep ? "ON" : "OFF", err == ESP_OK ? "OK" : esp_err_to_name(err));
  if (err != ESP_OK) {
    // A failed configure invalidates the experiment: halt on serial so no
    // USB session can be scored against the wrong build.
    LOG_ERR(TAG, "halting — PM configure failed, experiment invalid");
    while (true) {
      delay(1000);
    }
  }
#if defined(CROSSPOINT_PM_LSTEST) && CROSSPOINT_PM_LSTEST
  // Arm the RX-path counters: the first onEvent call creates the CDC event
  // loop, and every RX packet the ISR sees is counted from then on. The
  // armed sign is short and delivery-checked; the RX1 boot snapshot was a
  // full register line that the boot log flood could silently discard.
  logSerial.onEvent(ARDUINO_HW_CDC_RX_EVENT, onHwCdcRxEvent);
  char armed[64];
  int armedLen = snprintf(armed, sizeof(armed), "RX armed: intEna=0x%lx intRaw=0x%lx\n",
                          static_cast<unsigned long>(USB_SERIAL_JTAG.int_ena.val),
                          static_cast<unsigned long>(USB_SERIAL_JTAG.int_raw.val));
  if (armedLen > 0 && armedLen < static_cast<int>(sizeof(armed))) {
    writeChecked(armed, static_cast<size_t>(armedLen));
  }
#endif
}

void handleCommand(const char* command) {
  // On-demand only so logging never blocks light sleep by itself. With
  // CONFIG_PM_PROFILING the dump carries per-mode time and light-sleep entry
  // counts, the hardware evidence for the B1n UAT.
  if (command != nullptr && strcmp(command, "PMSTAT") == 0) {
    FILE* file = dumpFile();
    if (file == nullptr) {
      LOG_ERR(TAG, "PMSTAT: could not create the serial dump stream");
      return;
    }
    sDumpBytes = 0;
    LOG_INF(TAG, "PM lock dump follows");
    esp_pm_dump_locks(file);
    fflush(file);
#if defined(CROSSPOINT_PM_LSTEST) && CROSSPOINT_PM_LSTEST
    if (sDumpBytes > 0) {
      logSerial.printf("PMSTAT OK: %lu bytes dumped\n", static_cast<unsigned long>(sDumpBytes));
    } else {
      logSerial.printf("PMSTAT ERROR: the dump produced no output\n");
    }
#endif
  }
#if defined(CROSSPOINT_PM_LSTEST) && CROSSPOINT_PM_LSTEST
  if (command != nullptr && strcmp(command, "RXSTAT") == 0) {
    logUsjState("RXSTAT");
    logSerial.printf("RXSTAT OK: rxEvents=%lu rxBytes=%lu txShort=%lu\n", static_cast<unsigned long>(sRxEventCount),
                     static_cast<unsigned long>(sRxEventBytes), static_cast<unsigned long>(sTxShortWrites));
  }
#endif
}

#if defined(CROSSPOINT_PM_LSTEST) && CROSSPOINT_PM_LSTEST
namespace {

// The profiling dump, parsed from a fixed static capture buffer: the only
// public programmatic surface for the light-sleep counters. esp_pm keeps
// s_light_sleep_counts / s_light_sleep_reject_counts internal, and pm_impl.c
// prints the "Sleep stats:" block only while light_sleep_en holds — so the
// post-window snapshot must still be taken BEFORE disabling automatic light
// sleep, or the counters become unreadable.
constexpr size_t kDumpBufferBytes = 8192;

struct MemCookie {
  char* buf;
  size_t cap;
  size_t len;
  bool truncated;
};

ssize_t memCookieWrite(void* cookie, const char* buf, size_t len) {
  MemCookie* mem = static_cast<MemCookie*>(cookie);
  const size_t room = mem->len < mem->cap ? mem->cap - 1 - mem->len : 0;
  if (len > room) {
    mem->truncated = true;
    len = room;
  }
  memcpy(mem->buf + mem->len, buf, len);
  mem->len += len;
  mem->buf[mem->len] = '\0';
  return static_cast<ssize_t>(len);
}

struct LsCounters {
  long long bootTimeUs = -1;
  long long sleepModeTimeUs = -1;
  unsigned long entries = 0;
  unsigned long rejects = 0;
  bool sleepStatsPrinted = false;
};

// Reads the counters out of one dump. Lines are matched through their section
// headers, never by column position: the lock names are driver-owned.
bool parseLsCounters(const char* text, LsCounters* out) {
  bool inModeStats = false;
  bool inSleepStats = false;
  bool parsedBootTime = false;
  const char* line = text;
  while (line != nullptr && *line != '\0') {
    const char* end = strchr(line, '\n');
    if (strncmp(line, "Time since boot up:", 19) == 0) {
      parsedBootTime = sscanf(line, "Time since boot up: %lld", &out->bootTimeUs) == 1;
    } else if (strncmp(line, "Mode stats:", 11) == 0) {
      inModeStats = true;
      inSleepStats = false;
    } else if (strncmp(line, "Sleep stats:", 12) == 0) {
      inModeStats = false;
      inSleepStats = true;
      out->sleepStatsPrinted = true;
    } else if (inModeStats && strncmp(line, "SLEEP", 5) == 0) {
      // The PM_MODE_LIGHT_SLEEP row: "SLEEP  <freq>M<pad> <time_us> <pct>%";
      // %-3u pads two-digit frequencies ("80 M"), so the space directive
      // before the literal M must tolerate zero or more blanks.
      if (sscanf(line, "SLEEP %*u M %lld", &out->sleepModeTimeUs) != 1) {
        out->sleepModeTimeUs = -1;
      }
    } else if (inSleepStats && strncmp(line, "light_sleep_counts:", 19) == 0) {
      if (sscanf(line, "light_sleep_counts:%lu light_sleep_reject_counts:%lu", &out->entries, &out->rejects) != 2) {
        out->sleepStatsPrinted = false;
      }
    }
    line = end != nullptr ? end + 1 : nullptr;
  }
  return parsedBootTime;
}

// The experiment verdict, decided from measured values only.
enum class LsVerdict : uint8_t { Pass, Fail, Inconclusive };

const char* lsVerdictName(LsVerdict verdict) {
  switch (verdict) {
    case LsVerdict::Pass:
      return "PASS";
    case LsVerdict::Fail:
      return "FAIL";
    case LsVerdict::Inconclusive:
      return "INCONCLUSIVE";
  }
  return "INCONCLUSIVE";
}

// Dumps into the given buffer and NUL-terminates it.
bool captureLsDump(char* buf, size_t cap, MemCookie* cookie, FILE* memFile) {
  cookie->buf = buf;
  cookie->cap = cap;
  cookie->len = 0;
  cookie->truncated = false;
  clearerr(memFile);
  const esp_err_t err = esp_pm_dump_locks(memFile);
  fflush(memFile);
  if (err != ESP_OK) {
    LOG_ERR(TAG, "LSTEST: esp_pm_dump_locks failed: %s", esp_err_to_name(err));
    return false;
  }
  return true;
}

bool writeChunk(HalFile& file, const char* text, bool& writeError) {
  const size_t len = strlen(text);
  if (!writeError && file.write(text, len) != len) {
    writeError = true;
  }
  return !writeError;
}

// Restores the boot-time PM configuration; a failed restore cannot be left
// standing — the controlled answer is a reboot into the safe boot path.
void restoreAutoLightSleepOff(esp_err_t* errOut) {
  esp_pm_config_t off = {};
  off.max_freq_mhz = 240;
  off.min_freq_mhz = CROSSPOINT_PM_MIN_FREQ;
  off.light_sleep_enable = false;
  *errOut = esp_pm_configure(&off);
  if (*errOut != ESP_OK) {
    LOG_ERR(TAG, "LSTEST: restoring automatic light sleep OFF failed: %s — rebooting", esp_err_to_name(*errOut));
    delay(2000);
    esp_restart();
  }
}

// p1-b1q: per-task runtime attribution around the controlled window. FreeRTOS
// run-time stats are already compiled in (sdkconfig.x4c-pm80ls sets
// GENERATE_RUN_TIME_STATS with the esp_timer source), and the kernel
// accumulates wall time to each core's current task at switch-out in
// vTaskSwitchContext — ISR time lands on the interrupted task and
// light-sleep time on the sleeping cores' current tasks. Both snapshot calls
// run strictly outside the window.
constexpr UBaseType_t kTaskSnapshotMax = 20;
struct TaskSample {
  uint32_t runtime;
  uint32_t number;
  int32_t core;
  uint32_t priority;
  char name[16];
};
static TaskSample taskSamplesA[kTaskSnapshotMax];
static TaskSample taskSamplesB[kTaskSnapshotMax];
static UBaseType_t sTaskCountA = 0;
static UBaseType_t sTaskCountB = 0;

bool takeTaskSample(TaskSample* out, UBaseType_t* countOut) {
  static TaskStatus_t raw[kTaskSnapshotMax];
  *countOut = 0;
  if (uxTaskGetNumberOfTasks() > kTaskSnapshotMax) {
    return false;
  }
  configRUN_TIME_COUNTER_TYPE total = 0;
  const UBaseType_t filled = uxTaskGetSystemState(raw, kTaskSnapshotMax, &total);
  if (filled == 0) {
    return false;
  }
  for (UBaseType_t i = 0; i < filled; ++i) {
    out[i].runtime = raw[i].ulRunTimeCounter;
    out[i].number = raw[i].xTaskNumber;
    out[i].core = raw[i].xCoreID;
    out[i].priority = raw[i].uxCurrentPriority;
    strncpy(out[i].name, raw[i].pcTaskName != nullptr ? raw[i].pcTaskName : "?", sizeof(out[i].name) - 1);
    out[i].name[sizeof(out[i].name) - 1] = '\0';
  }
  *countOut = filled;
  return true;
}

// Per-task wall-time delta in the window, matched by task number and name.
// A runtime counter that wrapped between the snapshots (B < A) is flagged
// and excluded from sums rather than summed as a huge positive delta.
struct TaskDelta {
  const TaskSample* sample;
  uint32_t delta;
  bool wrapped;
  bool created;
};

void computeTaskDeltas(const TaskSample* a, UBaseType_t aCount, const TaskSample* b, UBaseType_t bCount,
                       TaskDelta* deltas, UBaseType_t* deltaCount, const TaskSample** deleted,
                       UBaseType_t* deletedCount) {
  UBaseType_t n = 0;
  UBaseType_t d = 0;
  for (UBaseType_t i = 0; i < bCount; ++i) {
    const TaskSample* match = nullptr;
    for (UBaseType_t j = 0; j < aCount; ++j) {
      if (a[j].number == b[i].number) {
        match = &a[j];
        break;
      }
    }
    if (match == nullptr || strcmp(match->name, b[i].name) != 0) {
      deltas[n++] = TaskDelta{&b[i], b[i].runtime, false, true};
      continue;
    }
    deltas[n++] = TaskDelta{&b[i], b[i].runtime - match->runtime, b[i].runtime < match->runtime, false};
  }
  *deltaCount = n;
  for (UBaseType_t j = 0; j < aCount; ++j) {
    bool stillThere = false;
    for (UBaseType_t i = 0; i < bCount; ++i) {
      if (b[i].number == a[j].number && strcmp(b[i].name, a[j].name) == 0) {
        stillThere = true;
        break;
      }
    }
    if (!stillThere) {
      deleted[d++] = &a[j];
    }
  }
  *deletedCount = d;
}

// Insertion sort by delta, descending; n is small (≤20).
void sortTaskDeltas(TaskDelta* deltas, UBaseType_t n) {
  for (UBaseType_t i = 1; i < n; ++i) {
    const TaskDelta key = deltas[i];
    UBaseType_t j = i;
    while (j > 0 && deltas[j - 1].delta < key.delta) {
      deltas[j] = deltas[j - 1];
      --j;
    }
    deltas[j] = key;
  }
}

void writeTaskRuntimeSection(HalFile& file, bool& writeError, int64_t tAUs, int64_t tBUs, bool sampledOk) {
  char line[192];
  writeChunk(file, "\n--- task runtime (p1-b1q) ---\n", writeError);
  snprintf(line, sizeof(line), "clock: esp_timer us (CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER)\n");
  writeChunk(file, line, writeError);
  snprintf(line, sizeof(line), "task window: %lld us (measured between the two snapshots)\n",
           static_cast<long long>(tBUs - tAUs));
  writeChunk(file, line, writeError);
  if (!sampledOk) {
    writeChunk(file, "task snapshot skipped: task count exceeds the fixed buffer or the call failed\n", writeError);
    return;
  }
  UBaseType_t aCount = 0;
  UBaseType_t bCount = 0;
  TaskDelta deltas[kTaskSnapshotMax];
  const TaskSample* deleted[kTaskSnapshotMax];
  UBaseType_t deltaCount = 0;
  UBaseType_t deletedCount = 0;
  computeTaskDeltas(taskSamplesA, sTaskCountA, taskSamplesB, sTaskCountB, deltas, &deltaCount, deleted, &deletedCount);
  sortTaskDeltas(deltas, deltaCount);
  writeChunk(file, "name            num core prio   delta_us\n", writeError);
  uint64_t coreSum[2] = {0, 0};
  uint64_t unpinnedSum = 0;
  uint64_t idleDelta[2] = {0, 0};
  bool idleSeen[2] = {false, false};
  for (UBaseType_t i = 0; i < deltaCount; ++i) {
    const TaskSample& t = *deltas[i].sample;
    if (deltas[i].wrapped) {
      snprintf(line, sizeof(line), "%-15s %3lu  %2ld  %3lu  (runtime counter wrap suspected -- excluded)\n", t.name,
               static_cast<unsigned long>(t.number), static_cast<long>(t.core), static_cast<unsigned long>(t.priority));
    } else if (deltas[i].created) {
      snprintf(line, sizeof(line), "%-15s %3lu  %2ld  %3lu  created in window (runtime by B: %lu us)\n", t.name,
               static_cast<unsigned long>(t.number), static_cast<long>(t.core), static_cast<unsigned long>(t.priority),
               static_cast<unsigned long>(t.runtime));
    } else {
      snprintf(line, sizeof(line), "%-15s %3lu  %2ld  %3lu  %lu\n", t.name, static_cast<unsigned long>(t.number),
               static_cast<long>(t.core), static_cast<unsigned long>(t.priority),
               static_cast<unsigned long>(deltas[i].delta));
    }
    writeChunk(file, line, writeError);
    if (deltas[i].wrapped || deltas[i].created) {
      continue;
    }
    if (t.core == 0 || t.core == 1) {
      coreSum[t.core] += deltas[i].delta;
      if (strcmp(t.name, t.core == 0 ? "IDLE0" : "IDLE1") == 0) {
        idleDelta[t.core] = deltas[i].delta;
        idleSeen[t.core] = true;
      }
    } else {
      unpinnedSum += deltas[i].delta;
    }
  }
  for (UBaseType_t j = 0; j < deletedCount; ++j) {
    snprintf(line, sizeof(line), "%-15s %3lu  %2ld  %3lu  deleted in window (runtime at A: %lu us)\n", deleted[j]->name,
             static_cast<unsigned long>(deleted[j]->number), static_cast<long>(deleted[j]->core),
             static_cast<unsigned long>(deleted[j]->priority), static_cast<unsigned long>(deleted[j]->runtime));
    writeChunk(file, line, writeError);
  }
  snprintf(line, sizeof(line), "core0 pinned sum: %llu us   core1 pinned sum: %llu us   unpinned sum: %llu us\n",
           static_cast<unsigned long long>(coreSum[0]), static_cast<unsigned long long>(coreSum[1]),
           static_cast<unsigned long long>(unpinnedSum));
  writeChunk(file, line, writeError);
  const long long wall = static_cast<long long>(tBUs - tAUs);
  snprintf(line, sizeof(line), "IDLE0 delta: %llu us   IDLE1 delta: %llu us\n",
           static_cast<unsigned long long>(idleSeen[0] ? idleDelta[0] : 0),
           static_cast<unsigned long long>(idleSeen[1] ? idleDelta[1] : 0));
  writeChunk(file, line, writeError);
  const long long core0NonIdle = static_cast<long long>(coreSum[0]) - static_cast<long long>(idleDelta[0]);
  snprintf(line, sizeof(line), "core0 non-idle task time: %lld us of %lld us wall\n", core0NonIdle, wall);
  writeChunk(file, line, writeError);
  snprintf(line, sizeof(line),
           "note: FreeRTOS runtime stats attribute ISR time to the interrupted task and only count\n");
  writeChunk(file, line, writeError);
  snprintf(line, sizeof(line),
           "completed context switches; a small core0 non-idle sum means either interrupt-driven\n");
  writeChunk(file, line, writeError);
  snprintf(line, sizeof(line), "activity or a single task that never switched out during the window.\n");
  writeChunk(file, line, writeError);
}

}  // namespace

void handleLsTest(bool usbExclusiveStorage) {
  // Precondition: the SD card must be owned by the normal filesystem. A USB
  // Drive session detaches it, and while its session lock is held automatic
  // light sleep could not engage, so the window would be void.
  if (usbExclusiveStorage) {
    logSerial.printf("LSTEST ERROR: USB Drive owns the storage\n");
    return;
  }
  if (!Storage.ready()) {
    logSerial.printf("LSTEST ERROR: storage not ready\n");
    return;
  }

  logSerial.printf("LSTEST OK: window starting (5000 ms)\n");

  // Boot-state snapshot: automatic light sleep is off, so this dump carries no
  // "Sleep stats" block (esp_pm prints it only while light_sleep_en holds).
  // The text is captured straight into the report copy — no second buffer.
  static char bootDumpCopy[kDumpBufferBytes];
  static char windowDump[kDumpBufferBytes];
  static MemCookie memCookie;
  static FILE* memFile = nullptr;
  if (memFile == nullptr) {
    static const cookie_io_functions_t io = {nullptr, memCookieWrite, nullptr, nullptr};
    memFile = fopencookie(&memCookie, "w", io);
  }
  if (memFile == nullptr) {
    LOG_ERR(TAG, "LSTEST: could not create the capture stream");
    return;
  }

  LsCounters bootStats;
  const bool bootCaptured = captureLsDump(bootDumpCopy, sizeof(bootDumpCopy), &memCookie, memFile) &&
                            parseLsCounters(bootDumpCopy, &bootStats);
  if (!bootCaptured) {
    LOG_ERR(TAG, "LSTEST: boot-state PM snapshot failed, refusing to run");
    return;
  }

  // The window runs on this (the Arduino loop) task: delay() parks it on a
  // FreeRTOS deadline the tickless idle schedules the light-sleep wake on
  // (esp_sleep_enable_timer_wakeup inside vApplicationSleep), so the
  // experiment completes without the USB console. Deep sleep cannot intercept
  // it: every enterDeepSleep/enterHardwareDeepSleep call site executes in
  // loop() only after this handler returns, and no NO_LIGHT_SLEEP lock is
  // involved — the loop task itself is what keeps the device awake.
  const int64_t windowStartUs = esp_timer_get_time();
  esp_pm_config_t on = {};
  on.max_freq_mhz = 240;
  on.min_freq_mhz = CROSSPOINT_PM_MIN_FREQ;
  on.light_sleep_enable = true;
  const esp_err_t enableErr = esp_pm_configure(&on);
  if (enableErr != ESP_OK) {
    LOG_ERR(TAG, "LSTEST: enabling automatic light sleep failed: %s", esp_err_to_name(enableErr));
    logSerial.printf("LSTEST ERROR: esp_pm_configure(ON) failed: %s\n", esp_err_to_name(enableErr));
    return;
  }

  LsCounters beforeStats;
  const bool beforeCaptured =
      captureLsDump(windowDump, sizeof(windowDump), &memCookie, memFile) && parseLsCounters(windowDump, &beforeStats);
  if (!beforeCaptured) {
    LOG_ERR(TAG, "LSTEST: pre-window PM snapshot failed, aborting the window");
    esp_err_t restoreErr = ESP_OK;
    restoreAutoLightSleepOff(&restoreErr);
    logSerial.printf("LSTEST ERROR: pre-window snapshot failed (restore %s)\n", esp_err_to_name(restoreErr));
    return;
  }

  // p1-b1q: task runtime snapshot A, strictly outside the window.
  const int64_t taskWindowStartUs = esp_timer_get_time();
  bool taskSampledOk = takeTaskSample(taskSamplesA, &sTaskCountA);
  if (!taskSampledOk) {
    LOG_INF(TAG, "LSTEST: task snapshot unavailable (count/buffer) — the section will say so");
  }

  // The unattended window: no serial writes, no SD I/O, no busy loop.
  delay(5000);

  // p1-b1q: task runtime snapshot B, still before any I/O.
  taskSampledOk = takeTaskSample(taskSamplesB, &sTaskCountB) && taskSampledOk;
  const int64_t taskWindowEndUs = esp_timer_get_time();

  LsCounters afterStats;
  const bool afterCaptured =
      captureLsDump(windowDump, sizeof(windowDump), &memCookie, memFile) && parseLsCounters(windowDump, &afterStats);

  const int64_t windowEndUs = esp_timer_get_time();
  esp_err_t restoreErr = ESP_OK;
  restoreAutoLightSleepOff(&restoreErr);
  const int64_t windowUs = windowEndUs - windowStartUs;

  LOG_INF(TAG, "LSTEST: window done (%lld ms), writing the report", static_cast<long long>(windowUs / 1000));

  const bool countersValid = beforeCaptured && afterCaptured && beforeStats.sleepModeTimeUs >= 0 &&
                             afterStats.sleepModeTimeUs >= 0 && beforeStats.sleepStatsPrinted &&
                             afterStats.sleepStatsPrinted;
  const long long deltaEntries =
      countersValid ? static_cast<long long>(afterStats.entries) - static_cast<long long>(beforeStats.entries) : -1;
  const long long deltaRejects =
      countersValid ? static_cast<long long>(afterStats.rejects) - static_cast<long long>(beforeStats.rejects) : -1;
  const long long deltaSleepUs = countersValid ? afterStats.sleepModeTimeUs - beforeStats.sleepModeTimeUs : -1;

  LsVerdict verdict = LsVerdict::Inconclusive;
  const char* reason = "";
  if (!countersValid) {
    reason = "PM snapshot capture or parse failed";
  } else if (deltaEntries > 0 && deltaSleepUs > 0) {
    verdict = LsVerdict::Pass;
    reason = "automatic light sleep engaged in the window";
  } else if (deltaEntries == 0 && deltaRejects == 0) {
    reason = "no light-sleep entry in the window (a lock may have held the PM mode)";
  } else if (deltaEntries == 0 && deltaRejects > 0) {
    reason = "light-sleep entry attempts were rejected in the window";
  } else {
    reason = "measured counters inconsistent";
  }

  // All SD I/O happens here, after automatic light sleep is off again: no
  // SDMMC work can race the sleep windows, and a USB Drive session cannot own
  // the card (the precondition checked above still holds — loop() was blocked).
  Storage.ensureDirectoryExists("/diagnostics");
  HalFile file;
  bool writeError = !Storage.openFileForWrite("LST", "/diagnostics/light-sleep-test.txt", file);
  if (writeError) {
    LOG_ERR(TAG, "LSTEST: could not open the report file — result available in this log only");
  } else {
    char line[192];
    snprintf(line, sizeof(line), "CrossPoint controlled light-sleep test\n");
    writeChunk(file, line, writeError);
    snprintf(line, sizeof(line), "firmware: " CROSSPOINT_VERSION "  env: x4c-lstest\n");
    writeChunk(file, line, writeError);
    snprintf(line, sizeof(line), "interposition: esp_pm_impl_isr_hook pass-through\n");
    writeChunk(file, line, writeError);
    snprintf(line, sizeof(line), "window requested: 5000 ms   window actual: %lld ms\n",
             static_cast<long long>(windowUs / 1000));
    writeChunk(file, line, writeError);
    snprintf(line, sizeof(line),
             "pm config: boot 240/%d autols OFF -> window 240/%d autols ON -> restored 240/%d autols OFF\n",
             CROSSPOINT_PM_MIN_FREQ, CROSSPOINT_PM_MIN_FREQ, CROSSPOINT_PM_MIN_FREQ);
    writeChunk(file, line, writeError);
    snprintf(line, sizeof(line), "configure(autols ON): %s   configure(autols OFF): %s\n", esp_err_to_name(enableErr),
             esp_err_to_name(restoreErr));
    writeChunk(file, line, writeError);
    if (countersValid) {
      snprintf(line, sizeof(line), "light_sleep entries: before=%lu after=%lu delta=%lld\n", beforeStats.entries,
               afterStats.entries, deltaEntries);
      writeChunk(file, line, writeError);
      snprintf(line, sizeof(line), "rejected entries: before=%lu after=%lu delta=%lld\n", beforeStats.rejects,
               afterStats.rejects, deltaRejects);
      writeChunk(file, line, writeError);
      snprintf(line, sizeof(line), "light sleep time us: before=%lld after=%lld delta=%lld\n",
               beforeStats.sleepModeTimeUs, afterStats.sleepModeTimeUs, deltaSleepUs);
      writeChunk(file, line, writeError);
    }
    snprintf(line, sizeof(line), "verdict: %s - %s\n", lsVerdictName(verdict), reason);
    writeChunk(file, line, writeError);
    writeChunk(file, "\n--- snapshot: boot state (autols OFF) ---\n", writeError);
    writeChunk(file, bootDumpCopy, writeError);
    if (afterCaptured) {
      writeChunk(file, "\n--- snapshot: post-window (before restore) ---\n", writeError);
      writeChunk(file, windowDump, writeError);
    }
    writeTaskRuntimeSection(file, writeError, taskWindowStartUs, taskWindowEndUs, taskSampledOk);
    if (memCookie.truncated) {
      writeChunk(file, "\n(warning: a dump exceeded the capture buffer and was truncated)\n", writeError);
    }
    if (writeError) {
      LOG_ERR(TAG, "LSTEST: the report write failed midway — see this log");
    }
    file.close();
  }
  // p1-b1q stack check (review ask): the report path added a ~0.6 KB frame on
  // the loop task; the high-water mark after the run bounds it on hardware.
  LOG_INF(TAG, "LSTEST: loop-task stack high-water mark: %u bytes free",
          static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t)));

  if (countersValid) {
    LOG_INF(TAG, "LSTEST: %s — %s (entries delta=%lld, sleep us delta=%lld, rejects delta=%lld)",
            lsVerdictName(verdict), reason, deltaEntries, deltaSleepUs, deltaRejects);
  } else {
    LOG_INF(TAG, "LSTEST: %s — %s", lsVerdictName(verdict), reason);
  }
  if (verdict == LsVerdict::Pass) {
    logSerial.printf("LSTEST OK: entries delta=%lld, sleep us delta=%lld, rejects delta=%lld\n", deltaEntries,
                     deltaSleepUs, deltaRejects);
  } else {
    logSerial.printf("LSTEST ERROR: %s — %s\n", lsVerdictName(verdict), reason);
  }
}

// The p1-b1o explicit-answer contract: every diagnostic command answers OK or
// ERROR over the serial log, so an operator cannot mistake a dead RX path for
// a rejected command. Called from loop() for every parsed CMD: line — the
// known list must name every command main.cpp's dispatch handles elsewhere
// (SCREENSHOT is handled after this call; GATEARM under its own guard).
void ackUnknownCommand(const char* cmd) {
  static const char* const known[] = {"PMSTAT", "RXSTAT", "LSTEST", "SCREENSHOT", "GATEARM"};
  if (cmd == nullptr) return;
  for (const char* k : known) {
    if (strcmp(cmd, k) == 0) return;
  }
  logSerial.printf("CMD ERROR: unknown '%s'\n", cmd);
}

// Periodic RX-path heartbeat for the UAT log stream: does the ISR-side RX
// counter ever move while TX keeps flowing? RX1's heartbeat ran as the
// third and largest line of the 10 s MEM burst and was silently discarded by
// HWCDC's bounded TX send; main.cpp now gives this call its own 10 s tick
// phase-offset from the MEM tick, so the line normally queues into a drained
// ring. The embedded t= shows when the line was built even if a stalled
// link flushes it late. The first call still carries the full snapshot.
void rxHeartbeat() {
  static bool firstSnapshot = true;
  if (firstSnapshot) {
    firstSnapshot = false;
    logUsjState("RX state at first heartbeat");
  }
  char line[112];
  int len = snprintf(line, sizeof(line), "RX heartbeat t=%lu rxEvents=%lu rxBytes=%lu intRaw=0x%lx outDataAvail=%lu\n",
                     static_cast<unsigned long>(millis()), static_cast<unsigned long>(sRxEventCount),
                     static_cast<unsigned long>(sRxEventBytes), static_cast<unsigned long>(USB_SERIAL_JTAG.int_raw.val),
                     static_cast<unsigned long>((USB_SERIAL_JTAG.ep1_conf.val >> 2) & 1));
  if (len > 0 && len < static_cast<int>(sizeof(line))) {
    writeChecked(line, static_cast<size_t>(len));
  }
}
#endif

}  // namespace pmonlydiag
#endif
