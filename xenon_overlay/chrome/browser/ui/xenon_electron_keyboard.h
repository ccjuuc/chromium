// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_UI_XENON_ELECTRON_KEYBOARD_H_
#define XENON_OVERLAY_CHROME_BROWSER_UI_XENON_ELECTRON_KEYBOARD_H_

#include <cstdint>
#include <string>

namespace content {
class WebContents;
}

namespace xenon {

// Used by top-level BrowserWindows and independently hosted WebContents alike.
// Document replacement cancels old input; listener transitions retain FIFO.
void ConfigureElectronKeyboardInput(content::WebContents* contents,
                                    const std::string& container,
                                    int32_t native_id,
                                    bool observe,
                                    bool cancel_pending = true);

}  // namespace xenon
#endif  // XENON_OVERLAY_CHROME_BROWSER_UI_XENON_ELECTRON_KEYBOARD_H_
