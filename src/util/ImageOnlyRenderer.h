#pragma once

#include <ToneLut.h>

#include <string>
#include <vector>

#include "GfxRenderer.h"
#include "util/SlideshowPolicy.h"

// Shared image-only rendering seam for the slideshow lifecycles (Image Viewer
// slideshow and global sleep slideshow): ONE BMP/PNG file rendered on the
// panel with an explicitly supplied tone LUT, no viewer chrome, no loading
// popup, no text. The normal Image Viewer (button hints, popups) keeps its own
// chrome path in BmpViewerActivity; both decode through the same Bitmap /
// PngToFramebufferConverter primitives — this module adds no second decoder.
namespace imageonly {

// Center an image on the page (shared by every BMP render path).
void fitOnScreen(int imageW, int imageH, int pageW, int pageH, int* x, int* y);

// Decode a PNG into the framebuffer at its centered fit — no clear, no
// display, no text. False when the file cannot be read/decoded.
bool renderPngToFramebuffer(GfxRenderer& renderer, const std::string& path);

// Render ONE BMP/PNG file image-only with the supplied tone LUT: centered/fit,
// full grayscale pipeline for gray sources, cleared framebuffer, no button
// hints, no loading popup, no error text. False when the file cannot be
// opened, parsed or drawn — the framebuffer content is then undefined and the
// caller owns the fallback (fail closed: no timer, repaint, or exit).
bool renderImageFile(GfxRenderer& renderer, const std::string& path, const ToneLut& tone);

// Deterministic sorted list of the BMP/PNG files directly inside dirPath
// (viewer slideshow semantics: hidden files skipped, natural sort). Empty when
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
