// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_native_image_bridge.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/containers/span.h"
#include "net/base/data_url.h"
#include "third_party/skia/include/codec/SkCodec.h"
#include "third_party/skia/include/codec/SkEncodedImageFormat.h"
#include "third_party/skia/include/codec/SkJpegDecoder.h"
#include "third_party/skia/include/codec/SkPngRustDecoder.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkColorSpace.h"
#include "third_party/skia/include/core/SkStream.h"
#include "ui/gfx/codec/jpeg_codec.h"
#include "ui/gfx/codec/png_codec.h"
#include "url/gurl.h"

namespace xenon::ipc {
namespace {

constexpr size_t kMaxEncodedBytes = 32 * 1024 * 1024;
constexpr uint64_t kMaxPixels = 16 * 1024 * 1024;

mojom::IpcResultPtr Success(base::Value value) {
  auto result = mojom::IpcResult::New();
  result->success = true;
  result->value = std::move(value);
  return result;
}

mojom::IpcResultPtr Failure(std::string error) {
  auto result = mojom::IpcResult::New();
  result->success = false;
  result->error = std::move(error);
  return result;
}

mojom::IpcResultPtr EmptyImage() {
  return Success(base::Value(
      base::DictValue().Set("pngBase64", "").Set("width", 0).Set("height", 0)));
}

SkBitmap DecodeImage(const std::string& bytes,
                     bool allow_png,
                     bool allow_jpeg,
                     std::string* error) {
  if (bytes.size() > kMaxEncodedBytes) {
    *error = "ERR_BUFFER_TOO_LARGE: encoded image exceeds 32 MiB";
    return {};
  }
  if (bytes.empty()) {
    return {};
  }
  // Use explicit decoder factories, like gfx::PNGCodec/JPEGCodec. Chromium
  // disables Skia's legacy registry initialization, and callers must not depend
  // on some unrelated component having registered global decoders first.
  std::unique_ptr<SkCodec> codec;
  SkCodec::Result result = SkCodec::kInvalidInput;
  auto stream = std::make_unique<SkMemoryStream>(bytes.data(), bytes.size(),
                                               /*copyData=*/false);
  if (allow_png && SkPngRustDecoder::IsPng(bytes.data(), bytes.size())) {
    codec = SkPngRustDecoder::Decode(std::move(stream), &result);
  } else if (allow_jpeg && SkJpegDecoder::IsJpeg(bytes.data(), bytes.size())) {
    codec = SkJpegDecoder::Decode(std::move(stream), &result);
  }
  if (!codec || result != SkCodec::kSuccess) {
    return {};
  }
  const auto format = codec->getEncodedFormat();
  if ((!allow_png || format != SkEncodedImageFormat::kPNG) &&
      (!allow_jpeg || format != SkEncodedImageFormat::kJPEG)) {
    return {};
  }
  const auto dimensions = codec->getInfo().dimensions();
  if (dimensions.width() <= 0 || dimensions.height() <= 0) {
    return {};
  }
  // Check decoded size before asking a codec to allocate the pixel buffer.
  if (static_cast<uint64_t>(dimensions.width()) * dimensions.height() >
      kMaxPixels) {
    *error = "ERR_BUFFER_TOO_LARGE: decoded image exceeds 16 megapixels";
    return {};
  }
  return format == SkEncodedImageFormat::kPNG
             ? gfx::PNGCodec::Decode(base::as_byte_span(bytes))
             : gfx::JPEGCodec::Decode(base::as_byte_span(bytes));
}

mojom::IpcResultPtr Decode(const base::DictValue& arguments) {
  const auto* encoded = arguments.FindString("dataBase64");
  const auto* data_url = arguments.FindString("dataURL");
  if ((!encoded && !data_url) || (encoded && data_url)) {
    return Failure(
        "ERR_INVALID_ARG_TYPE: image requires one dataBase64 or dataURL "
        "string");
  }
  std::string bytes;
  bool allow_png = true;
  bool allow_jpeg = true;
  if (data_url) {
    if (data_url->size() > kMaxEncodedBytes * 4) {
      return Failure("ERR_BUFFER_TOO_LARGE: image data URL exceeds limit");
    }
    std::string mime;
    std::string charset;
    if (!net::DataURL::Parse(GURL(*data_url), &mime, &charset, &bytes)) {
      return EmptyImage();
    }
    allow_png = mime == "image/png";
    allow_jpeg = mime == "image/jpeg";
  } else {
    if (encoded->size() > ((kMaxEncodedBytes + 2) / 3) * 4) {
      return Failure("ERR_BUFFER_TOO_LARGE: encoded image exceeds 32 MiB");
    }
    if (!base::Base64Decode(*encoded, &bytes)) {
      return EmptyImage();
    }
  }
  std::string error;
  const SkBitmap bitmap = DecodeImage(bytes, allow_png, allow_jpeg, &error);
  if (!error.empty()) {
    return Failure(std::move(error));
  }
  if (bitmap.isNull()) {
    return EmptyImage();
  }
  const auto png = gfx::PNGCodec::EncodeBGRASkBitmap(bitmap, false);
  if (!png) {
    return Failure("ERR_OPERATION_FAILED: unable to encode image as PNG");
  }
  // The canonical representation must remain accepted by later conversions.
  // A small compressed JPEG can expand beyond the encoded-input limit as PNG.
  if (png->size() > kMaxEncodedBytes) {
    return Failure("ERR_BUFFER_TOO_LARGE: canonical PNG exceeds 32 MiB");
  }
  return Success(base::Value(base::DictValue()
                                 .Set("pngBase64", base::Base64Encode(*png))
                                 .Set("width", bitmap.width())
                                 .Set("height", bitmap.height())));
}

}  // namespace

mojom::IpcResultPtr CallNativeImageApi(const std::string& operation,
                                       const base::DictValue& arguments) {
  if (operation == "nativeImage.decode") {
    return Decode(arguments);
  }
  if (operation != "nativeImage.toJPEG" &&
      operation != "nativeImage.toBitmap") {
    return Failure("ERR_NOT_SUPPORTED: unsupported Electron API " + operation);
  }
  const auto* encoded = arguments.FindString("pngBase64");
  if (!encoded) {
    return Failure("ERR_INVALID_ARG_TYPE: pngBase64 must be a string");
  }
  int quality = 0;
  if (operation == "nativeImage.toJPEG") {
    const auto requested_quality = arguments.FindInt("quality");
    if (!requested_quality || *requested_quality < 0 ||
        *requested_quality > 100) {
      return Failure(
          "ERR_INVALID_ARG_VALUE: JPEG quality must be an integer from 0 to "
          "100");
    }
    quality = *requested_quality;
  }
  if (encoded->empty()) {
    return Success(base::Value(""));
  }
  if (encoded->size() > ((kMaxEncodedBytes + 2) / 3) * 4) {
    return Failure("ERR_BUFFER_TOO_LARGE: encoded image exceeds 32 MiB");
  }
  std::string bytes;
  if (!base::Base64Decode(*encoded, &bytes)) {
    return Failure("ERR_INVALID_ARG_VALUE: image PNG is not valid base64");
  }
  std::string error;
  const SkBitmap bitmap = DecodeImage(bytes, true, false, &error);
  if (!error.empty()) {
    return Failure(std::move(error));
  }
  if (bitmap.isNull()) {
    return Failure("ERR_INVALID_ARG_VALUE: image is not a valid PNG");
  }
  if (operation == "nativeImage.toJPEG") {
    const auto jpeg = gfx::JPEGCodec::Encode(bitmap, quality);
    return jpeg ? Success(base::Value(base::Base64Encode(*jpeg)))
                : Failure(
                      "ERR_OPERATION_FAILED: unable to encode image as JPEG");
  }
  // Match Electron's packed native N32 premultiplied sRGB bitmap layout.
  const auto info =
      SkImageInfo::MakeN32Premul(bitmap.dimensions(), SkColorSpace::MakeSRGB());
  std::vector<uint8_t> pixels(info.computeMinByteSize());
  if (!bitmap.readPixels(info, pixels.data(), info.minRowBytes(), 0, 0)) {
    return Failure("ERR_OPERATION_FAILED: unable to read image pixels");
  }
  return Success(base::Value(base::Base64Encode(pixels)));
}

}  // namespace xenon::ipc
