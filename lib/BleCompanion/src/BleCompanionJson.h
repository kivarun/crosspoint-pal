#pragma once

// Strict bounded codec for the companion protocol. Pure C++ — no Arduino, no
// heap: a fixed-shape scanner for the four v1 messages plus writers for the
// reader's replies. Every parse either fills a fixed-capacity result or fails;
// nothing is echoed back verbatim except contentId, whose charset the parser
// restricts to characters that need no JSON escaping.

#include <cstddef>
#include <cstdint>

#include "BleCompanionProtocol.h"

namespace blecompanion {

enum class Op : uint8_t {
  None,     // not a v1 message
  Hello,
  Content,
  Malformed,  // JSON-shaped but invalid for this protocol (bounds, charset, types)
};

struct ContentMessage {
  char title[MAX_TITLE_BYTES + 1];
  char body[MAX_BODY_BYTES + 1];
  char contentId[MAX_CONTENT_ID_BYTES + 1];
};

// One control message in, one opcode + fixed result out.
Op parseControl(const char* data, size_t length, uint8_t& version, ContentMessage& content);

// --- Writers. All return the written length, or 0 when `cap` was too small. ---
size_t writeHelloAck(char* out, size_t cap);
size_t writeContentAck(const char* contentId, char* out, size_t cap);
size_t writeError(const char* code, const char* message, char* out, size_t cap);

enum class Button : uint8_t { Back, Confirm, Left, Right, Up, Down };
size_t writeButtonEvent(Button button, bool pressed, char* out, size_t cap);

}  // namespace blecompanion
