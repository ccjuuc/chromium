#ifndef XENON_OVERLAY_CHROME_BROWSER_UI_SVG_IMAGE_SOURCE_H_
#define XENON_OVERLAY_CHROME_BROWSER_UI_SVG_IMAGE_SOURCE_H_

#include <string>
#include <optional>

#include "ui/gfx/image/image_skia_source.h"
#include "third_party/skia/include/core/SkColor.h"

class SvgImageSource : public gfx::ImageSkiaSource {
 public:
  SvgImageSource(const std::string& source, int size, std::optional<SkColor> color);
  ~SvgImageSource() override = default;

  // gfx::ImageSkiaSource:
  gfx::ImageSkiaRep GetImageForScale(float scale) override;

 private:
  std::string source_;
  int size_;
  std::optional<SkColor> color_;
};

#endif  // XENON_OVERLAY_CHROME_BROWSER_UI_SVG_IMAGE_SOURCE_H_
