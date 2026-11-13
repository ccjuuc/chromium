// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
#ifndef XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_LOGIN_ITEMS_H_
#define XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_LOGIN_ITEMS_H_

#include <string>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/values.h"

namespace xenon::ipc {

// Browser-owned application identity. Never populate these fields from the
// application-supplied options or from its emulated process.execPath.
struct ElectronLoginItemContext {
  base::FilePath application_path;
  base::FilePath launch_path;
  base::FilePath executable_path;
  base::FilePath user_data_directory;
};

using ElectronLoginItemReply =
    base::OnceCallback<void(base::Value, std::string)>;

// arguments = {method: "get" | "set", options: {...}}. Registry work runs on
// a blocking worker; the reply returns to the calling sequence.
void CallElectronLoginItemSettings(ElectronLoginItemContext context,
                                   base::Value arguments,
                                   ElectronLoginItemReply reply);

}  // namespace xenon::ipc
#endif  // XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_LOGIN_ITEMS_H_
