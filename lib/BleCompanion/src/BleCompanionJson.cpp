#include "BleCompanionJson.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace blecompanion {
namespace {

bool skipSpace(const char* d, const size_t len, size_t& i) {
  while (i < len && (d[i] == ' ' || d[i] == '\t' || d[i] == '\n' || d[i] == '\r')) i++;
  return i < len;
}

// One \\uXXXX unit; `cp` is the code point for a non-surrogate, or the raw
// surrogate value when it is one (pairing checked by the caller).
bool readUnicodeEscape(const char* d, const size_t len, size_t& i, uint32_t& cp) {
  if (i + 4 > len) return false;
  cp = 0;
  for (size_t k = 0; k < 4; k++) {
    const char c = d[i + k];
    uint32_t nibble = 0;
    if (c >= '0' && c <= '9') {
      nibble = static_cast<uint32_t>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      nibble = static_cast<uint32_t>(c - 'a' + 10);
    } else if (c >= 'A' && c <= 'F') {
      nibble = static_cast<uint32_t>(c - 'A' + 10);
    } else {
      return false;
    }
    cp = (cp << 4) | nibble;
  }
  i += 4;
  return true;
}

// UTF-8 encodes one code point; false when it does not fit the buffer.
bool appendCodePoint(char* out, const size_t cap, size_t& n, const uint32_t cp) {
  size_t width = 0;
  uint8_t encoded[4] = {};
  if (cp < 0x80) {
    encoded[0] = static_cast<uint8_t>(cp);
    width = 1;
  } else if (cp < 0x800) {
    encoded[0] = static_cast<uint8_t>(0xC0 | (cp >> 6));
    encoded[1] = static_cast<uint8_t>(0x80 | (cp & 0x3F));
    width = 2;
  } else if (cp < 0x10000) {
    encoded[0] = static_cast<uint8_t>(0xE0 | (cp >> 12));
    encoded[1] = static_cast<uint8_t>(0x80 | ((cp >> 6) & 0x3F));
    encoded[2] = static_cast<uint8_t>(0x80 | (cp & 0x3F));
    width = 3;
  } else {
    encoded[0] = static_cast<uint8_t>(0xF0 | (cp >> 18));
    encoded[1] = static_cast<uint8_t>(0x80 | ((cp >> 12) & 0x3F));
    encoded[2] = static_cast<uint8_t>(0x80 | ((cp >> 6) & 0x3F));
    encoded[3] = static_cast<uint8_t>(0x80 | (cp & 0x3F));
    width = 4;
  }
  if (n + width >= cap) return false;
  memcpy(out + n, encoded, width);
  n += width;
  return true;
}

// One JSON string into a fixed buffer: at most cap-1 DECODED bytes. Escapes
// follow JSON: the short forms plus \uXXXX, including surrogate pairs. Raw
// control bytes stay rejected (JSON mandates escaping them) and the decoded
// result must fit exactly.
bool readString(const char* d, const size_t len, size_t& i, char* out, const size_t cap) {
  if (i >= len || d[i] != '"') return false;
  i++;
  size_t n = 0;
  while (true) {
    if (i >= len) return false;
    const char c = d[i];
    if (c == '"') {
      i++;
      out[n] = '\0';
      return true;
    }
    if (c == '\\') {
      i++;
      if (i >= len) return false;
      char decoded = 0;
      switch (d[i]) {
        case '"':
          decoded = '"';
          break;
        case '\\':
          decoded = '\\';
          break;
        case '/':
          decoded = '/';
          break;
        case 'b':
          decoded = '\b';
          break;
        case 'f':
          decoded = '\f';
          break;
        case 'n':
          decoded = '\n';
          break;
        case 'r':
          decoded = '\r';
          break;
        case 't':
          decoded = '\t';
          break;
        case 'u': {
          i++;
          uint32_t cp = 0;
          if (!readUnicodeEscape(d, len, i, cp)) return false;
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (i + 2 > len || d[i] != '\\' || d[i + 1] != 'u') return false;
            i += 2;
            uint32_t low = 0;
            if (!readUnicodeEscape(d, len, i, low) || low < 0xDC00 || low > 0xDFFF) return false;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            return false;  // unpaired low surrogate
          }
          if (!appendCodePoint(out, cap, n, cp)) return false;
          continue;  // readUnicodeEscape already moved past the hex digits
        }
        default:
          return false;
      }
      if (n >= cap - 1) return false;
      out[n++] = decoded;
      i++;
      continue;
    }
    if (static_cast<uint8_t>(c) < 0x20) return false;
    if (n >= cap - 1) return false;
    out[n++] = c;
    i++;
  }
}

// A non-negative integer that fits in uint16_t; the protocol only needs small
// numbers, so a loose grammar is a bug factory.
bool readUint(const char* d, const size_t len, size_t& i, uint32_t& out) {
  if (i >= len || d[i] < '0' || d[i] > '9') return false;
  uint32_t value = 0;
  while (i < len && d[i] >= '0' && d[i] <= '9') {
    value = value * 10 + static_cast<uint32_t>(d[i] - '0');
    if (value > 65535) return false;
    i++;
  }
  out = value;
  return true;
}

// Exactly `word` between the quotes at `i`, then `i` moves past the quotes.
bool matchKey(const char* d, const size_t len, size_t& i, const char* word) {
  const size_t n = strlen(word);
  if (i >= len || d[i] != '"') return false;
  if (i + n + 2 > len) return false;
  if (strncmp(d + i + 1, word, n) != 0 || d[i + 1 + n] != '"') return false;
  i += n + 2;
  return true;
}

bool colon(const char* d, const size_t len, size_t& i) {
  if (!skipSpace(d, len, i) || d[i] != ':') return false;
  i++;
  return skipSpace(d, len, i);
}

bool atEnd(const char* d, const size_t len, size_t& i) {
  skipSpace(d, len, i);
  return i == len;
}

// `}` then the end of the buffer.
bool closeObject(const char* d, const size_t len, size_t& i) {
  if (!skipSpace(d, len, i) || d[i] != '}') return false;
  i++;
  return atEnd(d, len, i);
}

// `{ "op":"opWord" ...` — the common prefix of every message. Leaves `i` on
// the comma (or `}` for op-only objects, which v1 never has); every
// subsequent key goes through nextKey.
bool openObject(const char* d, const size_t len, size_t& i, const char* opWord) {
  if (!skipSpace(d, len, i) || d[i] != '{') return false;
  i++;
  if (!skipSpace(d, len, i) || !matchKey(d, len, i, "op") || !colon(d, len, i)) return false;
  char op[24];
  if (!readString(d, len, i, op, sizeof(op)) || strcmp(op, opWord) != 0) return false;
  skipSpace(d, len, i);
  return true;
}

// `,"key":` between pairs.
bool nextKey(const char* d, const size_t len, size_t& i, const char* key) {
  if (!skipSpace(d, len, i) || d[i] != ',') return false;
  i++;
  if (!skipSpace(d, len, i) || !matchKey(d, len, i, key)) return false;
  return colon(d, len, i);
}

// Strict UTF-8: no overlongs, no surrogates, no >U+10FFFF, no bare
// continuation bytes. The stored strings go straight to the renderer.
bool validUtf8(const char* s, const size_t n) {
  size_t i = 0;
  while (i < n) {
    const uint8_t c = static_cast<uint8_t>(s[i]);
    if (c < 0x80) {
      i++;
      continue;
    }
    size_t rest = 0;
    uint32_t cp = 0;
    if (c >= 0xC2 && c <= 0xDF) {
      rest = 1;
      cp = c & 0x1F;
    } else if (c >= 0xE0 && c <= 0xEF) {
      rest = 2;
      cp = c & 0x0F;
    } else if (c >= 0xF0 && c <= 0xF4) {
      rest = 3;
      cp = c & 0x07;
    } else {
      return false;
    }
    if (i + rest >= n) return false;
    for (size_t k = 1; k <= rest; k++) {
      const uint8_t cc = static_cast<uint8_t>(s[i + k]);
      if ((cc & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (cc & 0x3F);
    }
    if ((rest == 2 && cp < 0x800) || (rest == 3 && cp < 0x10000)) return false;
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
    i += rest + 1;
  }
  return true;
}

bool plainId(const char* s, const size_t n) {
  for (size_t k = 0; k < n; k++) {
    const char c = s[k];
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                    c == '.' || c == '_' || c == '-' || c == ':';
    if (!ok) return false;
  }
  return true;
}

}  // namespace

Op parseControl(const char* data, const size_t length, uint8_t& version, ContentMessage& content) {
  version = 0;
  if (data == nullptr || length == 0 || length > MAX_CONTROL_BYTES) return Op::Malformed;
  size_t i = 0;
  if (!skipSpace(data, length, i) || data[i] != '{') return Op::Malformed;
  // Peek the op value first; the rest of the shape is checked per message.
  size_t probe = i;
  probe++;  // the '{'
  if (!skipSpace(data, length, probe) || !matchKey(data, length, probe, "op") || !colon(data, length, probe)) {
    return Op::Malformed;
  }
  char op[24];
  if (!readString(data, length, probe, op, sizeof(op))) return Op::Malformed;

  size_t p = i;
  if (strcmp(op, "hello") == 0) {
    if (!openObject(data, length, p, "hello") || !nextKey(data, length, p, "version")) return Op::Malformed;
    uint32_t v = 0;
    if (!readUint(data, length, p, v) || v > 255 || !closeObject(data, length, p)) return Op::Malformed;
    version = static_cast<uint8_t>(v);
    return Op::Hello;
  }

  if (strcmp(op, "content") == 0) {
    if (!openObject(data, length, p, "content")) return Op::Malformed;
    if (!nextKey(data, length, p, "title") ||
        !readString(data, length, p, content.title, sizeof(content.title))) {
      return Op::Malformed;
    }
    if (!nextKey(data, length, p, "body") ||
        !readString(data, length, p, content.body, sizeof(content.body))) {
      return Op::Malformed;
    }
    if (!nextKey(data, length, p, "contentId") ||
        !readString(data, length, p, content.contentId, sizeof(content.contentId))) {
      return Op::Malformed;
    }
    if (!closeObject(data, length, p)) return Op::Malformed;
    const size_t titleLen = strlen(content.title);
    const size_t bodyLen = strlen(content.body);
    const size_t idLen = strlen(content.contentId);
    if (titleLen == 0 || titleLen > MAX_TITLE_BYTES || !validUtf8(content.title, titleLen)) {
      return Op::Malformed;
    }
    if (bodyLen == 0 || bodyLen > MAX_BODY_BYTES || !validUtf8(content.body, bodyLen)) {
      return Op::Malformed;
    }
    if (idLen == 0 || idLen > MAX_CONTENT_ID_BYTES || !plainId(content.contentId, idLen)) {
      return Op::Malformed;
    }
    return Op::Content;
  }

  return Op::None;  // well-formed JSON object, but not a v1 message
}

size_t writeHelloAck(char* out, const size_t cap) {
  static const char VALUE[] = "{\"op\":\"hello_ack\",\"version\":1}";
  const size_t n = sizeof(VALUE) - 1;
  if (cap <= n) return 0;
  memcpy(out, VALUE, n + 1);
  return n;
}

size_t writeContentAck(const char* contentId, char* out, const size_t cap) {
  const size_t idLen = strlen(contentId);
  if (idLen == 0 || idLen > MAX_CONTENT_ID_BYTES || !plainId(contentId, idLen)) return 0;
  static const char PREFIX[] = "{\"op\":\"content_ack\",\"contentId\":\"";
  static const char SUFFIX[] = "\"}";
  const size_t plen = sizeof(PREFIX) - 1;
  const size_t slen = sizeof(SUFFIX) - 1;
  const size_t n = plen + idLen + slen;
  if (cap <= n) return 0;
  memcpy(out, PREFIX, plen);
  memcpy(out + plen, contentId, idLen);
  memcpy(out + plen + idLen, SUFFIX, slen);
  out[n] = '\0';
  return n;
}

size_t writeError(const char* code, const char* message, char* out, const size_t cap) {
  // Both words are the firmware's own; reject anything an escaper would need.
  for (const char* s : {code, message}) {
    if (s == nullptr) return 0;
    for (size_t k = 0; s[k] != '\0'; k++) {
      const char c = s[k];
      if (static_cast<uint8_t>(c) < 0x20 || c == '"' || c == '\\') return 0;
    }
  }
  const int n = snprintf(out, cap, "{\"op\":\"error\",\"code\":\"%s\",\"message\":\"%s\"}", code, message);
  if (n < 0 || static_cast<size_t>(n) >= cap) return 0;
  return static_cast<size_t>(n);
}

size_t writeButtonEvent(const Button button, const bool pressed, char* out, const size_t cap) {
  static const char* NAMES[] = {"back", "confirm", "left", "right", "up", "down"};
  const int n = snprintf(out, cap, "{\"op\":\"button\",\"button\":\"%s\",\"pressed\":%s}",
                         NAMES[static_cast<size_t>(button)], pressed ? "true" : "false");
  if (n < 0 || static_cast<size_t>(n) >= cap) return 0;
  return static_cast<size_t>(n);
}

}  // namespace blecompanion
