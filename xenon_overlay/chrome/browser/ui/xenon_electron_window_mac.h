// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_UI_XENON_ELECTRON_WINDOW_MAC_H_
#define XENON_OVERLAY_CHROME_BROWSER_UI_XENON_ELECTRON_WINDOW_MAC_H_

#include <string>

namespace views {
class Widget;
}

namespace xenon {

bool SetMacWindowShadow(views::Widget* widget, bool enabled, std::string* error);
bool GetMacWindowShadow(views::Widget* widget, bool* enabled, std::string* error);
bool ConfigureMacWindowTitleBar(views::Widget* widget,
                                const std::string& style,
                                std::string* error);
bool SetMacWindowButtonVisibility(views::Widget* widget,
                                  bool visible,
                                  std::string* error);

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_UI_XENON_ELECTRON_WINDOW_MAC_H_
