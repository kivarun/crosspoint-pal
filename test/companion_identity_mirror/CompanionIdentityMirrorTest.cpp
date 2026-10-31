#include <gtest/gtest.h>

#include "util/CompanionIdentityMirror.h"

// The identity mirror's pure decision core (p1-companion-gate preflight §2 Q8,
// rev. 3): canonical-text validation, mirror-if-absent migration, and the
// mirror ∩ bond access predicate. The Preferences I/O lives in the .cpp and
// is device-side only.

namespace {

constexpr const char* kAddr = "aa:bb:cc:dd:ee:ff";

}  // namespace

TEST(IdentityText, AcceptsCanonicalForms) {
  EXPECT_TRUE(companionidentity::textValid(kAddr, blecompanion::ADDR_TYPE_PUBLIC));
  EXPECT_TRUE(companionidentity::textValid(kAddr, blecompanion::ADDR_TYPE_RANDOM));
  EXPECT_TRUE(companionidentity::textValid("00:11:22:33:44:55", blecompanion::ADDR_TYPE_RANDOM));
}

TEST(IdentityText, RejectsMalformedText) {
  EXPECT_FALSE(companionidentity::textValid(nullptr, blecompanion::ADDR_TYPE_PUBLIC));
  EXPECT_FALSE(companionidentity::textValid("", blecompanion::ADDR_TYPE_PUBLIC));
  EXPECT_FALSE(companionidentity::textValid("aa:bb:cc:dd:ee", blecompanion::ADDR_TYPE_PUBLIC));      // too short
  EXPECT_FALSE(companionidentity::textValid("aa:bb:cc:dd:ee:ff:00", blecompanion::ADDR_TYPE_PUBLIC)); // too long
  EXPECT_FALSE(companionidentity::textValid("aabbc:cc:dd:ee:ff", blecompanion::ADDR_TYPE_PUBLIC));    // colon drift
  EXPECT_FALSE(companionidentity::textValid("aa:bb:cc:dd:ee:fg", blecompanion::ADDR_TYPE_PUBLIC));    // not hex
  EXPECT_FALSE(companionidentity::textValid("AA:BB:CC:DD:EE:FF", blecompanion::ADDR_TYPE_PUBLIC));    // not lowercase
  EXPECT_FALSE(companionidentity::textValid(kAddr, 2));                                             // unknown type
  EXPECT_FALSE(companionidentity::textValid(kAddr, 0xFF));                                          // unset type
}

TEST(IdentityMigration, MirrorIfAbsent) {
  using companionidentity::Migration;
  // A valid mirror is never overwritten, whatever SETTINGS claims.
  EXPECT_EQ(companionidentity::migrationDecision(true, kAddr, blecompanion::ADDR_TYPE_PUBLIC), Migration::None);
  EXPECT_EQ(companionidentity::migrationDecision(true, "11:22:33:44:55:66", blecompanion::ADDR_TYPE_RANDOM),
            Migration::None);
  // Absent mirror + valid SETTINGS identity → migrate.
  EXPECT_EQ(companionidentity::migrationDecision(false, kAddr, blecompanion::ADDR_TYPE_PUBLIC),
            Migration::WriteMirrorFromSettings);
  // Absent mirror + no usable SETTINGS identity → nothing to migrate.
  EXPECT_EQ(companionidentity::migrationDecision(false, "", 0xFF), Migration::None);
  EXPECT_EQ(companionidentity::migrationDecision(false, "garbage", blecompanion::ADDR_TYPE_PUBLIC), Migration::None);
  EXPECT_EQ(companionidentity::migrationDecision(false, kAddr, 0xFF), Migration::None);
}

TEST(IdentityAccess, RequiresMirrorAndBondAgreement) {
  // The mirror ∩ bond predicate: background access needs both halves.
  EXPECT_TRUE(companionidentity::gateMayProceed(true, true));
  EXPECT_FALSE(companionidentity::gateMayProceed(false, true));
  EXPECT_FALSE(companionidentity::gateMayProceed(true, false));
  EXPECT_FALSE(companionidentity::gateMayProceed(false, false));
}
