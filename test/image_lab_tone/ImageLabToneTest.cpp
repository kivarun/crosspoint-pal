#include <gtest/gtest.h>

#include <cstdint>

#include "lib/GfxRenderer/BitmapHelpers.h"
#include "lib/GfxRenderer/ToneLut.h"

namespace {

ToneLut makeLut(int brightnessPct, int gammaPct, int contrastPct) {
  ToneLut lut{};
  lut.brightnessPct = static_cast<uint8_t>(brightnessPct);
  lut.gammaPct = static_cast<uint8_t>(gammaPct);
  lut.contrastPct = static_cast<uint8_t>(contrastPct);
  buildToneLut(lut);
  return lut;
}

}  // namespace

TEST(ImageLabTone, IdentityLutIsExactPassThrough) {
  const ToneLut lut = makeLut(100, 100, 100);
  EXPECT_FALSE(lut.enabled);  // consumer keeps the baseline decode path
  for (int i = 0; i < 256; ++i) {
    EXPECT_EQ(lut.map[i], i) << "i=" << i;
  }
}

TEST(ImageLabTone, ClampedAtBothEnds) {
  const ToneLut max = makeLut(110, 130, 130);
  for (int i = 0; i < 256; ++i) {
    ASSERT_LE(max.map[i], 255) << "i=" << i;
    ASSERT_GE(max.map[i], 0) << "i=" << i;
  }
  EXPECT_EQ(max.map[255], 255);  // brightness 1.10 clips 255*1.10 -> 255; contrast clips 128+127*1.3 -> 255

  // With contrast disabled the poles stay pinned through brightness/gamma.
  const ToneLut pinned = makeLut(110, 130, 100);
  EXPECT_EQ(pinned.map[0], 0);    // 0 stays 0
  EXPECT_EQ(pinned.map[255], 255);  // clamped after brightness multiplication

  const ToneLut min = makeLut(70, 70, 80);
  for (int i = 0; i < 256; ++i) {
    ASSERT_LE(min.map[i], 255) << "i=" << i;
    ASSERT_GE(min.map[i], 0) << "i=" << i;
  }
}

TEST(ImageLabTone, BrightnessIsMultiplicative) {
  const ToneLut up = makeLut(110, 100, 100);
  EXPECT_EQ(up.map[100], 110);   // 100 * 1.10
  EXPECT_EQ(up.map[200], 220);   // 200 * 1.10
  EXPECT_EQ(up.map[240], 255);   // 264 clamps to 255
  const ToneLut down = makeLut(90, 100, 100);
  EXPECT_EQ(down.map[100], 90);  // 100 * 0.90
}

TEST(ImageLabTone, GammaAboveOneDarkensMidtonesBelowOneBrightens) {
  const ToneLut darker = makeLut(100, 130, 100);
  EXPECT_LT(darker.map[128], 128);  // 255*(128/255)^1.3 ~ 103
  EXPECT_LT(darker.map[60], 60);
  const ToneLut brighter = makeLut(100, 70, 100);
  EXPECT_GT(brighter.map[128], 128);  // 255*(128/255)^0.7 ~ 153
  EXPECT_GT(brighter.map[60], 60);
}

TEST(ImageLabTone, ContrastMovesAroundFixedMidpoint) {
  const ToneLut more = makeLut(100, 100, 130);
  EXPECT_EQ(more.map[128], 128);   // pivot unchanged
  EXPECT_EQ(more.map[64], 45);     // 128 + (64-128)*1.3 = 44.8
  EXPECT_EQ(more.map[192], 211);   // 128 + 64*1.3 = 211.2
  const ToneLut less = makeLut(100, 100, 80);
  EXPECT_EQ(less.map[128], 128);
  EXPECT_EQ(less.map[64], 77);     // 128 - 64*0.8 = 76.8
}

TEST(ImageLabTone, QuantizerOverrideSelection) {
  ToneLut tone{};
  // -1 = no override: falls back to the Bitmap constructor value
  EXPECT_EQ(toneLutQuantizerCanonical(&tone, false), false);
  EXPECT_EQ(toneLutQuantizerCanonical(&tone, true), true);
  EXPECT_EQ(toneLutQuantizerCanonical(nullptr, false), false);
  tone.quantizer = 0;  // legacy
  EXPECT_EQ(toneLutQuantizerCanonical(&tone, true), false);
  tone.quantizer = 1;  // canonical
  EXPECT_EQ(toneLutQuantizerCanonical(&tone, false), true);
}

TEST(ImageLabTone, QuantizerThresholdSetsDiverge) {
  // Legacy: 30/55/150. Canonical: 43/128/213.
  // gray=60: legacy -> level 2, canonical -> level 1.
  // gray=200: legacy -> level 3, canonical -> level 2.
  AtkinsonDitherer legacy(8, false);
  AtkinsonDitherer canonical(8, true);
  ASSERT_TRUE(legacy.isValid());
  ASSERT_TRUE(canonical.isValid());
  EXPECT_EQ(legacy.processPixel(60, 0), 2);
  EXPECT_EQ(canonical.processPixel(60, 0), 1);
  EXPECT_EQ(legacy.processPixel(200, 1), 3);
  EXPECT_EQ(canonical.processPixel(200, 1), 2);
}
