// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_electron_shell.h"

#include <utility>

namespace xenon::ipc {

void CallElectronShell(base::DictValue arguments,
                       ElectronApiCallback callback) {
  auto result = mojom::IpcResult::New();
  result->success = false;
  result->error =
      "ERR_NOT_SUPPORTED: Electron shell is not implemented on this platform";
  std::move(callback).Run(std::move(result));
}

}  // namespace xenon::ipc
