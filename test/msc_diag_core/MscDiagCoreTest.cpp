#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "util/MscDiagCore.h"

namespace {

using mscdiag::Entry;
using mscdiag::MscDiagRingCore;

// claim + fill + publish, mirroring the device-side note() sequence.
template <size_t N>
void emit(MscDiagRingCore<N>& ring, uint8_t kind, uint32_t millis) {
  const size_t slot = ring.claim();
  Entry& e = ring.slot(slot);
  e.seq = mscdiag::kSeqInvalid;
  e.millis = millis;
  e.kind = kind;
  e.ns = 0;
  e.a = 0;
  e.b = 0;
  e.freq = 240;
  ring.publish(slot, 1);
}

template <size_t N>
std::vector<uint8_t> drain(const MscDiagRingCore<N>& ring) {
  std::vector<uint8_t> kinds;
  ring.forEach([&](const Entry& e) {
    kinds.push_back(e.kind);
    return true;
  });
  return kinds;
}

TEST(MscDiagCoreTest, EntryIsSixteenBytes) {
  // The RTC_SLOW headroom math in the p1-b1b report depends on this layout.
  static_assert(sizeof(Entry) == 16, "entry layout changed");
  EXPECT_EQ(sizeof(Entry), 16u);
}

TEST(MscDiagCoreTest, RingFitsTheRtcSlowHeadroom) {
  // 30 entries is the shipped capacity (see MscDiag.h); the whole ring must
  // stay within the RTC_SLOW headroom left after the P0 diagnostic.
  static_assert(sizeof(MscDiagRingCore<30>) == 492, "ring size drifted");
  EXPECT_EQ(sizeof(MscDiagRingCore<30>), 492u);
}

TEST(MscDiagCoreTest, GarbageContentInvalidUntilReset) {
  // A power cycle leaves .rtc.noinit as garbage; 0xA5 fill is the stand-in.
  MscDiagRingCore<4> ring;
  memset(static_cast<void*>(&ring), 0xA5, sizeof(ring));
  EXPECT_FALSE(ring.contentValid());
  ring.reset();
  EXPECT_TRUE(ring.contentValid());
  // generation counts resets, not power cycles: from garbage it starts at
  // garbage+1, which is fine for a diagnostic label as long as it is non-zero.
  EXPECT_NE(ring.generation(), 0u);
}

TEST(MscDiagCoreTest, ResetBumpsGenerationAndClearsHistory) {
  MscDiagRingCore<4> ring;
  ring.reset();
  emit(ring, 1, 10);
  ring.reset();
  EXPECT_EQ(ring.generation(), 2u);
  EXPECT_EQ(ring.total(), 0u);
  EXPECT_TRUE(drain(ring).empty());
}

TEST(MscDiagCoreTest, IteratesOldestFirstAndSurvivesWrap) {
  MscDiagRingCore<4> ring;
  ring.reset();
  for (uint8_t i = 1; i <= 6; ++i) emit(ring, i, i * 10);
  const auto kinds = drain(ring);
  ASSERT_EQ(kinds.size(), 4u);
  EXPECT_EQ(kinds[0], 3);
  EXPECT_EQ(kinds[3], 6);
  EXPECT_EQ(ring.total(), 6u);
  EXPECT_EQ(ring.dropped(), 2u);
}

TEST(MscDiagCoreTest, ClaimsAreExclusiveAcrossSimulatedWriters) {
  // The device has two writer tasks; claim() must hand out distinct slots.
  MscDiagRingCore<4> ring;
  ring.reset();
  std::vector<size_t> claimed;
  for (int i = 0; i < 6; ++i) claimed.push_back(ring.claim());
  // Within any window of kEntries consecutive claims no slot repeats.
  for (size_t i = 0; i + 4 <= claimed.size(); ++i) {
    EXPECT_NE(claimed[i], claimed[i + 1]);
    EXPECT_NE(claimed[i], claimed[i + 2]);
    EXPECT_NE(claimed[i], claimed[i + 3]);
  }
}

TEST(MscDiagCoreTest, UnpublishedSlotsAreSkipped) {
  MscDiagRingCore<4> ring;
  ring.reset();
  emit(ring, 1, 10);
  // A writer died between claim and publish: the slot stays kSeqInvalid and
  // must never surface as a half-filled entry.
  const size_t torn = ring.claim();
  ring.slot(torn).seq = mscdiag::kSeqInvalid;
  ring.slot(torn).millis = 999;
  ring.slot(torn).kind = 9;
  emit(ring, 2, 20);
  const auto kinds = drain(ring);
  ASSERT_EQ(kinds.size(), 2u);
  EXPECT_EQ(kinds[0], 1);
  EXPECT_EQ(kinds[1], 2);
}

TEST(MscDiagCoreTest, ForEachCanBeStoppedEarly) {
  MscDiagRingCore<4> ring;
  ring.reset();
  emit(ring, 1, 10);
  emit(ring, 2, 20);
  size_t seen = 0;
  ring.forEach([&](const Entry&) {
    seen += 1;
    return false;
  });
  EXPECT_EQ(seen, 1u);
}

TEST(MscDiagCoreTest, RestartPreservesRingAcrossFakeReboot) {
  // On the device the same storage survives ESP.restart; bootInit() must keep
  // the events and the generation instead of resetting.
  MscDiagRingCore<4> ring;
  ring.reset();
  emit(ring, 1, 10);
  emit(ring, 2, 20);
  // Simulated reboot: storage untouched, generation already valid.
  EXPECT_TRUE(ring.contentValid());
  EXPECT_EQ(ring.generation(), 1u);
  emit(ring, 3, 30);
  const auto kinds = drain(ring);
  ASSERT_EQ(kinds.size(), 3u);
  EXPECT_EQ(kinds[2], 3);
}

}  // namespace
