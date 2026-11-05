#include <gtest/gtest.h>

#include <tuple>
#include <vector>

#include "lib/GfxRenderer/PanelCoordinates.h"

// The panel-coordinate transform shared by GfxRenderer::drawImage and every
// other renderer primitive, pinned with asymmetric images so the rotation
// direction is observable. The regression this suite pins: a drawImage that
// places pixels without rotating them shows the image turned 90° against
// correctly-rotated text in Portrait.

namespace {

using panelCoordinates::imagePixelIsInk;
using panelCoordinates::Orientation;
using panelCoordinates::rotateCoordinates;

constexpr uint16_t kPanelWidth = 40;
constexpr uint16_t kPanelHeight = 24;

// A 3x2 logical rect is asymmetric: its four bitmap corners stay
// distinguishable under rotation, so the table pins both the magnitude and
// the direction of the transform.
constexpr int kX = 2;
constexpr int kY = 5;
constexpr int kW = 3;
constexpr int kH = 2;

void expectBitmapCorner(const Orientation orientation, const int col, const int row, const int phyX, const int phyY) {
  int px = 0;
  int py = 0;
  rotateCoordinates(orientation, kX + col, kY + row, &px, &py, kPanelWidth, kPanelHeight);
  EXPECT_EQ(px, phyX);
  EXPECT_EQ(py, phyY);
}

// 16x10 asymmetric bitmap: ink only at the two diagonal corners, so every
// rotation maps the ink to a different physical position.
constexpr int kImageW = 16;
constexpr int kImageH = 10;
constexpr int kImageX = 8;
constexpr int kImageY = 9;

std::vector<uint8_t> diagonalInkImage() {
  // All white (white=1), then one ink pixel per diagonal corner: the byte
  // holds 1=white, 0=ink, MSB-first across the row.
  std::vector<uint8_t> bitmap((kImageW / 8) * kImageH, 0xFF);
  bitmap[0] = 0x7F;          // ink at bitmap (0, 0)
  bitmap[9 * 2 + 1] = 0xFE;  // ink at bitmap (15, 9)
  return bitmap;
}

std::vector<std::pair<int, int>> inkPhysicalPositions(const std::vector<uint8_t>& bitmap,
                                                      const Orientation orientation) {
  std::vector<std::pair<int, int>> found;
  for (int row = 0; row < kImageH; row++) {
    for (int col = 0; col < kImageW; col++) {
      if (imagePixelIsInk(bitmap.data(), kImageW, col, row)) {
        int phyX = 0;
        int phyY = 0;
        // The drawImage contract: bitmap pixel (col,row) is drawn at the
        // logical position (x+col, y+row) and lands wherever the shared
        // transform puts it.
        rotateCoordinates(orientation, kImageX + col, kImageY + row, &phyX, &phyY, kPanelWidth, kPanelHeight);
        found.emplace_back(phyX, phyY);
      }
    }
  }
  return found;
}

}  // namespace

TEST(ImageRotation, CornerTablePerOrientation) {
  expectBitmapCorner(Orientation::Portrait, 0, 0, 5, 21);
  expectBitmapCorner(Orientation::Portrait, 2, 0, 5, 19);
  expectBitmapCorner(Orientation::Portrait, 0, 1, 6, 21);
  expectBitmapCorner(Orientation::Portrait, 2, 1, 6, 19);

  expectBitmapCorner(Orientation::LandscapeClockwise, 0, 0, 37, 18);
  expectBitmapCorner(Orientation::LandscapeClockwise, 2, 0, 35, 18);
  expectBitmapCorner(Orientation::LandscapeClockwise, 0, 1, 37, 17);
  expectBitmapCorner(Orientation::LandscapeClockwise, 2, 1, 35, 17);

  expectBitmapCorner(Orientation::PortraitInverted, 0, 0, 34, 2);
  expectBitmapCorner(Orientation::PortraitInverted, 2, 0, 34, 4);
  expectBitmapCorner(Orientation::PortraitInverted, 0, 1, 33, 2);
  expectBitmapCorner(Orientation::PortraitInverted, 2, 1, 33, 4);

  expectBitmapCorner(Orientation::LandscapeCounterClockwise, 0, 0, 2, 5);
  expectBitmapCorner(Orientation::LandscapeCounterClockwise, 2, 0, 4, 5);
  expectBitmapCorner(Orientation::LandscapeCounterClockwise, 0, 1, 2, 6);
  expectBitmapCorner(Orientation::LandscapeCounterClockwise, 2, 1, 4, 6);
}

TEST(ImageRotation, AsymmetricImageRotatesWithThePanel) {
  // The regression this test pins: in the retired raw-blit drawImage, bitmap
  // pixel (0,0) landed at the blit origin (y, panelHeight-1-x-h) — in this
  // Portrait setup (9, 5) — instead of where the pixel transform puts the
  // logical top-left (9, 15). The image rotated 90° against the text.
  const std::vector<uint8_t> bitmap = diagonalInkImage();

  const std::vector<std::pair<int, int>> portrait = inkPhysicalPositions(bitmap, Orientation::Portrait);
  ASSERT_EQ(portrait.size(), 2u);
  EXPECT_EQ(portrait[0], std::make_pair(9, 15));  // bitmap (0,0): logical top-left
  EXPECT_EQ(portrait[1], std::make_pair(18, 0));  // bitmap (15,9): logical bottom-right

  const std::vector<std::pair<int, int>> inverted = inkPhysicalPositions(bitmap, Orientation::PortraitInverted);
  ASSERT_EQ(inverted.size(), 2u);
  EXPECT_EQ(inverted[0], std::make_pair(30, 8));
  EXPECT_EQ(inverted[1], std::make_pair(21, 23));

  const std::vector<std::pair<int, int>> clockwise = inkPhysicalPositions(bitmap, Orientation::LandscapeClockwise);
  ASSERT_EQ(clockwise.size(), 2u);
  EXPECT_EQ(clockwise[0], std::make_pair(31, 14));
  EXPECT_EQ(clockwise[1], std::make_pair(16, 5));

  const std::vector<std::pair<int, int>> native = inkPhysicalPositions(bitmap, Orientation::LandscapeCounterClockwise);
  ASSERT_EQ(native.size(), 2u);
  EXPECT_EQ(native[0], std::make_pair(kImageX, kImageY));
  EXPECT_EQ(native[1], std::make_pair(kImageX + 15, kImageY + 9));
}

TEST(ImageRotation, ImagePixelIsInkDecodesMsbFirstRowsBeyondOneByte) {
  // 12 wide: rowBytes=2 and the low nibble of the second byte is padding.
  // white=1, black=0, MSB-first.
  constexpr int kW = 12;
  static const uint8_t bitmap[] = {
      0x7F, 0x00,  // ink only at (0,0)
      0xFF, 0xEF,  // ink only at (11,1)
      0xFF, 0xFF,  // all 12 white (padding bits read white too)
      0xFF, 0x0F,  // ink at columns 8-11 of row 3
  };
  EXPECT_TRUE(imagePixelIsInk(bitmap, kW, 0, 0));
  EXPECT_FALSE(imagePixelIsInk(bitmap, kW, 1, 0));
  EXPECT_TRUE(imagePixelIsInk(bitmap, kW, 11, 1));
  EXPECT_FALSE(imagePixelIsInk(bitmap, kW, 0, 1));
  EXPECT_FALSE(imagePixelIsInk(bitmap, kW, 0, 2));
  EXPECT_FALSE(imagePixelIsInk(bitmap, kW, 7, 2));
  EXPECT_TRUE(imagePixelIsInk(bitmap, kW, 8, 3));
  EXPECT_TRUE(imagePixelIsInk(bitmap, kW, 11, 3));
}

TEST(ImageRotation, DrawLogicalImageMatchesTheDrawImageContract) {
  // drawImage delegates its loop to drawLogicalImage and plots each reported
  // call through drawPixel. The test drives the same pure loop with a
  // recording sink, so the placement/polarity contract of the production path
  // itself is pinned, not just the helpers it calls.
  const std::vector<uint8_t> bitmap = diagonalInkImage();
  std::vector<std::tuple<int, int, bool>> calls;
  panelCoordinates::drawLogicalImage(bitmap.data(), kImageW, kImageH, kImageX, kImageY,
                                     [&calls](const int logicalX, const int logicalY, const bool ink) {
                                       calls.emplace_back(logicalX, logicalY, ink);
                                     });
  ASSERT_EQ(calls.size(), static_cast<size_t>(kImageW * kImageH));  // every pixel reported exactly once
  // Row-major scan order: position (row, col) reports logical (x+col, y+row).
  const size_t topLeft = 0;
  const size_t oneRight = 1;
  const size_t bottomRight = static_cast<size_t>(9 * kImageW + 15);
  EXPECT_EQ(std::get<0>(calls[topLeft]), kImageX);
  EXPECT_EQ(std::get<1>(calls[topLeft]), kImageY);
  EXPECT_EQ(std::get<2>(calls[topLeft]), true);
  EXPECT_EQ(std::get<0>(calls[oneRight]), kImageX + 1);
  EXPECT_EQ(std::get<1>(calls[oneRight]), kImageY);
  EXPECT_EQ(std::get<2>(calls[oneRight]), false);  // white stays white
  EXPECT_EQ(std::get<0>(calls[bottomRight]), kImageX + 15);
  EXPECT_EQ(std::get<1>(calls[bottomRight]), kImageY + 9);
  EXPECT_EQ(std::get<2>(calls[bottomRight]), true);
}
