// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_electron_app_mac.h"

#import <Foundation/Foundation.h>
#import <ServiceManagement/ServiceManagement.h>

#include <utility>

#include "base/apple/bundle_locations.h"
#include "base/apple/foundation_util.h"
#include "base/check.h"
#include "base/mac/mac_util.h"

namespace xenon::ipc {
namespace {

mojom::IpcResultPtr Failure(const char* error) {
  auto result = mojom::IpcResult::New();
  result->success = false;
  result->error = error;
  return result;
}

}  // namespace

void GetElectronLoginItemSettings(const base::DictValue& arguments,
                                  ElectronApiCallback callback) {
  CHECK([NSThread isMainThread]);
  if (const base::Value* options = arguments.Find("options")) {
    if (!options->is_dict()) {
      std::move(callback).Run(Failure(
          "ERR_INVALID_ARG_TYPE: login item options must be an object"));
      return;
    }
    if (!options->GetDict().empty()) {
      std::move(callback).Run(Failure(
          "ERR_NOT_SUPPORTED: login item query options are not supported on macOS"));
      return;
    }
  }
  // Never let a hosted application choose another executable or substitute its
  // embedded Electron bundle for the browser that will actually launch it.
  if (!base::apple::AmIBundled()) {
    std::move(callback).Run(Failure(
        "ERR_NOT_SUPPORTED: login item queries require a bundled browser"));
    return;
  }
  const auto outer_bundle = base::apple::OuterBundlePath();
  auto settings = base::mac::GetLoginItemSettings(outer_bundle);
  if (!settings) {
    std::move(callback).Run(Failure(
        "ERR_SYSTEM_ERROR: unable to read the macOS login item list"));
    return;
  }
  // Electron 22 and older registrations use the legacy Login Items list.
  // Also recognize modern registration of this actual main app. A shortcut's
  // main bundle is a different app and must not affect the outer app's result.
  if (@available(macOS 13.0, *)) {
    if (base::apple::MainBundlePath() == outer_bundle &&
        SMAppService.mainAppService.status == SMAppServiceStatusEnabled) {
      settings->open_at_login = true;
    }
  }
  auto result = mojom::IpcResult::New();
  result->success = true;
  result->value = base::Value(
      base::DictValue()
          .Set("openAtLogin", settings->open_at_login)
          .Set("openAsHidden", settings->open_as_hidden)
          .Set("wasOpenedAtLogin", base::mac::WasLaunchedAsLoginOrResumeItem())
          .Set("wasOpenedAsHidden", base::mac::WasLaunchedAsHiddenLoginItem())
          .Set("restoreState", base::mac::WasLaunchedAsLoginItemRestoreState()));
  std::move(callback).Run(std::move(result));
}

}  // namespace xenon::ipc
