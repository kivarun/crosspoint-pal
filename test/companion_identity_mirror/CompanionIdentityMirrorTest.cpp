#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "util/CompanionIdentityMirror.h"

// The identity mirror's pure decision core (p1-companion-gate preflight §2 Q8,
// rev. 3): canonical-text validation, mirror-if-absent migration, and the
// mirror ∩ bond access predicate. The Preferences I/O lives in the .cpp and
// is device-side only; the readback acceptance it routes through is pinned
// against the production source below.

namespace fs = std::filesystem;

namespace {

constexpr const char* kAddr = "aa:bb:cc:dd:ee:ff";

std::string readAll(const fs::path& path) {
  std::ifstream in(path);
  std::string all;
  std::string line;
  while (std::getline(in, line)) all += line + "\n";
  return all;
}

}  // namespace

TEST(IdentityText, AcceptsCanonicalForms) {
  EXPECT_TRUE(companionidentity::textValid(kAddr, blecompanion::ADDR_TYPE_PUBLIC));
  EXPECT_TRUE(companionidentity::textValid(kAddr, blecompanion::ADDR_TYPE_RANDOM));
  EXPECT_TRUE(companionidentity::textValid("00:11:22:33:44:55", blecompanion::ADDR_TYPE_RANDOM));
}

TEST(IdentityText, RejectsMalformedText) {
  EXPECT_FALSE(companionidentity::textValid(nullptr, blecompanion::ADDR_TYPE_PUBLIC));
  EXPECT_FALSE(companionidentity::textValid("", blecompanion::ADDR_TYPE_PUBLIC));
  EXPECT_FALSE(companionidentity::textValid("aa:bb:cc:dd:ee", blecompanion::ADDR_TYPE_PUBLIC));        // too short
  EXPECT_FALSE(companionidentity::textValid("aa:bb:cc:dd:ee:ff:00", blecompanion::ADDR_TYPE_PUBLIC));  // too long
  EXPECT_FALSE(companionidentity::textValid("aabbc:cc:dd:ee:ff", blecompanion::ADDR_TYPE_PUBLIC));     // colon drift
  EXPECT_FALSE(companionidentity::textValid("aa:bb:cc:dd:ee:fg", blecompanion::ADDR_TYPE_PUBLIC));     // not hex
  EXPECT_FALSE(companionidentity::textValid("AA:BB:CC:DD:EE:FF", blecompanion::ADDR_TYPE_PUBLIC));     // not lowercase
  EXPECT_FALSE(companionidentity::textValid(kAddr, 2));                                                // unknown type
  EXPECT_FALSE(companionidentity::textValid(kAddr, 0xFF));                                             // unset type
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

// The save → load round-trip, reproduced with the exact buffers the device
// produces. save() writes the 17-character address; getString(key, char*,
// maxLen) reports the stored length with or without the terminator depending
// on the core version, so both reported lengths must load the same record.

namespace {

companionidentity::StoredIdentity saveOutputBuffer() {
  // What save() + getString hand to the load core: a zero-initialized buffer
  // holding the 17-character address with the NUL written by nvs_get_str.
  companionidentity::StoredIdentity out{};
  memcpy(out.addr, kAddr, 17);  // the NUL stays at out.addr[17]
  out.type = blecompanion::ADDR_TYPE_RANDOM;
  return out;
}

}  // namespace

TEST(IdentityReadback, AcceptsSaveOutputWithTerminatorIncluded) {
  // The terminator-included reporting the pinned core uses: 18 for the
  // 17-char address. This read must load the record save() wrote.
  const auto saved = saveOutputBuffer();
  companionidentity::StoredIdentity loaded{};
  ASSERT_TRUE(companionidentity::decodeMirrorRecord(18, saved.addr, saved.type, loaded));
  EXPECT_STREQ(loaded.addr, kAddr);
  EXPECT_EQ(loaded.type, blecompanion::ADDR_TYPE_RANDOM);
}

TEST(IdentityReadback, AcceptsSaveOutputWithTerminatorExcluded) {
  // A core that reports the string length without the terminator: the same
  // buffer reads back through the same acceptance path.
  const auto saved = saveOutputBuffer();
  companionidentity::StoredIdentity loaded{};
  ASSERT_TRUE(companionidentity::decodeMirrorRecord(17, saved.addr, saved.type, loaded));
  EXPECT_STREQ(loaded.addr, kAddr);
  EXPECT_EQ(loaded.type, blecompanion::ADDR_TYPE_RANDOM);
}

TEST(IdentityReadback, RejectsForeignLengths) {
  // Arbitrary non-zero lengths are not records save() can produce.
  const auto saved = saveOutputBuffer();
  companionidentity::StoredIdentity loaded{};
  EXPECT_FALSE(companionidentity::decodeMirrorRecord(0, saved.addr, saved.type, loaded));
  EXPECT_FALSE(companionidentity::decodeMirrorRecord(16, saved.addr, saved.type, loaded));
  EXPECT_FALSE(companionidentity::decodeMirrorRecord(19, saved.addr, saved.type, loaded));
  EXPECT_FALSE(companionidentity::decodeMirrorRecord(4096, saved.addr, saved.type, loaded));
}

TEST(IdentityReadback, RejectsUnterminatedBuffer) {
  // All 18 bytes non-NUL: strlen would run past the buffer, so the bounds
  // guard must reject before any text scan.
  companionidentity::StoredIdentity saved = saveOutputBuffer();
  saved.addr[17] = 'x';
  companionidentity::StoredIdentity loaded{};
  EXPECT_FALSE(companionidentity::decodeMirrorRecord(18, saved.addr, saved.type, loaded));
}

TEST(IdentityReadback, RejectsMalformedContentAndType) {
  companionidentity::StoredIdentity loaded{};
  // A 16-character address is foreign content, whatever length is reported.
  companionidentity::StoredIdentity shortSaved = saveOutputBuffer();
  memcpy(shortSaved.addr, "aa:bb:cc:dd:ee:f", 16);
  shortSaved.addr[16] = '\0';
  EXPECT_FALSE(companionidentity::decodeMirrorRecord(17, shortSaved.addr, shortSaved.type, loaded));
  EXPECT_FALSE(companionidentity::decodeMirrorRecord(18, shortSaved.addr, shortSaved.type, loaded));
  // Unset/unknown type bytes fail the format check.
  const auto saved = saveOutputBuffer();
  EXPECT_FALSE(companionidentity::decodeMirrorRecord(18, saved.addr, 0xFF, loaded));
  EXPECT_FALSE(companionidentity::decodeMirrorRecord(18, saved.addr, 2, loaded));
}

// The production load() must route through the readback core: the device-side
// length comparison cannot be exercised by a host build, so the source pin is
// the guard that keeps the acceptance logic in the host-tested core.
TEST(IdentityReadback, ProductionLoadUsesTheReadbackCore) {
  const std::string source = readAll(fs::path(REPO_ROOT_PATH) / "src" / "util" / "CompanionIdentityMirror.cpp");
  EXPECT_NE(source.find("return decodeMirrorRecord(read, addr, type, out);"), std::string::npos)
      << "load() must accept the record through the host-tested readback core";
  EXPECT_EQ(source.find("read != 17"), std::string::npos)
      << "a terminator-dependent exact-length comparison must not return in load()";
  EXPECT_EQ(source.find("read != 18"), std::string::npos) << "a single-semantics length gate must not return in load()";
}
