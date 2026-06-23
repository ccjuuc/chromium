// Copyright 2026 The Xunlei Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/xenon/chrome/browser/fonts/xenon_font_loader.h"

#include "base/files/file_path.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "build/build_config.h"
#include "skia/ext/font_utils.h"
#include "third_party/skia/include/core/SkData.h"
#include "third_party/skia/include/core/SkFontMgr.h"
#include "third_party/skia/include/core/SkTypeface.h"
#include "xenon_overlay/xenon/chrome/browser/fonts/xenon_font_embedded_data.h"
#include <string_view>

namespace xenon_fonts {

namespace {

// Predefined font families
constexpr std::string_view kDdinFontFamily = "D-DIN";
constexpr std::string_view kShanHaiFontFamilyLower = "shanhaitongmengleyuan55w";
constexpr std::string_view kShanHaiFontFamilyCamel = "ShanHaiTongMengLeYuan55W";

// Predefined font filenames
constexpr std::string_view kDdinRegularFilename = "D-DIN.ttf";
constexpr std::string_view kDdinBoldFilename = "D-DIN-Bold.ttf";
constexpr std::string_view kShanHaiFilename = "shanhaitongmengleyuan55w.ttf";

sk_sp<SkTypeface>& GetRegularTypefaceRef() {
  static base::NoDestructor<sk_sp<SkTypeface>> typeface;
  return *typeface;
}

sk_sp<SkTypeface>& GetBoldTypefaceRef() {
  static base::NoDestructor<sk_sp<SkTypeface>> typeface;
  return *typeface;
}

sk_sp<SkTypeface>& GetShanHaiTypefaceRef() {
  static base::NoDestructor<sk_sp<SkTypeface>> typeface;
  return *typeface;
}

struct FontEntry {
  std::string_view family_name;
  std::string_view filename;
  bool is_bold;
  const uint8_t* embedded_data;
  size_t embedded_size;
  sk_sp<SkTypeface>& (*get_typeface_ref)();
};

const FontEntry kFontEntries[] = {
    {kDdinFontFamily, kDdinRegularFilename, false, kXenonFontDdinRegular,
     kXenonFontDdinRegularSize, GetRegularTypefaceRef},
    {kDdinFontFamily, kDdinBoldFilename, true, kXenonFontDdinBold,
     kXenonFontDdinBoldSize, GetBoldTypefaceRef},
    {kShanHaiFontFamilyLower, kShanHaiFilename, false, nullptr, 0,
     GetShanHaiTypefaceRef},
    {kShanHaiFontFamilyCamel, kShanHaiFilename, false, nullptr, 0,
     GetShanHaiTypefaceRef},
};

bool g_fonts_registered = false;

sk_sp<SkTypeface> LoadTypefaceFromMemory(const uint8_t* data, size_t size) {
  if (!data || !size) {
    return nullptr;
  }

  sk_sp<SkData> font_data = SkData::MakeWithCopy(data, size);
  if (!font_data) {
    return nullptr;
  }

  sk_sp<SkFontMgr> font_manager = skia::DefaultFontMgr();
  if (!font_manager) {
    return nullptr;
  }

  return font_manager->makeFromData(std::move(font_data), 0);
}

sk_sp<SkTypeface> LoadTypefaceFromFile(std::string_view filename) {
  base::FilePath exe_dir;
  if (!base::PathService::Get(base::DIR_EXE, &exe_dir)) {
    return nullptr;
  }
  base::FilePath font_path = exe_dir.AppendASCII("fonts").Append(
      base::FilePath::FromUTF8Unsafe(filename));
  sk_sp<SkFontMgr> font_manager = skia::DefaultFontMgr();
  if (!font_manager) {
    return nullptr;
  }
  return font_manager->makeFromFile(font_path.AsUTF8Unsafe().c_str(), 0);
}

sk_sp<SkTypeface> GetSkiaCustomFont(const char* name, SkFontStyle style) {
  if (!name) {
    return nullptr;
  }

  std::string_view name_view(name);
  bool requested_bold = style.weight() >= SkFontStyle::kSemiBold_Weight;

  for (const auto& entry : kFontEntries) {
    if (entry.family_name == name_view && entry.is_bold == requested_bold) {
      sk_sp<SkTypeface>& cached_face = entry.get_typeface_ref();
      if (!cached_face) {
        if (entry.embedded_size > 0 && entry.embedded_data) {
          cached_face =
              LoadTypefaceFromMemory(entry.embedded_data, entry.embedded_size);
        } else {
          cached_face = LoadTypefaceFromFile(entry.filename);
        }
        if (!cached_face) {
          LOG(ERROR) << "Failed to load custom font: " << entry.family_name
                     << " (file: " << entry.filename << ")";
        }
      }
      return cached_face;
    }
  }
  return nullptr;
}

}  // namespace

bool RegisterXunleiFonts() {
  if (g_fonts_registered) {
    return true;
  }

  skia::SetGetCustomFontCallback(GetSkiaCustomFont);
  g_fonts_registered = true;
  return true;
}

void UnregisterXunleiFonts() {
  skia::SetGetCustomFontCallback(nullptr);
  for (const auto& entry : kFontEntries) {
    entry.get_typeface_ref() = nullptr;
  }
  g_fonts_registered = false;
}

}  // namespace xenon_fonts
