#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

#include "lib/GfxRenderer/BitmapHelpers.h"
#include "lib/GfxRenderer/LabSettingsInput.h"
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

TEST(ImageLabTone, SettingsStepClampsAtRangeBounds) {
  // Brightness 70..110 step 5: cannot exceed the bounds in either direction.
  EXPECT_EQ(labStepClamped(110, 1, LAB_BRIGHTNESS_MIN, LAB_BRIGHTNESS_MAX), 110);
  EXPECT_EQ(labStepClamped(70, -1, LAB_BRIGHTNESS_MIN, LAB_BRIGHTNESS_MAX), 70);
  EXPECT_EQ(labStepClamped(105, 1, LAB_BRIGHTNESS_MIN, LAB_BRIGHTNESS_MAX), 110);
  // Gamma (x100) 70..130, Contrast 80..130 use the same helper.
  EXPECT_EQ(labStepClamped(130, 1, LAB_GAMMA_MIN, LAB_GAMMA_MAX), 130);
  EXPECT_EQ(labStepClamped(80, -1, LAB_CONTRAST_MIN, LAB_CONTRAST_MAX), 80);
  EXPECT_EQ(labStepClamped(100, 1, LAB_CONTRAST_MIN, LAB_CONTRAST_MAX), 105);
}

namespace {
using labSettingsInput::Action;
using labSettingsInput::Button;

Action event(const Button b, const bool press, const bool release) {
  return labSettingsInput::actionFor(b, press, release);
}
}  // namespace

// The Image settings page is 2-axis: one button event -> exactly ONE action.
// Vertical axis (row selection): side Up/Down ONLY. Horizontal axis (value
// stepping): front Left/Right ONLY. No merged NavNext/NavPrevious semantics.
TEST(ImageLabSettingsInput, VerticalAxisOnlyMovesRows) {
  EXPECT_EQ(event(Button::Up, true, false), Action::RowUp);
  EXPECT_EQ(event(Button::Down, true, false), Action::RowDown);
  // Release edges of the vertical keys are NOT actions (no double-fire).
  EXPECT_EQ(event(Button::Up, false, true), Action::None);
  EXPECT_EQ(event(Button::Down, false, true), Action::None);
}

TEST(ImageLabSettingsInput, HorizontalAxisOnlyStepsValues) {
  EXPECT_EQ(event(Button::Left, true, false), Action::ValueDown);
  EXPECT_EQ(event(Button::Right, true, false), Action::ValueUp);
  EXPECT_EQ(event(Button::Left, false, true), Action::None);
  EXPECT_EQ(event(Button::Right, false, true), Action::None);
}

TEST(ImageLabSettingsInput, ActionButtonsOnReleaseOnly) {
  EXPECT_EQ(event(Button::Confirm, false, true), Action::Activate);
  EXPECT_EQ(event(Button::Back, false, true), Action::Back);
  EXPECT_EQ(event(Button::Confirm, true, false), Action::None);
  EXPECT_EQ(event(Button::Back, true, false), Action::None);
}

TEST(ImageLabSettingsInput, NoEventMeansNoAction) {
  for (int b = 0; b <= 6; ++b) {
    EXPECT_EQ(event(static_cast<Button>(b), false, false), Action::None);
  }
}

TEST(ImageLabSettingsInput, ConfirmRoutesSettingsRows) {
  using labSettingsInput::confirmActionForRow;
  using labSettingsInput::RowAction;
  // Every editable value row AND the explicit Apply row share one route:
  // apply the draft to the viewer (Confirm applies without navigating down).
  for (int row = 0; row <= 4; ++row) {
    EXPECT_EQ(confirmActionForRow(row, 4), RowAction::ApplyViewer) << "row=" << row;
  }
  // The dedicated sleep row is the ONLY save-profile route — not viewer Apply.
  EXPECT_EQ(confirmActionForRow(5, 4), RowAction::SaveSleepProfile);
  // Out-of-range rows activate nothing; routes are mutually exclusive by
  // construction (an enum return can carry only one action).
  EXPECT_EQ(confirmActionForRow(-1, 4), RowAction::None);
  EXPECT_EQ(confirmActionForRow(6, 4), RowAction::None);
}

// ---- Sleep render profile (data/config type + shared normalization) ----

TEST(ImageLabSleepProfile, DefaultsArePristineRendering) {
  const ToneProfile profile{};
  EXPECT_EQ(profile.brightnessPct, 100);
  EXPECT_EQ(profile.gammaPct, 100);
  EXPECT_EQ(profile.contrastPct, 100);
  EXPECT_EQ(profile.quantizer, -1);  // no override: Bitmap keeps its fallback quantizer

  // The default profile must produce the SAME rendering decision the viewer
  // produces without any profile: identity LUT + fallback quantizer.
  ToneLut sleep = toneLutFromProfile(profile);
  buildToneLut(sleep);
  EXPECT_FALSE(sleep.enabled);  // baseline decode path (adjustPixel pass-through)
  EXPECT_EQ(toneLutQuantizerCanonical(&sleep, true), true);   // SSD1677 fallback preserved
  EXPECT_EQ(toneLutQuantizerCanonical(&sleep, false), false); // legacy-panel fallback preserved
  for (int i = 0; i < 256; ++i) EXPECT_EQ(sleep.map[i], i) << "i=" << i;
}

TEST(ImageLabSleepProfile, NormalizeClampsToLabRanges) {
  ToneProfile wild{255, 0, 200, 7};
  normalizeToneProfile(wild);
  EXPECT_EQ(wild.brightnessPct, LAB_BRIGHTNESS_MAX);
  EXPECT_EQ(wild.gammaPct, LAB_GAMMA_MIN);
  EXPECT_EQ(wild.contrastPct, LAB_CONTRAST_MAX);
  EXPECT_EQ(wild.quantizer, -1);  // out-of-range folds to default (settings-load convention)

  ToneProfile floor{0, 5, 10, 0};
  normalizeToneProfile(floor);
  EXPECT_EQ(floor.brightnessPct, LAB_BRIGHTNESS_MIN);
  EXPECT_EQ(floor.gammaPct, LAB_GAMMA_MIN);
  EXPECT_EQ(floor.contrastPct, LAB_CONTRAST_MIN);
  EXPECT_EQ(floor.quantizer, 0);

  ToneProfile valid{85, 90, 110, 1};
  normalizeToneProfile(valid);
  EXPECT_EQ(valid, (ToneProfile{85, 90, 110, 1}));  // in-range values pass through untouched
}

TEST(ImageLabSleepProfile, ProfileTransformMatchesViewerTransform) {
  // Same shared implementation: a ToneLut built from a persisted profile must
  // be identical to a ToneLut built directly from viewer values.
  const ToneLut viewer = makeLut(85, 90, 110);
  ToneLut sleep = toneLutFromProfile(ToneProfile{85, 90, 110, 1});
  buildToneLut(sleep);
  EXPECT_EQ(sleep.enabled, viewer.enabled);
  EXPECT_EQ(0, memcmp(sleep.map, viewer.map, sizeof(sleep.map)));
  EXPECT_EQ(toneLutQuantizerCanonical(&sleep, true), toneLutQuantizerCanonical(&viewer, true));

  const ToneLut viewerLegacy = makeLut(110, 70, 80);
  ToneLut sleepLegacy = toneLutFromProfile(ToneProfile{110, 70, 80, 0});
  buildToneLut(sleepLegacy);
  EXPECT_EQ(0, memcmp(sleepLegacy.map, viewerLegacy.map, sizeof(sleepLegacy.map)));
  EXPECT_EQ(toneLutQuantizerCanonical(&sleepLegacy, false), toneLutQuantizerCanonical(&viewerLegacy, false));
}

TEST(ImageLabSleepProfile, PersistedSubsetExtraction) {
  // toneProfileFromTone is the save path input: it reads EXACTLY the draft it
  // is given (the activity passes settingsDraft, never labTone) and normalizes
  // the quantizer subset (-1 default kept, explicit 0/1 kept).
  ToneLut draft = makeLut(85, 90, 110);
  draft.quantizer = 1;
  EXPECT_EQ(toneProfileFromTone(draft), (ToneProfile{85, 90, 110, 1}));

  draft.quantizer = -1;  // untouched quantizer: stored as "no override"
  EXPECT_EQ(toneProfileFromTone(draft), (ToneProfile{85, 90, 110, -1}));

  ToneLut garbage = makeLut(100, 100, 100);
  garbage.quantizer = 42;  // cannot happen via the UI; folds to the default
  EXPECT_EQ(toneProfileFromTone(garbage), (ToneProfile{100, 100, 100, -1}));
}

TEST(ImageLabSleepProfile, NormalizeIsIdempotent) {
  ToneProfile a{200, 65, 140, 9};
  normalizeToneProfile(a);
  ToneProfile b = a;
  normalizeToneProfile(b);
  EXPECT_EQ(a, b);  // save -> load -> save keeps the same values
}

TEST(ImageLabSettingsInput, PressWinsOverSimultaneousReleaseWindow) {
  // A button observed with both edges in one dispatch window still maps to a
  // single action (the press edge) for the directional axes.
  EXPECT_EQ(event(Button::Left, true, true), Action::ValueDown);
  // Confirm/Back act on release only; a physically ambiguous press+release
  // window maps to no action (the next release event re-fires normally).
  EXPECT_EQ(event(Button::Confirm, true, true), Action::None);
}
