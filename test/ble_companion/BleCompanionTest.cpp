#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "BleCompanionJson.h"
#include "CompanionSession.h"
#include "ConnectionHandle.h"

using namespace blecompanion;

namespace {
ContentMessage makeContent() { return {}; }

std::string hello(uint8_t version) { return "{\"op\":\"hello\",\"version\":" + std::to_string(version) + "}"; }

std::string contentMsg(const std::string& title, const std::string& body, const std::string& id) {
  return "{\"op\":\"content\",\"title\":\"" + title + "\",\"body\":\"" + body + "\",\"contentId\":\"" + id + "\"}";
}
}  // namespace

TEST(BleCompanionJson, ValidHelloParses) {
  uint8_t version = 0;
  ContentMessage c = makeContent();
  EXPECT_EQ(parseControl(hello(1).data(), hello(1).size(), version, c), Op::Hello);
  EXPECT_EQ(version, 1);
}

TEST(BleCompanionJson, WrongVersionStillParsesAsHello) {
  // The version check belongs to the session, not the codec.
  uint8_t version = 0;
  ContentMessage c = makeContent();
  EXPECT_EQ(parseControl(hello(2).data(), hello(2).size(), version, c), Op::Hello);
  EXPECT_EQ(version, 2);
}

TEST(BleCompanionJson, UnknownOpIsNone) {
  const std::string other = "{\"op\":\"notification_poll\",\"limit\":5}";
  uint8_t version = 0;
  ContentMessage c = makeContent();
  EXPECT_EQ(parseControl(other.data(), other.size(), version, c), Op::None);
}

TEST(BleCompanionJson, MalformedShapesReject) {
  uint8_t version = 0;
  ContentMessage c = makeContent();
  EXPECT_EQ(parseControl("not json", 8, version, c), Op::Malformed);
  EXPECT_EQ(parseControl("{}", 2, version, c), Op::Malformed);
  EXPECT_EQ(parseControl("{\"op\":", 6, version, c), Op::Malformed);
  EXPECT_EQ(parseControl(hello(1).c_str(), hello(1).size() + 1, version, c), Op::Malformed);  // trailing NUL byte
  const std::string noVersion = "{\"op\":\"hello\"}";
  EXPECT_EQ(parseControl(noVersion.data(), noVersion.size(), version, c), Op::Malformed);
  const std::string extra = hello(1) + "x";
  EXPECT_EQ(parseControl(extra.data(), extra.size(), version, c), Op::Malformed);
  const std::string badEscape = "{\"op\":\"hel\\xlo\",\"version\":1}";
  EXPECT_EQ(parseControl(badEscape.data(), badEscape.size(), version, c), Op::Malformed);
}

TEST(BleCompanionJson, EscapesDecode) {
  uint8_t version = 0;
  ContentMessage c = makeContent();
  // \u0068\u0065\u006C\u006C\u006F spells hello.
  const std::string spelled = "{\"op\":\"hel\\u006Co\",\"version\":1}";
  EXPECT_EQ(parseControl(spelled.data(), spelled.size(), version, c), Op::Hello);
  EXPECT_EQ(version, 1);

  const std::string lines = contentMsg("Line1\\nLine2", "tab\\there", "id1");
  EXPECT_EQ(parseControl(lines.data(), lines.size(), version, c), Op::Content);
  EXPECT_STREQ(c.title, "Line1\nLine2");
  EXPECT_STREQ(c.body, "tab\there");

  const std::string smiley = contentMsg("\xF0\x9F\x98\x80", "a", "id1");  // literal 😀
  EXPECT_EQ(parseControl(smiley.data(), smiley.size(), version, c), Op::Content);
  const std::string escaped = contentMsg("\\uD83D\\uDE00", "a", "id1");  // escaped surrogate pair
  EXPECT_EQ(parseControl(escaped.data(), escaped.size(), version, c), Op::Content);
  const std::string unpaired = contentMsg("\\uD83D", "a", "id1");
  EXPECT_EQ(parseControl(unpaired.data(), unpaired.size(), version, c), Op::Malformed);
  const std::string alone = contentMsg("\\uDE00", "a", "id1");
  EXPECT_EQ(parseControl(alone.data(), alone.size(), version, c), Op::Malformed);
  // A raw control byte stays rejected even though \n is legal escaped.
  const std::string rawNewline = contentMsg("a\nb", "c", "id1");
  EXPECT_EQ(parseControl(rawNewline.data(), rawNewline.size(), version, c), Op::Malformed);
}

TEST(BleCompanionJson, OversizedPacketRejects) {
  uint8_t version = 0;
  ContentMessage c = makeContent();
  std::string big(MAX_CONTROL_BYTES + 1, 'x');
  EXPECT_EQ(parseControl(big.data(), big.size(), version, c), Op::Malformed);
}

TEST(BleCompanionJson, ContentBoundsEnforced) {
  uint8_t version = 0;
  ContentMessage c = makeContent();
  const std::string ok = contentMsg(std::string(MAX_TITLE_BYTES, 'a'), "b", "id1");
  EXPECT_EQ(parseControl(ok.data(), ok.size(), version, c), Op::Content);
  EXPECT_STREQ(c.contentId, "id1");
  EXPECT_EQ(c.title[MAX_TITLE_BYTES], '\0');

  const std::string longTitle = contentMsg(std::string(MAX_TITLE_BYTES + 1, 'a'), "b", "id1");
  EXPECT_EQ(parseControl(longTitle.data(), longTitle.size(), version, c), Op::Malformed);

  const std::string longBody = contentMsg("t", std::string(MAX_BODY_BYTES + 1, 'b'), "id1");
  EXPECT_EQ(parseControl(longBody.data(), longBody.size(), version, c), Op::Malformed);

  const std::string longId = contentMsg("t", "b", std::string(MAX_CONTENT_ID_BYTES + 1, 'x'));
  EXPECT_EQ(parseControl(longId.data(), longId.size(), version, c), Op::Malformed);
}

TEST(BleCompanionJson, Utf8BoundaryEnforced) {
  uint8_t version = 0;
  ContentMessage c = makeContent();
  // A title that ends mid-code-point.
  const std::string bad = contentMsg(std::string(3, 'a') + "\xC3", "b", "id1");
  EXPECT_EQ(parseControl(bad.data(), bad.size(), version, c), Op::Malformed);
  // Overlong encoding of '/'.
  const std::string overlong = contentMsg("\xC0\xAF", "b", "id1");
  EXPECT_EQ(parseControl(overlong.data(), overlong.size(), version, c), Op::Malformed);
  // A surrogate.
  const std::string surrogate = contentMsg("\xED\xA0\x80", "b", "id1");
  EXPECT_EQ(parseControl(surrogate.data(), surrogate.size(), version, c), Op::Malformed);
  // Two-byte code point at the cap boundary is fine.
  std::string twoByte(MAX_TITLE_BYTES - 2, 'a');
  twoByte += "\xC3\xA9";  // total exactly 64 bytes
  const std::string edge = contentMsg(twoByte, "b", "id1");
  EXPECT_EQ(parseControl(edge.data(), edge.size(), version, c), Op::Content);
}

TEST(BleCompanionJson, ContentIdCharsetRestricted) {
  uint8_t version = 0;
  ContentMessage c = makeContent();
  const std::string quoted = contentMsg("t", "b", "a\"b");
  EXPECT_EQ(parseControl(quoted.data(), quoted.size(), version, c), Op::Malformed);
  const std::string empty = contentMsg("t", "b", "");
  EXPECT_EQ(parseControl(empty.data(), empty.size(), version, c), Op::Malformed);
  const std::string punctuation = contentMsg("t", "b", "id.9_x:y");
  EXPECT_EQ(parseControl(punctuation.data(), punctuation.size(), version, c), Op::Content);
}

TEST(BleCompanionJson, WritersProduceBoundedJson) {
  char out[MAX_CONTROL_BYTES];
  size_t n = writeHelloAck(out, sizeof(out));
  EXPECT_GT(n, 0u);
  EXPECT_EQ(n, strlen(out));
  EXPECT_EQ(std::string(out, n), "{\"op\":\"hello_ack\",\"version\":1}");

  n = writeContentAck("id.1", out, sizeof(out));
  EXPECT_EQ(std::string(out, n), "{\"op\":\"content_ack\",\"contentId\":\"id.1\"}");

  n = writeButtonEvent(Button::Back, true, out, sizeof(out));
  EXPECT_EQ(std::string(out, n), "{\"op\":\"button\",\"button\":\"back\",\"pressed\":true}");
  n = writeButtonEvent(Button::Down, false, out, sizeof(out));
  EXPECT_EQ(std::string(out, n), "{\"op\":\"button\",\"button\":\"down\",\"pressed\":false}");

  n = writeError("bad_message", "Malformed control message", out, sizeof(out));
  EXPECT_EQ(std::string(out, n),
            "{\"op\":\"error\",\"code\":\"bad_message\",\"message\":\"Malformed control message\"}");

  char small[16];
  EXPECT_EQ(writeHelloAck(small, sizeof(small)), 0u);
  EXPECT_EQ(writeContentAck("id", small, sizeof(small)), 0u);
  EXPECT_EQ(writeButtonEvent(Button::Back, true, small, sizeof(small)), 0u);
}

TEST(BleCompanionSession, ContentBeforeHelloRejected) {
  CompanionSession s;
  const std::string msg = contentMsg("t", "b", "id1");
  EXPECT_EQ(s.onControl(msg.data(), msg.size()), SessionEvent::Rejected);
  EXPECT_EQ(s.lastRejection(), Rejection::NotHello);
  EXPECT_FALSE(s.helloed());
}

TEST(BleCompanionSession, HelloThenContentStores) {
  CompanionSession s;
  const std::string h = hello(1);
  EXPECT_EQ(s.onControl(h.data(), h.size()), SessionEvent::HelloAccepted);
  EXPECT_TRUE(s.helloed());
  const std::string msg = contentMsg("Title", "Body", "id1");
  EXPECT_EQ(s.onControl(msg.data(), msg.size()), SessionEvent::ContentStored);
  EXPECT_STREQ(s.content().title, "Title");
  EXPECT_STREQ(s.content().body, "Body");
}

TEST(BleCompanionSession, WrongVersionRejected) {
  CompanionSession s;
  const std::string h = hello(2);
  EXPECT_EQ(s.onControl(h.data(), h.size()), SessionEvent::Rejected);
  EXPECT_EQ(s.lastRejection(), Rejection::Version);
  EXPECT_FALSE(s.helloed());
  // A correct hello still works afterwards.
  const std::string good = hello(1);
  EXPECT_EQ(s.onControl(good.data(), good.size()), SessionEvent::HelloAccepted);
}

TEST(BleCompanionSession, RepeatedContentIdIsIdempotent) {
  CompanionSession s;
  const std::string h = hello(1);
  s.onControl(h.data(), h.size());
  const std::string msg = contentMsg("Title", "Body", "id1");
  EXPECT_EQ(s.onControl(msg.data(), msg.size()), SessionEvent::ContentStored);
  EXPECT_EQ(s.onControl(msg.data(), msg.size()), SessionEvent::ContentRepeat);
  // Same id, different payload is NOT a repeat.
  const std::string changed = contentMsg("Title", "Body 2", "id1");
  EXPECT_EQ(s.onControl(changed.data(), changed.size()), SessionEvent::ContentStored);
}

TEST(BleCompanionSession, DisconnectResetsEphemeralState) {
  CompanionSession s;
  const std::string h = hello(1);
  s.onControl(h.data(), h.size());
  const std::string msg = contentMsg("Title", "Body", "id1");
  EXPECT_EQ(s.onControl(msg.data(), msg.size()), SessionEvent::ContentStored);
  s.reset();
  EXPECT_FALSE(s.helloed());
  EXPECT_FALSE(s.contentPresent());
  // Content after the reset is rejected again.
  EXPECT_EQ(s.onControl(msg.data(), msg.size()), SessionEvent::Rejected);
  EXPECT_EQ(s.lastRejection(), Rejection::NotHello);
}

TEST(BleCompanionSession, AuthenticationDecisions) {
  PeerId first{};
  ASSERT_TRUE(peerIdFromString("aa:bb:cc:dd:ee:01", ADDR_TYPE_RANDOM, first));
  PeerId second{};
  ASSERT_TRUE(peerIdFromString("aa:bb:cc:dd:ee:02", ADDR_TYPE_RANDOM, second));
  PeerId unset{};
  EXPECT_TRUE(unset.unset());
  // First valid identity is adopted.
  EXPECT_EQ(CompanionSession::authenticate(first, unset), AuthDecision::Adopt);
  // The same identity is allowed.
  EXPECT_EQ(CompanionSession::authenticate(first, first), AuthDecision::Allow);
  // A different identity is rejected.
  EXPECT_EQ(CompanionSession::authenticate(second, first), AuthDecision::Reject);
  EXPECT_EQ(CompanionSession::authenticate(unset, first), AuthDecision::Reject);
  // Persistent pairing state lives outside the session: authenticating never
  // touches the session state machine.
  CompanionSession s;
  const std::string h = hello(1);
  s.onControl(h.data(), h.size());
  EXPECT_EQ(CompanionSession::authenticate(second, first), AuthDecision::Reject);
  EXPECT_TRUE(s.helloed());
}

TEST(BleCompanionPeer, IdentityComparesAddressAndType) {
  PeerId a{};
  ASSERT_TRUE(peerIdFromString("aa:bb:cc:dd:ee:01", ADDR_TYPE_RANDOM, a));
  PeerId b{};
  ASSERT_TRUE(peerIdFromString("aa:bb:cc:dd:ee:01", ADDR_TYPE_RANDOM, b));
  EXPECT_EQ(a, b);
  // Same bytes, different type: a DIFFERENT identity.
  PeerId otherType{};
  ASSERT_TRUE(peerIdFromString("aa:bb:cc:dd:ee:01", ADDR_TYPE_PUBLIC, otherType));
  EXPECT_NE(a, otherType);
  // Same text, different bytes: different.
  PeerId c{};
  ASSERT_TRUE(peerIdFromString("aa:bb:cc:dd:ee:02", ADDR_TYPE_RANDOM, c));
  EXPECT_NE(a, c);
}

TEST(BleCompanionPeer, TextRoundTrip) {
  PeerId in{};
  ASSERT_TRUE(peerIdFromString("a1:b2:c3:d4:e5:f6", ADDR_TYPE_RANDOM, in));
  char text[PEER_ID_TEXT_LEN + 1];
  ASSERT_TRUE(peerIdToString(in, text, sizeof(text)));
  EXPECT_STREQ(text, "a1:b2:c3:d4:e5:f6");
  PeerId out{};
  ASSERT_TRUE(peerIdFromString(text, ADDR_TYPE_RANDOM, out));
  EXPECT_EQ(in, out);
  // The stored form and a reloaded form with the same type are the same
  // identity: persistence survives a settings reload.
}

TEST(BleCompanionPeer, MalformedTextRejected) {
  PeerId out{};
  EXPECT_FALSE(peerIdFromString("", ADDR_TYPE_RANDOM, out));
  EXPECT_FALSE(peerIdFromString("aa:bb:cc:dd:ee", ADDR_TYPE_RANDOM, out));
  EXPECT_FALSE(peerIdFromString("aa:bb:cc:dd:ee:ff:00", ADDR_TYPE_RANDOM, out));
  EXPECT_FALSE(peerIdFromString("aa:bb:cc:dd:ee:gg", ADDR_TYPE_RANDOM, out));
  EXPECT_FALSE(peerIdFromString("aa:bb:cc:dd:ee:ff", 2, out));
  EXPECT_FALSE(peerIdFromString("aa:bb:cc:dd:ee:ff", 0xFF, out));
  EXPECT_TRUE(out.unset());
}

TEST(BleCompanionPeer, BondTargetSelectsExactIdentityOnly) {
  // A configured identity selects exactly that bond.
  PeerId stored{};
  ASSERT_TRUE(peerIdFromString("aa:bb:cc:dd:ee:01", ADDR_TYPE_RANDOM, stored));
  PeerId target{};
  ASSERT_TRUE(companionBondTarget(stored, target));
  EXPECT_EQ(target, stored);  // the EXACT identity, never a guess
  // No configured identity: nothing may be deleted (no delete-all fallback).
  PeerId unset{};
  EXPECT_FALSE(companionBondTarget(unset, target));
}

TEST(BleCompanionSession, ConnectionHandleZeroIsAValidAcceptedSession) {
  // BLE connection handle 0x0000 is valid: an accepted session on it must
  // behave like any other accepted handle.
  constexpr uint16_t kZero = 0;
  EXPECT_TRUE(sessionBound(kZero, kZero));  // entry from the accepted session
  EXPECT_FALSE(sessionBound(kZero, 7));     // a foreign connection's entry
  // The no-session sentinel never matches, not even handle 0 or 0xffff.
  EXPECT_FALSE(sessionBound(kNoSessionHandle, kZero));
  EXPECT_FALSE(sessionBound(kNoSessionHandle, kNoSessionHandle));
  // A real accepted handle keeps matching only itself.
  EXPECT_TRUE(sessionBound(5, 5));
  EXPECT_FALSE(sessionBound(5, 7));
  // The sentinel's value: NimBLE's BLE_HS_CONN_HANDLE_NONE (0xffff), asserted
  // against the real NimBLE constant in the adapter.
  EXPECT_EQ(kNoSessionHandle, 0xFFFF);
}
