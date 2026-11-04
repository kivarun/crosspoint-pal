#pragma once
#ifdef CROSSPOINT_RTC_CLK_DIAG

#include <HalGPIO.h>
#include <HalPowerManager.h>

// Dev-only RTC slow-clock diagnostic (p0-rtc-clock). Compiled only in
// env:x4c-rtcclk; production builds see none of it. State rides in RTC_NOINIT
// memory, so a selected clock and an armed measurement loop survive deep
// sleep but vanish on a power cycle. If a production feature ever needs
// slow-clock control, it belongs behind the HAL/SDK boundary — this module
// then shrinks to a caller of it.
namespace rtcclockdiag {

// Applies the scratch-selected RTC slow clock and, when a wake-loop
// measurement is armed, logs the completed cycle and sleeps again (that path
// never returns). Always returns on the ordinary-boot paths.
void begin(HalPowerManager& power, HalGPIO& gpio, HalGPIO::WakeupReason wake);

// Consumes RTCCLK / RTCLOOP / RTCCAL serial commands.
void handleCommand(HalPowerManager& power, HalGPIO& gpio, const char* command);

// Dumps the RTC-retained diagnostic log once per boot when the USB console
// is connected, so early-boot events are not lost while the host reconnects.
void flushBufferedLog();

}  // namespace rtcclockdiag
#endif
