#pragma once

#include <Bitmap.h>
#include <HalDisplay.h>
#include <ToneLut.h>

#include <string>
#include <vector>

#include "GfxRenderer.h"
#include "util/SlideshowPolicy.h"

// Shared slideshow rendering seam (Image Viewer slideshow and global sleep
// slideshow): ONE BMP/PNG file rendered on the panel with an explicitly
// supplied tone LUT, using the ESTABLISHED SLEEP-IMAGE PRESENTATION POLICY —
// no chrome, no popup, no text, and NO post-presentation BW framebuffer
// rebuild (the interactive viewer's differential-UI preparation stays in
// BmpViewerActivity; the next content arrives with the next wake/frame
// paint). Both decode through the same Bitmap / PngToFramebufferConverter
// primitives — this module adds no second decoder and no third grayscale
// policy: the presentation below is the verbatim sleep-image contract of
// SleepActivity::renderBitmapSleepScreen().
namespace imageonly {

// Center an image on the page (shared by every BMP render path).
void fitOnScreen(int imageW, int imageH, int pageW, int pageH, int* x, int* y);

// Grayscale transfer mode STATIC sleep-image rendering uses: Direct when the
// panel supports it, Absolute otherwise. Callers of drawSleepBitmap() own the
// mode; slideshow frames always pass Absolute instead of this.
HalDisplay::GrayscaleMode sleepGrayscaleMode(const GfxRenderer& renderer);

// Decode a PNG into the framebuffer at its centered fit — no clear, no
// display, no text. False when the file cannot be read/decoded.
bool renderPngToFramebuffer(GfxRenderer& renderer, const std::string& path);

// The sleep-image presentation policy, extracted verbatim from
// SleepActivity::renderBitmapSleepScreen(): one drawBitmap pass into the
// cleared framebuffer, then the panel transfer the sleep pipeline uses —
// grayscale sources get the gray base + LSB/MSB plane transfers + the gray
// buffer (the gray base mode is the CALLER's policy: Direct, Absolute or the
// HALF-refresh fallback for panels without native gray support; the plane
// loop honors preserveBackground so overlay mode keeps its background bits),
// BW sources get the single-pass HALF_REFRESH transfer. hasGreyscale is the
// CALLER's source classification (the cover filter may deliberately downgrade
// a gray bitmap to BW). invertAfterDraw reproduces the cover screens'
// inverted-filter step between the draw and the base transfer. Deliberately
// NO BW framebuffer rebuild and no chrome afterward. Returns false when the
// bitmap could not be drawn (the HALF_REFRESH fallback transfer ran, as the
// sleep policy does).
bool drawSleepBitmap(GfxRenderer& renderer, const Bitmap& bitmap, const bool hasGreyscale,
                     const HalDisplay::GrayscaleMode grayscaleMode, const int x, const int y, const float cropX,
                     const float cropY, const bool preserveBackground, const bool invertAfterDraw);

// Render ONE BMP/PNG file as a slideshow frame with the supplied tone LUT:
// centered/fit, the sleep-image presentation policy above, no button hints,
// no loading popup, no error text, no BW framebuffer rebuild. False when the
// file cannot be opened, parsed or drawn — the caller owns the fallback
// (fail closed: no timer, repaint, or exit).
bool renderImageFile(GfxRenderer& renderer, const std::string& path, const ToneLut& tone);

// Deterministic sorted list of the BMP/PNG files directly inside dirPath
// (slideshow frame semantics: hidden files skipped, natural sort). Empty when
// the directory cannot be opened.
std::vector<std::string> listImageFiles(const std::string& dirPath);

// The next slideshow frame after currentPath inside ITS directory. Forward
// and Reverse walk the sorted list (slideshow::indexAfterAdvance); Random
// advances its randomized exhaustive cycle (slideshow::randomCycleNext,
// adapted from CrossPoint upstream PR #3841 by @gkaindl): every available
// image is shown exactly once before the first repeat, and the walk position
// is the current image's index. cycle is the retained metadata IN/OUT —
// returned updated (pass-through for Forward/Reverse); the caller persists
// it, this module owns no persistence. randomValue is the CALLER's device-RNG
// value feeding a new cycle's increment. path is "" when the directory holds
// no candidate image.
std::string nextImageAfter(const std::string& currentPath, const slideshow::Order order,
                           const slideshow::RandomCycleState& cycle, const uint32_t randomValue,
                           slideshow::RandomCycleState& cycleOut);

// The first frame of the sleep-slideshow source under the fixed storage
// contract, under the persisted ORDER policy (slideshow::initialIndex):
// Forward opens the first sorted entry, Reverse the last, Random an
// arbitrary one. /.sleep scanned first, /sleep as the legacy fallback. ""
// when neither directory offers a candidate image (the caller fails closed:
// no timer).
std::string firstSleepSlideshowPath(const slideshow::Order order);

}  // namespace imageonly
