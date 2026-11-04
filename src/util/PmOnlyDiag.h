#pragma once
#ifdef CROSSPOINT_PM_ONLY_DIAG

// Dev-only PM diagnostic (p1-b1k/b1l/b1n and the p1-b1o harness): the PM
// initialization extracted from blelightsleepdiag::begin() — the
// esp_pm_configure DFS range with automatic light sleep per
// CROSSPOINT_PM_AUTOLS. Compiled only in env:x4c-pmonly, env:x4c-pm80,
// env:x4c-pm80ls and env:x4c-lstest (CROSSPOINT_PM_MIN_FREQ picks the DFS
// floor, CROSSPOINT_PM_AUTOLS the light-sleep switch per env, and
// CROSSPOINT_PM_LSTEST the controlled-window harness); production and the
// other diagnostic envs see none of it.

namespace pmonlydiag {

// Applies esp_pm_configure(240 MHz max / CROSSPOINT_PM_MIN_FREQ min,
// automatic light sleep per CROSSPOINT_PM_AUTOLS). A failed configure makes
// the experiment invalid: logs the error and halts.
void begin();

// Consumes the PMSTAT serial command (esp_pm lock and mode dump; with
// CONFIG_PM_PROFILING the dump carries light-sleep entry counts).
void handleCommand(const char* command);

#ifdef CROSSPOINT_PM_LSTEST
// Runs the p1-b1o controlled automatic-light-sleep window on the caller's
// task: boot-state PM snapshot, esp_pm_configure(240/80, autols ON), a 5 s
// unattended window (the loop task blocked in delay, no SD/serial I/O),
// esp_pm_configure(240/80, autols OFF), delta computation, and one SD report
// write. `usbExclusiveStorage` is the caller's requiresExclusiveStorageLoop()
// state; the test refuses to run while USB Drive owns the SD card.
//
// Contract: call ONLY from loop()'s command dispatch. The app's deep-sleep
// paths are kept out of the window by that synchronous position alone — no
// PM lock takes part — so moving this call off the loop task would let deep
// sleep intercept the window.
void handleLsTest(bool usbExclusiveStorage);

// p1-b1o RX diagnostics: an explicit "CMD ERROR" reply for commands the
// dispatch does not handle, so a dead RX path is distinguishable from a
// rejected command. Called from loop() for every parsed CMD: line.
void ackUnknownCommand(const char* cmd);

// Periodic RX-path heartbeat for the UAT log stream: a short checked line
// (rx counters + the two RX-marker register fields, with the build time
// embedded) emitted on its own 10 s tick in loop(), plus a one-time full
// USJ register snapshot on the first call. Short writes are counted and
// surfaced as txShort in the RXSTAT reply.
void rxHeartbeat();
#endif

}  // namespace pmonlydiag
#endif
