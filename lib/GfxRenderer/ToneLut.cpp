#include "ToneLut.h"

#include <cmath>

// See ToneLut.h for the formula and the fixed B -> G -> C order.
void buildToneLut(ToneLut& lut) {
  const bool brightnessActive = lut.brightnessPct != 100;
  const bool gammaActive = lut.gammaPct != 100;
  const bool contrastActive = lut.contrastPct != 100;
  lut.enabled = brightnessActive || gammaActive || contrastActive;
  if (!lut.enabled) {
    for (int i = 0; i < 256; i++) lut.map[i] = static_cast<uint8_t>(i);
    return;
  }

  const float brightness = static_cast<float>(lut.brightnessPct) / 100.0f;
  const float gamma = static_cast<float>(lut.gammaPct) / 100.0f;
  const float contrast = static_cast<float>(lut.contrastPct) / 100.0f;

  for (int i = 0; i < 256; i++) {
    float v = static_cast<float>(i);
    // 1. Brightness: multiplicative.
    if (brightnessActive) {
      v *= brightness;
      if (v > 255.0f) v = 255.0f;
      if (v < 0.0f) v = 0.0f;
    }
    // 2. Gamma: out = 255 * (v/255)^G; G > 1 darkens midtones, G < 1 brightens.
    if (gammaActive) {
      v = 255.0f * powf(v / 255.0f, gamma);
      if (v > 255.0f) v = 255.0f;
      if (v < 0.0f) v = 0.0f;
    }
    // 3. Contrast: around the midpoint 128.
    if (contrastActive) {
      v = 128.0f + (v - 128.0f) * contrast;
      if (v > 255.0f) v = 255.0f;
      if (v < 0.0f) v = 0.0f;
    }
    lut.map[i] = static_cast<uint8_t>(v + 0.5f);
  }
}

bool toneLutQuantizerCanonical(const ToneLut* tone, bool fallback) {
  if (tone == nullptr || tone->quantizer < 0) return fallback;
  return tone->quantizer != 0;
}
