#pragma once

// Connection-handle semantics for the companion session, pure and
// host-testable. NimBLE uses BLE_HS_CONN_HANDLE_NONE (0xffff) as the "no
// connection" sentinel and 0x0000 is a perfectly valid connection handle, so
// the companion must never treat 0 as "no session".

#include <cstdint>

namespace blecompanion {

// The no-session sentinel; the adapter static_asserts equality with NimBLE's
// BLE_HS_CONN_HANDLE_NONE.
inline constexpr uint16_t kNoSessionHandle = 0xFFFF;

// Whether a ring entry belongs to the accepted companion session. False when
// no session is accepted or the handle is a foreign/rejected connection.
inline bool sessionBound(uint16_t accepted, uint16_t handle) {
  return accepted != kNoSessionHandle && accepted == handle;
}

}  // namespace blecompanion
