#pragma once

#include <BoardConfig.h>

namespace slideshow {

// ONE board capability for the sleep slideshow: the physical prerequisite is
// that battery power stays on through deep sleep, so the ESP timer can wake
// the MCU. Proven on X4 Classic (power.latch0 = the GPIO1 master rail, held
// HIGH through sleep; battery timer-wake UAT passed). Conservative elsewhere
// until the same power topology is validated on hardware: X4 Pro shares the
// latch topology but its timer wake is unproven; the C3 X4/X3 sleep path cuts
// the battery MOSFET (GPIO13); Paper Mono's button sits behind its PMIC.
//
// The capability gates ONLY the fork's timer-driven Slideshow mode. Upstream's
// static sleep screens (including Cover + Transparent) are board-independent
// and stay advertised on every board: the sleep-screen advertisement omits
// exactly this fork's SLIDESHOW value elsewhere (SlideshowPolicy
// advertisement seam) and never hides an upstream capability.
inline bool sleepSlideshowSupported() { return BoardConfig::isX4Classic(); }

}  // namespace slideshow
