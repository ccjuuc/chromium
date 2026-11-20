// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_APP_CONFIG_H_
#define XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_APP_CONFIG_H_

#include <memory>
#include <string>

#include "base/files/file_path.h"
#include "base/types/expected.h"
#include "xenon_overlay/public/mojom/xenon_ipc.mojom.h"

namespace xenon {
class XenonManager;
namespace asar {
class PreparedArchivePublicKeys;
}
}  // namespace xenon

namespace xenon::ipc {

// A fully resolved disk application and its validated ASAR keys. Only the
// preparation function can construct this result; the manager can consume it
// without repeating filesystem work on the UI sequence.
class PreparedElectronAppConfig {
 public:
  ~PreparedElectronAppConfig();

  PreparedElectronAppConfig(const PreparedElectronAppConfig&) = delete;
  PreparedElectronAppConfig& operator=(const PreparedElectronAppConfig&) =
      delete;

  // Mojo StructPtr accessors are not deeply const. Expose only an independent
  // snapshot so callers cannot change fields after their validation.
  mojom::IpcMainConfigPtr CloneConfig() const { return config_.Clone(); }

 private:
  friend class xenon::XenonManager;
  friend base::expected<std::unique_ptr<PreparedElectronAppConfig>, std::string>
  PrepareElectronAppConfig(const base::FilePath& path,
                           const std::string& container_id);

  PreparedElectronAppConfig(
      mojom::IpcMainConfigPtr config,
      std::unique_ptr<asar::PreparedArchivePublicKeys> archive_keys);

  mojom::IpcMainConfigPtr config_;
  std::unique_ptr<asar::PreparedArchivePublicKeys> archive_keys_;
};

// Run on a MayBlock sequence after browser initialization. Resolves descriptor,
// application, executable, host paths and ASAR keys without retaining a key
// registration or modifying the application. The result can cross sequences.
base::expected<std::unique_ptr<PreparedElectronAppConfig>, std::string>
PrepareElectronAppConfig(const base::FilePath& path,
                         const std::string& container_id);

// Accepts an application directory/archive or an external host JSON descriptor.
// Resolves metadata without rewriting or extracting any application resources.
bool LoadElectronAppConfig(const base::FilePath& path,
                           const std::string& container_id,
                           mojom::IpcMainConfigPtr* config,
                           std::string* error);

// Pure path resolution: no filesystem access or directory migration. Explicit
// storage_id wins, then a stable named container; direct default-container
// launches fall back to the selected installation path. Names/versions do not
// participate. The host path remains unchanged in the configuration.
base::expected<base::FilePath, std::string>
ResolveElectronAppStorageUserDataPath(const mojom::IpcMainConfig& config);

}  // namespace xenon::ipc

#endif  // XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_APP_CONFIG_H_
