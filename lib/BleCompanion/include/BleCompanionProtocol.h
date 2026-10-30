#pragma once

// Companion protocol v1 constants. Pure — the codec, the session, the radio
// adapter and the host tests all read the same numbers from here.
namespace blecompanion {

constexpr uint8_t VERSION = 1;

// Both characteristics carry complete UTF-8 JSON objects up to this size.
constexpr size_t MAX_CONTROL_BYTES = 512;

// Field caps in bytes, enforced by the codec. The phone truncates at UTF-8
// code-point boundaries before sending; the reader rejects rather than
// truncates, so both sides agree on what was stored.
constexpr size_t MAX_TITLE_BYTES = 64;
constexpr size_t MAX_BODY_BYTES = 384;
// contentId echoes back to the phone inside a JSON string, so its charset is
// restricted to characters that need no escaping.
constexpr size_t MAX_CONTENT_ID_BYTES = 32;

constexpr const char* SERVICE_UUID = "7b1f1001-6f70-4f69-8f31-63726f737370";
constexpr const char* CONTROL_UUID = "7b1f1002-6f70-4f69-8f31-63726f737370";
constexpr const char* EVENT_UUID = "7b1f1003-6f70-4f69-8f31-63726f737370";

}  // namespace blecompanion
