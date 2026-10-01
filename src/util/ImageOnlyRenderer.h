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

// Grayscale transfer mode the sleep-image presentation policy uses: Direct
// when the panel supports it, Absolute otherwise.
HalDisplay::GrayscaleMode sleepGrayscaleMode(const GfxRenderer& renderer);

// Decode a PNG into the framebuffer at its centered fit — no clear, no
// display, no text. False when the file cannot be read/decoded.
bool renderPngToFramebuffer(GfxRenderer& renderer, const std::string& path);

// The sleep-image presentation policy, extracted verbatim from
// SleepActivity::renderBitmapSleepScreen(): one drawBitmap pass into the
// cleared framebuffer, then the panel transfer the sleep pipeline uses —
// grayscale sources get the gray base + LSB/MSB plane transfers + the gray
// buffer (absolute vs non-absolute panel behavior preserved; the plane loop
// honors preserveBackground so overlay mode keeps its background bits), BW
// sources get the single-pass HALF_REFRESH transfer. hasGreyscale is the
// CALLER's source classification (the cover filter may deliberately downgrade
// a gray bitmap to BW). invertAfterDraw reproduces the cover screens'
// inverted-filter step between the draw and the base transfer. Deliberately
// NO BW framebuffer rebuild and no chrome afterward. Returns false when the
// bitmap could not be drawn (the HALF_REFRESH fallback transfer ran, as the
// sleep policy does).
bool drawSleepBitmap(GfxRenderer& renderer, const Bitmap& bitmap, const bool hasGreyscale, const int x, const int y,
                     const float cropX, const float cropY, const bool preserveBackground, const bool invertAfterDraw);

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

// The next slideshow frame after currentPath inside ITS directory: wrap at the
// end, restart from the first when the current file is missing from the scan.
// "" when the directory holds no candidate images.
std::string nextImageAfter(const std::string& currentPath);

// First frame of the sleep-slideshow source under the fixed storage contract:
// /.sleep scanned first, /sleep as the legacy fallback. "" when neither
// directory offers a candidate image (the caller fails closed: no timer).
std::string firstSleepSlideshowPath();

}  // namespace imageonly
