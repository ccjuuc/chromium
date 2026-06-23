// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_XENON_CHROME_BROWSER_UI_VIEWS_XENON_TOAST_H_
#define XENON_OVERLAY_XENON_CHROME_BROWSER_UI_VIEWS_XENON_TOAST_H_

#include <string>

#include "base/functional/callback.h"
#include "base/time/time.h"
#include "ui/gfx/native_ui_types.h"

namespace views {
class Widget;
}

namespace xunlei {

// Helper class for displaying temporary toast notifications.
class XenonToast {
 public:
  enum class Type { kSuccess, kInfo, kError, kWarning, kLoading };

  struct Params {
    Params();
    ~Params();
    Params(Params&&);
    Params& operator=(Params&&);

    Type type = Type::kInfo;
    std::u16string text;
    std::u16string action_text;
    base::OnceClosure action_callback;
    base::TimeDelta duration;  // 0 uses default based on type
  };

  XenonToast() = delete;
  XenonToast(const XenonToast&) = delete;
  XenonToast& operator=(const XenonToast&) = delete;

  // Shows or updates a toast in the parent window.
  static views::Widget* Show(gfx::NativeWindow parent, Params params);
};

}  // namespace xunlei

#endif  // XENON_OVERLAY_XENON_CHROME_BROWSER_UI_VIEWS_XENON_TOAST_H_
