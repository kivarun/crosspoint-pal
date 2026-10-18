#include "CompanionPeer.h"

namespace blecompanion {

bool peerIdToString(const PeerId& peer, char* out, const size_t cap) {
  if (peer.unset() || cap < PEER_ID_TEXT_LEN + 1) return false;
  static const char HEX[] = "0123456789abcdef";
  size_t n = 0;
  for (size_t i = 0; i < sizeof(peer.addr); i++) {
    if (i != 0) out[n++] = ':';
    out[n++] = HEX[peer.addr[i] >> 4];
    out[n++] = HEX[peer.addr[i] & 0x0F];
  }
  out[n] = '\0';
  return true;
}

bool peerIdFromString(const char* text, const uint8_t type, PeerId& out) {
  out = PeerId{};
  if (text == nullptr || (type != ADDR_TYPE_PUBLIC && type != ADDR_TYPE_RANDOM)) return false;
  // Exactly 17 chars: six hex pairs separated by colons.
  size_t n = 0;
  for (size_t i = 0; i < sizeof(out.addr); i++) {
    if (i != 0) {
      if (text[n] != ':') return false;
      n++;
    }
    uint8_t byte = 0;
    for (size_t k = 0; k < 2; k++) {
      const char c = text[n++];
      uint8_t nibble = 0;
      if (c >= '0' && c <= '9') {
        nibble = static_cast<uint8_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        nibble = static_cast<uint8_t>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        nibble = static_cast<uint8_t>(c - 'A' + 10);
      } else {
        return false;
      }
      byte = static_cast<uint8_t>((byte << 4) | nibble);
    }
    out.addr[i] = byte;
  }
  if (text[n] != '\0') return false;
  out.type = type;
  return true;
}

bool companionBondTarget(const PeerId& stored, PeerId& out) {
  if (stored.unset()) return false;
  out = stored;
  return true;
}

}  // namespace blecompanion
