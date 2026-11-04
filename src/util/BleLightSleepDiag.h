#pragma once
#ifdef CROSSPOINT_BLE_LIGHT_SLEEP_DIAG

// Dev-only BLE light-sleep diagnostic (P1-B0a). Compiled only in the
// x4c-blelp diagnostic envs (rebuilt core); production builds see none of it.

// 1 (default): esp_pm automatic light sleep enabled at configure time.
// 0 (env:x4c-blelp-nols): identical PM configuration with automatic light
// sleep off — the control build for isolating USB-MSC from sleep windows.
#ifndef CROSSPOINT_BLE_LIGHT_SLEEP_AUTOLS
#define CROSSPOINT_BLE_LIGHT_SLEEP_AUTOLS 1
#endif

// DFS floor of the boot-time configure. 40 MHz is the B0a diagnostic recipe;
// the p1-b1l finding (40 MHz XTAL floor breaks USB-MSC) is why experimental
// envs that keep USB-MSC raise it to 80.
#ifndef CROSSPOINT_BLE_LIGHT_SLEEP_MIN_FREQ
#define CROSSPOINT_BLE_LIGHT_SLEEP_MIN_FREQ 40
#endif

namespace blelightsleepdiag {

// Logs the effective controller sleep clock the rebuilt core baked in, the
// boot slow-clock source confirming the sdkconfig-selected external 32k
// crystal, and applies esp_pm configure with automatic light sleep per
// CROSSPOINT_BLE_LIGHT_SLEEP_AUTOLS.
void begin();

// Consumes the PMSTAT serial command (esp_pm lock dump).
void handleCommand(const char* command);

}  // namespace blelightsleepdiag
#endif
