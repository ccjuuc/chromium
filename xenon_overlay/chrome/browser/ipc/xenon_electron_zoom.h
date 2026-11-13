// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
#ifndef XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_ZOOM_H_
#define XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_ZOOM_H_

#include <string>

#include "base/values.h"
namespace content {
class WebContents;
}
namespace blink::web_pref {
struct WebPreferences;
}
namespace xenon::ipc {
bool CallElectronPageZoom(content::WebContents* contents,
                          const std::string& command,
                          const base::Value& arguments,
                          base::Value* result,
                          std::string* error);
bool SetElectronVisualZoomLimits(content::WebContents* contents,
                                 const base::Value& arguments,
                                 std::string* error);
// Reapplies the WebContents-owned setting when Chromium refreshes preferences.
bool ApplyElectronVisualZoomLimits(
    content::WebContents* contents,
    blink::web_pref::WebPreferences* preferences);
}  // namespace xenon::ipc
#endif
