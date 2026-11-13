// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_native_image_bridge.h"

#include <string>
#include <string_view>
#include <utility>

#include "base/base64.h"
#include "base/containers/span.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkColor.h"
#include "third_party/skia/include/core/SkColorSpace.h"
#include "ui/gfx/codec/jpeg_codec.h"
#include "ui/gfx/codec/png_codec.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_api_bridge.h"

namespace xenon::ipc {
namespace {

class XenonNativeImageBridgeTest : public testing::Test {
 protected:
  void SetUp() override {
    bitmap_.allocN32Pixels(2, 2);
    bitmap_.eraseColor(SK_ColorRED);
    bitmap_.erase(SK_ColorGREEN, SkIRect::MakeXYWH(1, 0, 1, 1));
    bitmap_.erase(SK_ColorBLUE, SkIRect::MakeXYWH(0, 1, 1, 1));
    bitmap_.erase(SkColorSetARGB(128, 0, 255, 0),
                  SkIRect::MakeXYWH(1, 1, 1, 1));
    auto png = gfx::PNGCodec::EncodeBGRASkBitmap(bitmap_, false);
    ASSERT_TRUE(png);
    png_base64_ = base::Base64Encode(*png);
  }

  mojom::IpcResultPtr Decode(const std::string& data) {
    return CallNativeImageApi("nativeImage.decode",
                              base::DictValue().Set("dataBase64", data));
  }

  void ExpectEmpty(mojom::IpcResultPtr result) {
    ASSERT_TRUE(result->success) << result->error;
    ASSERT_TRUE(result->value.is_dict());
    EXPECT_EQ(0, result->value.GetDict().FindInt("width"));
    EXPECT_EQ(0, result->value.GetDict().FindInt("height"));
    ASSERT_TRUE(result->value.GetDict().FindString("pngBase64"));
    EXPECT_TRUE(result->value.GetDict().FindString("pngBase64")->empty());
  }

  void ExpectPixels(const SkBitmap& decoded) {
    ASSERT_FALSE(decoded.isNull());
    ASSERT_EQ(2, decoded.width());
    ASSERT_EQ(2, decoded.height());
    for (int y = 0; y < 2; ++y) {
      for (int x = 0; x < 2; ++x) {
        EXPECT_EQ(bitmap_.getColor(x, y), decoded.getColor(x, y));
      }
    }
  }

  base::test::TaskEnvironment task_environment_;
  SkBitmap bitmap_;
  std::string png_base64_;
};

TEST_F(XenonNativeImageBridgeTest, CanonicalPngRoundTripPreservesRealPixels) {
  auto result = Decode(png_base64_);
  ASSERT_TRUE(result->success) << result->error;
  ASSERT_TRUE(result->value.is_dict());
  EXPECT_EQ(2, result->value.GetDict().FindInt("width"));
  EXPECT_EQ(2, result->value.GetDict().FindInt("height"));
  const auto* encoded = result->value.GetDict().FindString("pngBase64");
  ASSERT_TRUE(encoded);
  std::string decoded;
  ASSERT_TRUE(base::Base64Decode(*encoded, &decoded));
  ExpectPixels(gfx::PNGCodec::Decode(base::as_byte_span(decoded)));
  auto repeated = Decode(*encoded);
  ASSERT_TRUE(repeated->success);
  EXPECT_EQ(result->value, repeated->value);
}

TEST_F(XenonNativeImageBridgeTest, DataUrlsUseRealParsingAndRespectMimeType) {
  auto result = CallNativeImageApi(
      "nativeImage.decode",
      base::DictValue().Set("dataURL", "data:image/png;base64," + png_base64_));
  ASSERT_TRUE(result->success) << result->error;
  EXPECT_EQ(2, result->value.GetDict().FindInt("width"));
  std::string bytes;
  ASSERT_TRUE(base::Base64Decode(png_base64_, &bytes));
  static constexpr std::string_view kHex = "0123456789ABCDEF";
  std::string escaped;
  for (unsigned char byte : bytes) {
    escaped.push_back('%');
    escaped.push_back(kHex[byte >> 4]);
    escaped.push_back(kHex[byte & 15]);
  }
  auto percent_url = CallNativeImageApi(
      "nativeImage.decode",
      base::DictValue().Set("dataURL", "data:image/png," + escaped));
  ASSERT_TRUE(percent_url->success);
  EXPECT_EQ(result->value, percent_url->value);
  ExpectEmpty(CallNativeImageApi(
      "nativeImage.decode",
      base::DictValue().Set("dataURL",
                            "data:image/jpeg;base64," + png_base64_)));
  ExpectEmpty(CallNativeImageApi(
      "nativeImage.decode",
      base::DictValue().Set("dataURL",
                            "data:text/plain;base64," + png_base64_)));
}

TEST_F(XenonNativeImageBridgeTest, JpegDecodeAndEncodeProduceActualJpegImages) {
  SkBitmap opaque;
  opaque.allocN32Pixels(8, 6);
  opaque.eraseColor(SK_ColorBLUE);
  const auto source = gfx::JPEGCodec::Encode(opaque, 95);
  ASSERT_TRUE(source);
  auto image = Decode(base::Base64Encode(*source));
  ASSERT_TRUE(image->success) << image->error;
  EXPECT_EQ(8, image->value.GetDict().FindInt("width"));
  EXPECT_EQ(6, image->value.GetDict().FindInt("height"));
  const auto* png = image->value.GetDict().FindString("pngBase64");
  ASSERT_TRUE(png);
  auto jpeg = CallNativeImageApi(
      "nativeImage.toJPEG",
      base::DictValue().Set("pngBase64", *png).Set("quality", 95));
  ASSERT_TRUE(jpeg->success) << jpeg->error;
  std::string bytes;
  ASSERT_TRUE(base::Base64Decode(jpeg->value.GetString(), &bytes));
  const auto decoded = gfx::JPEGCodec::Decode(base::as_byte_span(bytes));
  ASSERT_FALSE(decoded.isNull());
  EXPECT_EQ(8, decoded.width());
  EXPECT_EQ(6, decoded.height());
  EXPECT_GT(SkColorGetB(decoded.getColor(3, 3)), 245u);
  EXPECT_LT(SkColorGetR(decoded.getColor(3, 3)), 10u);
  EXPECT_LT(SkColorGetG(decoded.getColor(3, 3)), 10u);
  auto data_url = CallNativeImageApi(
      "nativeImage.decode",
      base::DictValue().Set(
          "dataURL", "data:image/jpeg;base64," + jpeg->value.GetString()));
  ASSERT_TRUE(data_url->success);
  EXPECT_EQ(8, data_url->value.GetDict().FindInt("width"));
}

TEST_F(XenonNativeImageBridgeTest, BitmapUsesPackedNativePremultipliedPixels) {
  auto result = CallNativeImageApi(
      "nativeImage.toBitmap", base::DictValue().Set("pngBase64", png_base64_));
  ASSERT_TRUE(result->success) << result->error;
  std::string bytes;
  ASSERT_TRUE(base::Base64Decode(result->value.GetString(), &bytes));
  ASSERT_EQ(16u, bytes.size());
  SkBitmap decoded;
  ASSERT_TRUE(decoded.installPixels(
      SkImageInfo::MakeN32Premul(2, 2, SkColorSpace::MakeSRGB()), bytes.data(),
      8));
  ExpectPixels(decoded);
}

TEST_F(XenonNativeImageBridgeTest,
       InvalidImagesReturnEmptyAndInvalidCallsFail) {
  ExpectEmpty(Decode(""));
  ExpectEmpty(Decode("!invalid base64!"));
  ExpectEmpty(Decode(base::Base64Encode("not an image")));
  ExpectEmpty(Decode(png_base64_.substr(0, 24)));
  ExpectEmpty(CallNativeImageApi(
      "nativeImage.decode",
      base::DictValue().Set("dataURL", "https://example.test/image.png")));
  EXPECT_FALSE(
      CallNativeImageApi("nativeImage.decode", base::DictValue())->success);
  EXPECT_FALSE(CallNativeImageApi(
                   "nativeImage.decode",
                   base::DictValue().Set("dataBase64", "").Set("dataURL", ""))
                   ->success);
  EXPECT_FALSE(
      CallNativeImageApi("nativeImage.resize", base::DictValue())->success);
  EXPECT_FALSE(
      CallNativeImageApi(
          "nativeImage.toJPEG",
          base::DictValue().Set("pngBase64", png_base64_).Set("quality", 101))
          ->success);
  EXPECT_FALSE(
      CallNativeImageApi("nativeImage.toBitmap",
                         base::DictValue().Set("pngBase64", "!invalid!"))
          ->success);
  auto empty = CallNativeImageApi("nativeImage.toBitmap",
                                  base::DictValue().Set("pngBase64", ""));
  ASSERT_TRUE(empty->success);
  EXPECT_TRUE(empty->value.GetString().empty());
}

TEST_F(XenonNativeImageBridgeTest,
       RejectsOversizedDimensionsBeforeDecodingPixels) {
  // A valid PNG header declaring 4097 x 4096 pixels followed by tiny image
  // data. The size error confirms the header is inspected before pixel
  // allocation.
  auto result = Decode(
      "iVBORw0KGgoAAAANSUhEUgAAEAEAABAACAYAAAAdYU8pAAAAC0lEQVR4nGNgAAIAAAUAAXpe"
      "qz8AAAAASUVORK5CYII=");
  EXPECT_FALSE(result->success);
  EXPECT_EQ("ERR_BUFFER_TOO_LARGE: decoded image exceeds 16 megapixels",
            result->error);
}

TEST_F(XenonNativeImageBridgeTest,
       ElectronDispatcherRoutesMainAndRendererCalls) {
  auto request = base::DictValue()
                     .Set("operation", "nativeImage.decode")
                     .Set("dataBase64", png_base64_);
  base::test::TestFuture<mojom::IpcResultPtr> main;
  CallElectronApi(base::Value(request.Clone()), main.GetCallback());
  auto main_result = main.Take();
  ASSERT_TRUE(main_result->success) << main_result->error;
  base::test::TestFuture<mojom::IpcResultPtr> renderer;
  CallElectronApi(base::Value(base::ListValue().Append(std::move(request))),
                  renderer.GetCallback());
  auto renderer_result = renderer.Take();
  ASSERT_TRUE(renderer_result->success) << renderer_result->error;
  EXPECT_EQ(main_result->value, renderer_result->value);
  EXPECT_EQ(2, renderer_result->value.GetDict().FindInt("width"));
}

}  // namespace
}  // namespace xenon::ipc
