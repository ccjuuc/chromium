// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_zoom.h"

#include <cmath>
#include <limits>
#include <memory>

#include "base/supports_user_data.h"
#include "content/public/browser/host_zoom_map.h"
#include "content/public/browser/web_contents.h"
#include "third_party/blink/public/common/web_preferences/web_preferences.h"

namespace xenon::ipc {
bool CallElectronPageZoom(content::WebContents* contents,
                          const std::string& command,
                          const base::Value& arguments,
                          base::Value* result,
                          std::string* error) {
  if (!contents || contents->IsBeingDestroyed()) {
    *error = "ERR_ABORTED: WebContents has been destroyed";
    return false;
  }
  if (command == "get-zoom-level") {
    *result = base::Value(content::HostZoomMap::GetZoomLevel(contents));
    return true;
  }
  const auto* options = arguments.GetIfDict();
  const auto level = options ? options->FindDouble("level") : std::nullopt;
  if (command != "set-zoom-level" || !level || !std::isfinite(*level)) {
    *error = "ERR_INVALID_ARG_VALUE: zoom level must be a finite number";
    return false;
  }
  content::HostZoomMap::SetZoomLevel(contents, *level);
  return true;
}
namespace {
const char kZoomKey[] = "xenon-electron-visual-zoom";
struct VisualZoomLimits : base::SupportsUserData::Data {
  float minimum;
  float maximum;
};
}  // namespace
bool ApplyElectronVisualZoomLimits(
    content::WebContents* contents,
    blink::web_pref::WebPreferences* preferences) {
  const auto* limits =
      static_cast<VisualZoomLimits*>(contents->GetUserData(kZoomKey));
  if (!limits) {
    return false;
  }
  const bool changed =
      preferences->default_minimum_page_scale_factor != limits->minimum ||
      preferences->default_maximum_page_scale_factor != limits->maximum;
  preferences->default_minimum_page_scale_factor = limits->minimum;
  preferences->default_maximum_page_scale_factor = limits->maximum;
  return changed;
}
bool SetElectronVisualZoomLimits(content::WebContents* contents,
                                 const base::Value& arguments,
                                 std::string* error) {
  const auto* options = arguments.GetIfDict();
  const auto minimum = options ? options->FindDouble("minimum") : std::nullopt;
  const auto maximum = options ? options->FindDouble("maximum") : std::nullopt;
  if (!minimum || !maximum || !std::isfinite(*minimum) ||
      !std::isfinite(*maximum) ||
      *minimum < std::numeric_limits<float>::min() || *maximum < *minimum ||
      *maximum > std::numeric_limits<float>::max()) {
    *error =
        "ERR_INVALID_ARG_VALUE: visual zoom limits must be positive finite "
        "numbers in increasing order";
    return false;
  }
  if (!contents || contents->IsBeingDestroyed()) {
    *error = "ERR_ABORTED: WebContents has been destroyed";
    return false;
  }
  auto limits = std::make_unique<VisualZoomLimits>();
  limits->minimum = static_cast<float>(*minimum);
  limits->maximum = static_cast<float>(*maximum);
  contents->SetUserData(kZoomKey, std::move(limits));
  auto preferences = contents->GetOrCreateWebPreferences();
  ApplyElectronVisualZoomLimits(contents, &preferences);
  contents->SetWebPreferences(preferences);
  return true;
}
}  // namespace xenon::ipc
