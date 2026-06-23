// Copyright 2026 The Xunlei Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_XENON_CHROME_BROWSER_FONTS_XENON_FONT_LOADER_H_
#define XENON_OVERLAY_XENON_CHROME_BROWSER_FONTS_XENON_FONT_LOADER_H_

#include "third_party/skia/include/core/SkRefCnt.h"

class SkTypeface;

namespace xenon_fonts {

// Loads bundled D-DIN fonts for Views/Skia. Safe to call multiple times.
bool RegisterXunleiFonts();

// Removes fonts registered by RegisterXunleiFonts().
void UnregisterXunleiFonts();

}  // namespace xenon_fonts

#endif  // XENON_OVERLAY_XENON_CHROME_BROWSER_FONTS_XENON_FONT_LOADER_H_
