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

bool renderImageFile(GfxRenderer& renderer, const std::string& path, const ToneLut& tone) {
  if (FsHelpers::hasPngExtension(path)) {
    // PNG: clear + centered decode + clean refresh; the converter has no tone
    // hook, so the supplied LUT does not apply here.
    renderer.clearScreen();
    if (!renderPngToFramebuffer(renderer, path)) return false;
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return true;
  }

  HalFile file;
  if (!Storage.openFileForRead("IMG", path, file)) return false;

  // Always attach the tone: quantizer selection is active even when the LUT
  // itself is at identity (enabled=false). The gray-capable gate matches the
  // viewer's decode (SSD1677 panels render absolute gray).
  Bitmap bitmap(file, true,
                renderer.grayscaleCapabilities(HalDisplay::GrayscaleMode::Absolute).supported() &&
                    display.getController() == HalDisplay::Controller::SSD1677,
                &tone);

  if (bitmap.parseHeaders() != BmpReaderError::Ok) return false;

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  int x, y;
  fitOnScreen(bitmap.getWidth(), bitmap.getHeight(), pageWidth, pageHeight, &x, &y);

  renderer.clearScreen();
  if (!renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, 0, 0)) return false;

  if (!bitmap.hasGreyscale()) {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return true;
  }

  // Grayscale pipeline (viewer image-only path): base transfer, then the two
  // planes, then the gray buffer, then the BW framebuffer rebuild for
  // subsequent differential updates.
  const bool absolute = renderer.grayscaleCapabilities(HalDisplay::GrayscaleMode::Absolute).supported();
  if (absolute && !renderer.displayGrayscaleBase(HalDisplay::GrayscaleMode::Absolute)) return false;
  if (!absolute) renderer.displayGrayscaleBase(HalDisplay::HALF_REFRESH);
  bool planesReady = true;
  for (const auto mode : {GfxRenderer::GRAYSCALE_LSB, GfxRenderer::GRAYSCALE_MSB}) {
    if (bitmap.rewindToData() != BmpReaderError::Ok) {
      LOG_ERR("IMG", "Failed to rewind bitmap for grayscale rendering");
      planesReady = false;
      break;
    }
    renderer.clearScreen(absolute ? 0xFF : 0x00);
    renderer.setRenderMode(mode);
    if (!renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, 0, 0)) {
      planesReady = false;
      break;
    }
    if (mode == GfxRenderer::GRAYSCALE_LSB) {
      renderer.copyGrayscaleLsbBuffers();
    } else {
      renderer.copyGrayscaleMsbBuffers();
    }
  }
  if (planesReady) renderer.displayGrayBuffer();

  // Rebuild the BW framebuffer so the next paint starts from the shown image.
  renderer.setRenderMode(GfxRenderer::BW);
  renderer.clearScreen();
  if (bitmap.rewindToData() != BmpReaderError::Ok || !renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, 0, 0)) {
    LOG_ERR("IMG", "Failed to rewind bitmap to restore the BW framebuffer");
    planesReady = false;
  }
  renderer.cleanupGrayscaleWithFrameBuffer();
  if (!planesReady) renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  return true;
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

std::string nextImageAfter(const std::string& currentPath) {
  const std::string dirPath = FsHelpers::extractFolderPath(currentPath);
  const auto images = listImageFiles(dirPath);
  if (images.empty()) return {};

  const size_t lastSlash = currentPath.find_last_of('/');
  const std::string fileName = (lastSlash != std::string::npos) ? currentPath.substr(lastSlash + 1) : currentPath;
  const auto image = std::find(images.begin(), images.end(), fileName);
  // A current file missing from the scan restarts from the first image.
  const int currentIndex = image != images.end() ? static_cast<int>(image - images.begin()) : -1;
  const auto next = FsHelpers::imageIndexAfterAdvance(currentIndex, static_cast<int>(images.size()));
  if (!next.has_value() || *next < 0) return {};

  std::string path = dirPath;
  if (!path.empty() && path.back() != '/') path += '/';
  path += images[*next];
  return path;
}

std::string firstSleepSlideshowPath() {
  for (const char* dir : {slideshow::SLEEP_SLIDESHOW_DIR, slideshow::SLEEP_SLIDESHOW_DIR_LEGACY}) {
    const auto images = listImageFiles(dir);
    if (images.empty()) continue;
    std::string path = dir;
    path += '/';
    path += images.front();
    return path;
  }
  return {};
}

}  // namespace imageonly
