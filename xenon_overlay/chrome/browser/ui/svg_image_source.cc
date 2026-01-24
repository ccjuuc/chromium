#include "xenon_overlay/chrome/browser/ui/svg_image_source.h"

#include <cmath>

#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkStream.h"
#include "third_party/skia/modules/svg/include/SkSVGDOM.h"
#include "ui/gfx/image/image_skia_rep.h"

SvgImageSource::SvgImageSource(const std::string& source, int size, std::optional<SkColor> color)
    : source_(source), size_(size), color_(color) {}

gfx::ImageSkiaRep SvgImageSource::GetImageForScale(float scale) {
  int px_size = std::ceil(size_ * scale);
  SkBitmap bitmap;
  bitmap.allocN32Pixels(px_size, px_size);
  bitmap.eraseColor(SK_ColorTRANSPARENT);

  SkCanvas canvas(bitmap);
  canvas.scale(scale, scale);

  auto stream = SkMemoryStream::MakeDirect(source_.data(), source_.size());
  auto dom = SkSVGDOM::MakeFromStream(*stream);
  if (dom) {
    dom->setContainerSize(SkSize::Make(size_, size_));
    dom->render(&canvas);
  }

  if (color_.has_value()) {
    canvas.drawColor(color_.value(), SkBlendMode::kSrcIn);
  }

  return gfx::ImageSkiaRep(bitmap, scale);
}
