// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_APP_STORAGE_H_
#define XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_APP_STORAGE_H_

#include <memory>
#include <optional>
#include <string>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/types/expected.h"
#include "base/values.h"
#include "content/public/browser/storage_partition_config.h"

namespace content {
class BrowserContext;
}

namespace xenon::ipc {

using ElectronAppStorageContextCreated =
    base::OnceCallback<void(std::unique_ptr<content::BrowserContext>,
                            std::string)>;
using ElectronAppStorageContextFactory =
    base::RepeatingCallback<void(const base::FilePath&,
                                 ElectronAppStorageContextCreated)>;
using ElectronAppStorageContextDestroyer =
    base::RepeatingCallback<void(std::unique_ptr<content::BrowserContext>)>;

// All entry points run on Browser UI. The owner is the authenticated service
// generation, never a value supplied by application JavaScript.
base::expected<void, std::string> RegisterElectronAppStorage(
    content::BrowserContext* host,
    const std::string& container,
    const std::string& owner,
    const base::FilePath& initial_user_data,
    ElectronAppStorageContextFactory factory = {},
    ElectronAppStorageContextDestroyer destroyer = {});

// Does not instantiate or freeze storage. An unredirected/uninitialized owner
// returns its host context; a retired owner returns nullptr.
content::BrowserContext* PeekElectronAppStorageContext(
    const std::string& owner);
base::FilePath GetElectronAppStorageUserDataPath(const std::string& owner);

// Creates the initial userData on a MayBlock worker before main JavaScript can
// run. Does not instantiate/freeze session storage or move existing files.
// Completion is posted and rejects owners retired while the work was pending.
void PrepareElectronAppStorageUserData(
    const std::string& owner,
    base::OnceCallback<void(std::string)> callback);

using ElectronAppStorageReady =
    base::OnceCallback<void(content::BrowserContext*, std::string)>;
// Freezes the effective session path and resolves an optional independent
// Profile. Completion is always posted, including the legacy host path.
void EnsureElectronAppStorageContext(const std::string& owner,
                                     ElectronAppStorageReady callback);

using ElectronAppStoragePathReply =
    base::OnceCallback<void(base::Value, std::string)>;
// {method:"get"|"set",name:"userData"|"sessionData",path?:string}.
// Replies with the effective path string. A failed set never changes state.
void CallElectronAppStoragePath(const std::string& owner,
                                base::Value arguments,
                                ElectronAppStoragePathReply callback);

// First stop owner requests and close its windows/views, then retire. The
// Profile and its directory lease survive until dependent renderers shut down.
void RetireElectronAppStorage(const std::string& owner);

// Returns a config only for the independent Profile owned by this container.
// Its default session uses the actual Profile root; legacy host sessions keep
// their existing partition config and existing data without migration.
std::optional<content::StoragePartitionConfig>
GetElectronAppStorageSessionConfig(content::BrowserContext* context,
                                   const std::string& container,
                                   const std::string& partition);

// Tests substitute context construction/destruction, preserving real path
// validation, leases and the production owner state machine.
void SetElectronAppStorageContextFactoryForTesting(
    ElectronAppStorageContextFactory factory,
    ElectronAppStorageContextDestroyer destroyer);

}  // namespace xenon::ipc

#endif  // XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_APP_STORAGE_H_
