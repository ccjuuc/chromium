// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_electron_user_agent.h"

#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "net/http/http_util.h"
#include "xenon_overlay/common/ipc/xenon_runtime_platform.h"

namespace xenon::ipc {

std::string BuildElectronUserAgent(std::string_view chromium_user_agent,
                                   std::string_view app_name,
                                   std::string_view app_version) {
  std::string result(chromium_user_agent);
  // Electron removes spaces from the display name for its product token. Other
  // invalid characters must not turn package metadata into HTTP header syntax.
  std::string product_name;
  base::RemoveChars(app_name, " ", &product_name);
  if (product_name != "Electron" && net::HttpUtil::IsToken(product_name) &&
      net::HttpUtil::IsToken(app_version)) {
    const std::string product = base::StrCat({product_name, "/", app_version});
    const size_t chrome = result.find(" Chrome/");
    if (chrome == std::string::npos) {
      base::StrAppend(&result, {" ", product});
    } else {
      result.insert(chrome, " " + product);
    }
  }
  // Keep Mozilla at the start and use the same explicit compatibility version
  // exposed by process.versions.electron; this is not a vendor Electron build.
  const std::string runtime =
      base::StrCat({" Electron/", kElectronCompatibilityVersion});
  const size_t safari = result.find(" Safari/");
  if (safari == std::string::npos) {
    result.append(runtime);
  } else {
    result.insert(safari, runtime);
  }
  return result;
}

}  // namespace xenon::ipc
