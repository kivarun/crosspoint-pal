// The compile-time companion advertising interval override (P1-B9-E3), kept
// NimBLE-free so host tests can pin the millisecond → BLE-unit arithmetic.
#pragma once

#include <cstdint>

namespace blecompanion {
// One Bluetooth LE advertising unit is 0.625 ms (BLE_HCI_ADV_ITVL). The
// exact-unit helper stays uint32_t so range checks never see a truncated
// value; the narrow result is exact whenever the millisecond value is a
// multiple of 5, which every declared experiment value is.
constexpr uint32_t advertisingUnitsExact(uint32_t ms) { return (ms * 8) / 5; }

constexpr uint16_t advertisingUnitsFromMs(uint32_t ms) { return static_cast<uint16_t>(advertisingUnitsExact(ms)); }
}  // namespace blecompanion
