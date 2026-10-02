#include "util/ImageOnlyRenderer.h"

#include <Bitmap.h>
#include <Epub/converters/PngToFramebufferConverter.h>
#include <FsHelpers.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include <esp_random.h>

namespace imageonly {

namespace {

// SdFat long-name buffer, same bound as the viewer's sibling scan.
constexpr size_t MAX_IMAGE_NAME_LEN = 500;

}  // namespace

void fitOnScreen(const int imageW, const int imageH, const int pageW, const int pageH, int* x, int* y) {
  if (imageW > pageW || imageH > pageH) {
    const float ratio = static_cast<float>(imageW) / static_cast<float>(imageH);
    const float screenRatio = static_cast<float>(pageW) / static_cast<float>(pageH);
    if (ratio > screenRatio) {
      *x = 0;
      *y = std::round((static_cast<float>(pageH) - static_cast<float>(pageW) / ratio) / 2);
    } else {
      *x = std::round((static_cast<float>(pageW) - static_cast<float>(pageH) * ratio) / 2);
      *y = 0;
    }
  } else {
    *x = (pageW - imageW) / 2;
    *y = (pageH - imageH) / 2;
  }
}

bool renderPngToFramebuffer(GfxRenderer& renderer, const std::string& path) {
  ImageDimensions dimensions;
  if (!PngToFramebufferConverter::getDimensionsStatic(path, dimensions)) return false;
  if (dimensions.width <= 0 || dimensions.height <= 0) return false;

  const float scale = std::min(static_cast<float>(renderer.getScreenWidth()) / dimensions.width,
                               static_cast<float>(renderer.getScreenHeight()) / dimensions.height);
  const int width = std::min(renderer.getScreenWidth(), static_cast<int>(dimensions.width * std::min(scale, 1.0f)));
  const int height = std::min(renderer.getScreenHeight(), static_cast<int>(dimensions.height * std::min(scale, 1.0f)));
  RenderConfig config{(renderer.getScreenWidth() - width) / 2, (renderer.getScreenHeight() - height) / 2, width,
                      height};

  PngToFramebufferConverter converter;
  return converter.decodeToFramebuffer(path, renderer, config);
}

HalDisplay::GrayscaleMode sleepGrayscaleMode(const GfxRenderer& renderer) {
  return renderer.grayscaleCapabilities(HalDisplay::GrayscaleMode::Direct).supported()
             ? HalDisplay::GrayscaleMode::Direct
             : HalDisplay::GrayscaleMode::Absolute;
}

bool drawSleepBitmap(GfxRenderer& renderer, const Bitmap& bitmap, const bool hasGreyscale,
                     const HalDisplay::GrayscaleMode grayscaleMode, const int x, const int y, const float cropX,
                     const float cropY, const bool preserveBackground, const bool invertAfterDraw) {
  // Verbatim sleep-image presentation contract of
  // SleepActivity::renderBitmapSleepScreen() (minus the cover placement): one
  // drawBitmap pass, then the gray base + plane transfers + gray buffer for
  // grayscale sources, or the single-pass HALF_REFRESH transfer for BW
  // sources. No BW framebuffer rebuild — the next content arrives with the
  // next wake/frame paint. hasGreyscale is the CALLER's source classification
  // (the cover filter may deliberately downgrade a gray bitmap to BW).
  // grayscaleMode is the CALLER's gray policy: static sleep screens keep the
  // Direct-capable sleepGrayscaleMode(), slideshow frames pass Absolute so
  // every timer wake gets the scrubbing base transition over the physically
  // retained e-ink frame.
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  if (!preserveBackground) renderer.clearScreen();

  if (!renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY, preserveBackground)) {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    return false;
  }

  if (invertAfterDraw) renderer.invertScreen();

  const bool nativeGray = hasGreyscale && renderer.grayscaleCapabilities(grayscaleMode).supported();
  if (nativeGray) {
    if (!renderer.displayGrayscaleBase(grayscaleMode)) return true;
  } else if (hasGreyscale) {
    // OEM grayscale pipeline base. Must stay HALF: the gray nudge LUT is
    // calibrated against the pixel state the single-pass HALF waveform leaves
    // behind. A FULL (GC) base parks pixels in a different charge state and
    // the differential nudge then lands unevenly (blotchy noise in gray areas).
    renderer.displayGrayscaleBase(HalDisplay::HALF_REFRESH);
  } else {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    return true;
  }

  bool ready = true;
  for (const auto plane : {GfxRenderer::GRAYSCALE_LSB, GfxRenderer::GRAYSCALE_MSB}) {
    if (bitmap.rewindToData() != BmpReaderError::Ok) {
      ready = false;
      break;
    }
    if (!nativeGray || !preserveBackground) renderer.clearScreen(nativeGray ? 0xFF : 0x00);
    renderer.setRenderMode(plane);
    if (!renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY, preserveBackground)) {
      ready = false;
      break;
    }
    if (plane == GfxRenderer::GRAYSCALE_LSB)
      renderer.copyGrayscaleLsbBuffers();
    else
      renderer.copyGrayscaleMsbBuffers();
  }
  if (ready)
    renderer.displayGrayBuffer();
  else
    LOG_ERR("IMG", "Incomplete grayscale image; keeping the current display");
  renderer.setRenderMode(GfxRenderer::BW);
  return true;
}

bool renderImageFile(GfxRenderer& renderer, const std::string& path, const ToneLut& tone) {
  if (FsHelpers::hasPngExtension(path)) {
    // PNG: clear + centered decode + the sleep/slideshow clean refresh (the
    // single-pass HALF_REFRESH sleep contract, not the interactive viewer's
    // FAST_REFRESH). The converter has no tone hook, so the supplied LUT does
    // not apply here.
    renderer.clearScreen();
    if (!renderPngToFramebuffer(renderer, path)) return false;
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    return true;
  }

  HalFile file;
  if (!Storage.openFileForRead("IMG", path, file)) return false;

  // Always attach the tone: quantizer selection is active even when the LUT
  // itself is at identity (enabled=false). The gray-capable gate follows the
  // slideshow gray mode below (SSD1677 panels).
  const auto slideshowGrayMode = HalDisplay::GrayscaleMode::Absolute;
  Bitmap bitmap(file, true,
                renderer.grayscaleCapabilities(slideshowGrayMode).supported() &&
                    display.getController() == HalDisplay::Controller::SSD1677,
                &tone);

  if (bitmap.parseHeaders() != BmpReaderError::Ok) return false;

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  int x, y;
  fitOnScreen(bitmap.getWidth(), bitmap.getHeight(), pageWidth, pageHeight, &x, &y);

  // Slideshow presentation: the sleep-image policy (draw + panel transfer,
  // no chrome, no BW framebuffer rebuild), with the grayscale base always
  // Absolute — the scrub/base transition clears the physically retained
  // e-ink frame each timer wake, where Direct would layer onto it.
  return drawSleepBitmap(renderer, bitmap, bitmap.hasGreyscale(), slideshowGrayMode, x, y,
                         /*cropX=*/0.0f, /*cropY=*/0.0f, /*preserveBackground=*/false, /*invertAfterDraw=*/false);
}

std::vector<std::string> listImageFiles(const std::string& dirPath) {
  std::vector<std::string> images;
  if (dirPath.empty()) return images;

  auto dir = Storage.open(dirPath.c_str());
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return images;
  }

  images.reserve(16);
  char name[MAX_IMAGE_NAME_LEN];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    if (!file.isDirectory()) {
      file.getName(name, sizeof(name));
      if (name[0] != '.') {
        std::string fname(name);
        if (FsHelpers::hasBmpExtension(fname) || FsHelpers::hasPngExtension(fname)) {
          images.push_back(fname);
        }
      }
    }
    file.close();
  }
  dir.close();

  FsHelpers::sortFileList(images);
  return images;
}

std::string nextImageAfter(const std::string& currentPath, const slideshow::Order order) {
  const std::string dirPath = FsHelpers::extractFolderPath(currentPath);
  const auto images = listImageFiles(dirPath);
  if (images.empty()) return {};

  const size_t lastSlash = currentPath.find_last_of('/');
  const std::string fileName = (lastSlash != std::string::npos) ? currentPath.substr(lastSlash + 1) : currentPath;
  const auto image = std::find(images.begin(), images.end(), fileName);
  // A current file missing from the scan hands the order policy an invalid
  // index (its documented restart semantics apply).
  const int currentIndex = image != images.end() ? static_cast<int>(image - images.begin()) : -1;
  // The device RNG (esp_random.h) feeds the Random order; the other orders
  // never look at the value.
  const uint32_t randomValue = order == slideshow::Order::Random ? esp_random() : 0;
  const int nextIndex =
      slideshow::indexAfterAdvance(currentIndex, static_cast<int>(images.size()), order, randomValue);
  if (nextIndex < 0 || nextIndex >= static_cast<int>(images.size())) return {};

  std::string path = dirPath;
  if (!path.empty() && path.back() != '/') path += '/';
  path += images[nextIndex];
  return path;
}

std::string firstSleepSlideshowPath(const slideshow::Order order) {
  for (const char* dir : {slideshow::SLEEP_SLIDESHOW_DIR, slideshow::SLEEP_SLIDESHOW_DIR_LEGACY}) {
    const auto images = listImageFiles(dir);
    if (images.empty()) continue;
    const uint32_t randomValue = order == slideshow::Order::Random ? esp_random() : 0;
    const int index = slideshow::initialIndex(static_cast<int>(images.size()), order, randomValue);
    if (index < 0 || index >= static_cast<int>(images.size())) continue;

    std::string path = dir;
    path += '/';
    path += images[index];
    return path;
  }
  return {};
}

}  // namespace imageonly
