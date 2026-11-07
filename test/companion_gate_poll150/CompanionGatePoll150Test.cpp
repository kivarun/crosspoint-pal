// The P1-B9-E2 poll150 A/B configuration, compiled with the experimental
// environment's exact override: the gate window's automatic-light-sleep poll
// period becomes 150 ms while every other cadence stays untouched.

#include <gtest/gtest.h>

#include "util/CompanionGatePower.h"

// The CTest target compiles with -DCROSSPOINT_COMPANION_GATE_POLL_MS=150;
// tools that scan the source without the target's definitions (cppcheck via
// pio check) must still compile, so nothing here can fail preprocessing.
#ifdef CROSSPOINT_COMPANION_GATE_POLL_MS
static_assert(CROSSPOINT_COMPANION_GATE_POLL_MS == 150, "the poll150 target must carry the 150 ms configuration");
#endif

TEST(GatePoll150, ExperimentalPeriodReachesTheGatePoll) {
#ifdef CROSSPOINT_COMPANION_GATE_POLL_MS
  EXPECT_EQ(CROSSPOINT_COMPANION_GATE_POLL_MS, 150u);
#endif
  EXPECT_EQ(companiongatepower::kAutolsPollMs, 150u);
  EXPECT_EQ(companiongatepower::gatePollMs(true), 150u);
}

TEST(GatePoll150, StockCadenceIsUntouched) {
  EXPECT_EQ(companiongatepower::kStockPollMs, 2u);
  EXPECT_EQ(companiongatepower::gatePollMs(false), 2u);
}
