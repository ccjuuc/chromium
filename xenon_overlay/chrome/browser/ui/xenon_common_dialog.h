// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_UI_XENON_COMMON_DIALOG_H_
#define XENON_OVERLAY_CHROME_BROWSER_UI_XENON_COMMON_DIALOG_H_

#include <string>

#include "base/functional/callback.h"
#include "ui/gfx/native_ui_types.h"

namespace xenon {

// Helper class for constructing and showing generic common native dialogs.
class XenonCommonDialog {
 public:
  enum class Style { kSmall, kMedium };

  struct Result {
    bool accepted = false;
    bool checkbox_checked = false;
  };

  using Callback = base::OnceCallback<void(const Result& result)>;

  XenonCommonDialog() = delete;
  XenonCommonDialog(const XenonCommonDialog&) = delete;
  XenonCommonDialog& operator=(const XenonCommonDialog&) = delete;

  // Static method to construct and display the dialog.
  static void Show(gfx::NativeWindow parent,
                   Style style,
                   const std::u16string& title,
                   const std::u16string& body_text,
                   const std::u16string& checkbox_text,
                   bool checkbox_checked,
                   const std::u16string& cancel_text,
                   const std::u16string& confirm_text,
                   Callback callback,
                   bool show_mask = true);
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_UI_XENON_COMMON_DIALOG_H_
