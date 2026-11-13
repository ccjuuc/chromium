// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
#ifndef XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_SESSION_BRIDGE_H_
#define XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_SESSION_BRIDGE_H_

#include <optional>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/values.h"
#include "content/public/browser/storage_partition_config.h"

namespace content {
class BrowserContext;
class SiteInstance;
class StoragePartition;
}  // namespace content
class GURL;

namespace xenon::ipc {

struct ElectronSessionIdentity {
  std::string container_id;
  std::string partition;
};

struct ElectronSessionPreloadScript {
  std::string id;
  std::string type;
  std::string file_path;
  std::string source;
};

// All calls are Browser UI sequence only. Every Electron session, including
// a default session, is isolated by its container partition or dedicated
// BrowserContext. A dedicated context's default session uses its default
// StoragePartition.
content::StoragePartitionConfig GetElectronSessionPartitionConfig(
    content::BrowserContext* context,
    const std::string& container_id,
    const std::string& partition);
// Selects the matching SiteInstance factory: Chromium's fixed-partition factory
// only accepts non-default partitions.
scoped_refptr<content::SiteInstance> CreateElectronSessionSiteInstance(
    content::BrowserContext* context,
    const std::string& container_id,
    const std::string& partition,
    const GURL& url);
// Returns null after BrowserContext shutdown has begun; shutdown must not
// recreate sessions or retain storage partitions that are about to be freed.
content::StoragePartition* ResolveElectronSessionPartition(
    content::BrowserContext* context,
    const std::string& container_id,
    const std::string& partition);
std::optional<ElectronSessionIdentity> FindElectronSessionIdentity(
    content::StoragePartition* partition);
// Ownership provenance survives an application exit until its actual partition
// is destroyed. Permission routing uses this to deny retired app documents
// instead of falling back to the unrelated ordinary-browser permission store.
std::optional<ElectronSessionIdentity> FindElectronSessionProvenance(
    content::StoragePartition* partition);
std::vector<ElectronSessionPreloadScript> GetElectronSessionPreloadScripts(
    content::StoragePartition* partition,
    const std::string& type);

using ElectronSessionCallCallback =
    base::OnceCallback<void(base::Value result, const std::string& error)>;
using ElectronSessionEventCallback = base::RepeatingCallback<void(base::Value)>;
void CallElectronSession(content::BrowserContext* context,
                         const std::string& container_id,
                         base::Value arguments,
                         ElectronSessionCallCallback callback,
                         ElectronSessionEventCallback event_callback = {});
// Revokes executable preloads and event observers in every BrowserContext when
// their shared container Utility exits, even after the active profile changes.
void RemoveElectronSessionsForContainer(const std::string& container_id);

}  // namespace xenon::ipc
#endif
