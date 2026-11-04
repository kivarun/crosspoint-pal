#pragma once

#include <cstdint>

// The logical→physical panel coordinate transform shared by every GfxRenderer
// primitive, in a device-free header so host tests exercise the exact
// transform production uses (the GlyphBitmap.h pattern).
namespace panelCoordinates {

// Logical screen orientation from the perspective of callers
enum Orientation {
  Portrait,                  // 480x800 logical coordinates (current default)
  LandscapeClockwise,        // 800x480 logical coordinates, rotated 180° (swap top/bottom)
  PortraitInverted,          // 480x800 logical coordinates, inverted
  LandscapeCounterClockwise  // 800x480 logical coordinates, native panel orientation
};

// Translate logical (x,y) coordinates to physical panel coordinates based on
// current orientation
// This should always be inlined for better performance
inline void rotateCoordinates(const Orientation orientation, const int x, const int y, int* phyX, int* phyY,
                              const uint16_t panelWidth, const uint16_t panelHeight) {
  switch (orientation) {
    case Portrait: {
      // Logical portrait (480x800) → panel (800x480)
      // Rotation: 90 degrees clockwise
      *phyX = y;
      *phyY = panelHeight - 1 - x;
      break;
    }
    case LandscapeClockwise: {
      // Logical landscape (800x480) rotated 180 degrees (swap top/bottom and left/right)
      *phyX = panelWidth - 1 - x;
      *phyY = panelHeight - 1 - y;
      break;
    }
    case PortraitInverted: {
      // Logical portrait (480x800) → panel (800x480)
      // Rotation: 90 degrees counter-clockwise
      *phyX = panelWidth - 1 - y;
      *phyY = x;
      break;
    }
    case LandscapeCounterClockwise: {
      // Logical landscape (800x480) aligned with panel orientation
      *phyX = x;
      *phyY = y;
      break;
    }
  }
}

// One pixel of a 1bpp MSB-first bitmap authored in logical orientation (the
// Logo120/MoonIcon generator convention: white=1, black=0, row stride
// (width+7)/8). True when the pixel is ink (black).
inline bool imagePixelIsInk(const uint8_t* bitmap, const int width, const int col, const int row) {
  const int rowBytes = (width + 7) / 8;
  const uint8_t byte = bitmap[row * rowBytes + (col >> 3)];
  return ((byte >> (7 - (col & 7))) & 1) == 0;
}

}  // namespace panelCoordinates
