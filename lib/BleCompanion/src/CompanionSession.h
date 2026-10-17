#pragma once

// The companion session state machine. Pure C++ — no NimBLE, no Arduino — so
// the host tests drive it directly. It owns only EPHEMERAL session state:
// the hello barrier and the content slot. Persistent pairing state lives in
// the firmware's settings, never here.

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "BleCompanionJson.h"
#include "BleCompanionProtocol.h"

namespace blecompanion {

enum class SessionState : uint8_t { AwaitingHello, Active };

enum class SessionEvent : uint8_t {
  None,          // the message changed nothing the UI shows
  HelloAccepted, // send hello_ack
  ContentStored, // the content slot changed: repaint
  ContentRepeat, // identical contentId and payload: ack only, no repaint
  Rejected,      // protocol error: send the error reply from lastRejection()
};

enum class Rejection : uint8_t {
  None,
  Version,   // hello with an unknown protocol version
  NotHello,  // any message before hello
  BadMessage // malformed or out of bounds
};

// Whether a peer may bond/authenticate. The radio adapter calls this with the
// address the firmware has persisted for the companion (empty = none yet).
enum class AuthDecision : uint8_t { Allow, Adopt, Reject };

class CompanionSession {
 public:
  SessionState state() const { return state_; }
  bool helloed() const { return state_ == SessionState::Active; }
  const ContentMessage& content() const { return content_; }
  bool contentPresent() const { return contentPresent_; }
  Rejection lastRejection() const { return rejection_; }

  // One control message from the phone.
  SessionEvent onControl(const char* data, size_t length) {
    uint8_t version = 0;
    const Op op = parseControl(data, length, version, content_);
    if (op == Op::Hello) {
      if (version != VERSION) {
        rejection_ = Rejection::Version;
        return SessionEvent::Rejected;
      }
      rejection_ = Rejection::None;
      state_ = SessionState::Active;
      return SessionEvent::HelloAccepted;
    }
    if (op == Op::Content) {
      if (state_ != SessionState::Active) {
        rejection_ = Rejection::NotHello;
        return SessionEvent::Rejected;
      }
      if (contentPresent_ && sameContent(stored_, content_)) {
        rejection_ = Rejection::None;
        return SessionEvent::ContentRepeat;
      }
      stored_ = content_;
      contentPresent_ = true;
      rejection_ = Rejection::None;
      return SessionEvent::ContentStored;
    }
    // Op::None (a JSON object we do not speak) and Op::Malformed both reject;
    // before hello any of them reads as "hello first", afterwards as a bad
    // message.
    if (state_ != SessionState::Active) {
      rejection_ = Rejection::NotHello;
    } else {
      rejection_ = Rejection::BadMessage;
    }
    return SessionEvent::Rejected;
  }

  // Authentication decision for a connecting peer against the persisted
  // companion address ("" when none). Adopt saves the address on the caller.
  static AuthDecision authenticate(const char* peerAddr, const char* storedAddr) {
    if (peerAddr == nullptr || peerAddr[0] == '\0') return AuthDecision::Reject;
    if (storedAddr != nullptr && storedAddr[0] != '\0' && strcmp(storedAddr, peerAddr) == 0) {
      return AuthDecision::Allow;
    }
    if (storedAddr == nullptr || storedAddr[0] == '\0') return AuthDecision::Adopt;
    return AuthDecision::Reject;  // one phone at a time; the user resets pairing to change it
  }

  // A disconnect ends the session and drops the content slot: nothing on the
  // screen promises a live link.
  void reset() {
    state_ = SessionState::AwaitingHello;
    contentPresent_ = false;
    content_ = {};
    rejection_ = Rejection::None;
  }

 private:
  static bool sameContent(const ContentMessage& a, const ContentMessage& b) {
    return strcmp(a.title, b.title) == 0 && strcmp(a.body, b.body) == 0 && strcmp(a.contentId, b.contentId) == 0;
  }

  SessionState state_ = SessionState::AwaitingHello;
  Rejection rejection_ = Rejection::None;
  bool contentPresent_ = false;
  ContentMessage content_{};
  ContentMessage stored_{};
};

}  // namespace blecompanion
