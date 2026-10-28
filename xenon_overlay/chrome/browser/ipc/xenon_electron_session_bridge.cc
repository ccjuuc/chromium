// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_session_bridge.h"

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <string_view>
#include <utility>

#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/supports_user_data.h"
#include "base/task/thread_pool.h"
#include "base/uuid.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/service_worker_context.h"
#include "content/public/browser/service_worker_context_observer.h"
#include "content/public/browser/service_worker_running_info.h"
#include "content/public/browser/storage_partition.h"
#include "crypto/hash.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_file_system_bridge.h"

namespace xenon::ipc {
namespace {
const char kRegistryKey[] = "xenon-electron-sessions";
class SessionState;
auto& PartitionSessions() {
  static base::NoDestructor<std::map<content::StoragePartition*, SessionState*>>
      map;
  return *map;
}

base::DictValue ScriptValue(const ElectronSessionPreloadScript& script) {
  return base::DictValue()
      .Set("id", script.id)
      .Set("type", script.type)
      .Set("filePath", script.file_path);
}

base::DictValue WorkerValue(int64_t id,
                            const content::ServiceWorkerRunningInfo& info,
                            const std::string& status) {
  return base::DictValue()
      .Set("versionId", static_cast<double>(id))
      .Set("scope", info.scope.spec())
      .Set("scriptUrl", info.script_url.spec())
      .Set("renderProcessId", info.render_process_id.GetUnsafeValue())
      .Set("runningStatus", status);
}

class SessionState : public content::ServiceWorkerContextObserver {
 public:
  SessionState(ElectronSessionIdentity identity,
               content::StoragePartition* partition)
      : identity_(std::move(identity)),
        partition_(partition),
        workers_(partition->GetServiceWorkerContext()) {
    PartitionSessions()[partition] = this;
    workers_->AddObserver(this);
  }
  ~SessionState() override {
    if (workers_) {
      workers_->RemoveObserver(this);
    }
    if (partition_) {
      PartitionSessions().erase(partition_);
    }
  }
  const ElectronSessionIdentity& identity() const { return identity_; }
  const std::vector<ElectronSessionPreloadScript>& scripts() const {
    return scripts_;
  }
  void SetEventCallback(ElectronSessionEventCallback callback) {
    if (callback) {
      event_callback_ = std::move(callback);
    }
  }

  void Call(const base::DictValue& args, ElectronSessionCallCallback callback) {
    const std::string* method = args.FindString("method");
    if (!method) {
      std::move(callback).Run(base::Value(), "Session method is required");
      return;
    }
    if (*method == "observe-workers") {
      std::move(callback).Run(base::Value(), "");
    } else if (*method == "get-preloads") {
      base::ListValue result;
      for (const auto& script : scripts_) {
        result.Append(ScriptValue(script));
      }
      std::move(callback).Run(base::Value(std::move(result)), "");
    } else if (*method == "register-preload") {
      const auto* descriptor = args.FindDict("script");
      const std::string* path =
          descriptor ? descriptor->FindString("filePath") : nullptr;
      const std::string* type =
          descriptor ? descriptor->FindString("type") : nullptr;
      const std::string* requested_id =
          descriptor ? descriptor->FindString("id") : nullptr;
      if (!path || !type || (*type != "frame" && *type != "service-worker") ||
          !base::FilePath::FromUTF8Unsafe(*path).IsAbsolute() ||
          (descriptor->contains("id") &&
           (!requested_id || requested_id->empty()))) {
        std::move(callback).Run(base::Value(),
                                "Invalid preload script descriptor");
        return;
      }
      ElectronSessionPreloadScript script{
          requested_id ? *requested_id
                       : base::Uuid::GenerateRandomV4().AsLowercaseString(),
          *type,
          *path,
          {}};
      base::ListValue request;
      request.Append(base::DictValue()
                         .Set("operation", "read_file")
                         .Set("path", *path)
                         .Set("returnBytes", true));
      // Registration is synchronous to JS, but disk/ASAR reads never block UI.
      base::ThreadPool::PostTaskAndReplyWithResult(
          FROM_HERE, {base::MayBlock()},
          base::BindOnce(&PerformFileSystemCall,
                         base::Value(std::move(request))),
          base::BindOnce(
              [](base::WeakPtr<SessionState> self,
                 ElectronSessionPreloadScript script,
                 ElectronSessionCallCallback callback,
                 mojom::IpcResultPtr result) {
                if (!self) {
                  std::move(callback).Run(base::Value(),
                                          "Session owner disconnected");
                  return;
                }
                if (!result || !result->success || !result->value.is_blob()) {
                  std::move(callback).Run(base::Value(),
                                          !result || result->error.empty()
                                              ? "Unable to read preload script"
                                              : result->error);
                  return;
                }
                for (const auto& existing : self->scripts_) {
                  if (existing.id == script.id) {
                    std::move(callback).Run(
                        base::Value(), "Preload script id already registered");
                    return;
                  }
                }
                const auto& bytes = result->value.GetBlob();
                script.source.assign(bytes.begin(), bytes.end());
                const std::string id = script.id;
                self->scripts_.push_back(std::move(script));
                std::move(callback).Run(base::Value(id), "");
              },
              weak_factory_.GetWeakPtr(), std::move(script),
              std::move(callback)));
    } else if (*method == "unregister-preload") {
      const std::string* id = args.FindString("id");
      auto found = std::find_if(
          scripts_.begin(), scripts_.end(),
          [&](const auto& script) { return id && script.id == *id; });
      if (found == scripts_.end()) {
        std::move(callback).Run(base::Value(),
                                "Preload script id is not registered");
        return;
      }
      scripts_.erase(found);
      std::move(callback).Run(base::Value(), "");
    } else if (*method == "get-running-workers") {
      base::DictValue result;
      if (workers_) {
        for (const auto& [id, info] :
             workers_->GetRunningServiceWorkerInfos()) {
          const auto status = statuses_.find(id);
          result.Set(base::NumberToString(id),
                     WorkerValue(id, info,
                                 status == statuses_.end() ? "running"
                                                           : status->second));
        }
      }
      std::move(callback).Run(base::Value(std::move(result)), "");
    } else {
      std::move(callback).Run(base::Value(),
                              "Unsupported session method: " + *method);
    }
  }

  void OnVersionStartingRunning(int64_t id) override { Emit(id, "starting"); }
  void OnVersionStartedRunning(
      int64_t id,
      const content::ServiceWorkerRunningInfo& info) override {
    Emit(id, "running");
  }
  void OnVersionStoppingRunning(int64_t id) override { Emit(id, "stopping"); }
  void OnVersionStoppedRunning(int64_t id) override { Emit(id, "stopped"); }
  void OnDestruct(content::ServiceWorkerContext* context) override {
    // The wrapper still owns its observer list while notifying destruction and
    // requires every observer to detach before that list is destroyed.
    context->RemoveObserver(this);
    PartitionSessions().erase(partition_);
    partition_ = nullptr;
    workers_ = nullptr;
    scripts_.clear();
    weak_factory_.InvalidateWeakPtrs();
  }

 private:
  void Emit(int64_t id, const char* status) {
    if (std::string_view(status) == "stopped") {
      statuses_.erase(id);
    } else {
      statuses_[id] = status;
    }
    if (event_callback_) {
      event_callback_.Run(base::Value(
          base::DictValue()
              .Set("partition", identity_.partition)
              .Set("event", "running-status-changed")
              .Set("details", base::DictValue()
                                  .Set("versionId", static_cast<double>(id))
                                  .Set("runningStatus", status))));
    }
  }
  ElectronSessionIdentity identity_;
  raw_ptr<content::StoragePartition> partition_;
  raw_ptr<content::ServiceWorkerContext> workers_;
  std::vector<ElectronSessionPreloadScript> scripts_;
  std::map<int64_t, std::string> statuses_;
  ElectronSessionEventCallback event_callback_;
  base::WeakPtrFactory<SessionState> weak_factory_{this};
};

class SessionRegistry : public base::SupportsUserData::Data {
 public:
  SessionRegistry();
  ~SessionRegistry() override;

  std::map<std::pair<std::string, std::string>, std::unique_ptr<SessionState>>
      sessions;
};

// BrowserContexts own the registries. This UI-sequence index permits revoking a
// Utility's sessions across profiles without retaining any BrowserContext.
auto& SessionRegistries() {
  static base::NoDestructor<std::set<SessionRegistry*>> registries;
  return *registries;
}

SessionRegistry::SessionRegistry() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  SessionRegistries().insert(this);
}

SessionRegistry::~SessionRegistry() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  SessionRegistries().erase(this);
}

SessionRegistry* Registry(content::BrowserContext* context) {
  auto* registry =
      static_cast<SessionRegistry*>(context->GetUserData(kRegistryKey));
  if (!registry) {
    auto owned = std::make_unique<SessionRegistry>();
    registry = owned.get();
    context->SetUserData(kRegistryKey, std::move(owned));
  }
  return registry;
}
}  // namespace

content::StoragePartitionConfig GetElectronSessionPartitionConfig(
    content::BrowserContext* context,
    const std::string& container_id,
    const std::string& partition) {
  return content::StoragePartitionConfig::Create(
      context,
      "xenon" + base::HexEncodeLower(crypto::hash::Sha256(container_id)),
      base::HexEncodeLower(crypto::hash::Sha256(partition)),
      !partition.empty() && !base::StartsWith(partition, "persist:"));
}

content::StoragePartition* ResolveElectronSessionPartition(
    content::BrowserContext* context,
    const std::string& container_id,
    const std::string& partition) {
  auto* storage = context->GetStoragePartition(
      GetElectronSessionPartitionConfig(context, container_id, partition));
  auto& sessions = Registry(context)->sessions;
  const auto key = std::make_pair(container_id, partition);
  if (!sessions.contains(key)) {
    sessions.emplace(
        key, std::make_unique<SessionState>(
                 ElectronSessionIdentity{container_id, partition}, storage));
  }
  return storage;
}

std::optional<ElectronSessionIdentity> FindElectronSessionIdentity(
    content::StoragePartition* partition) {
  const auto found = PartitionSessions().find(partition);
  return found == PartitionSessions().end()
             ? std::nullopt
             : std::make_optional(found->second->identity());
}

std::vector<ElectronSessionPreloadScript> GetElectronSessionPreloadScripts(
    content::StoragePartition* partition,
    const std::string& type) {
  std::vector<ElectronSessionPreloadScript> scripts;
  const auto found = PartitionSessions().find(partition);
  if (found != PartitionSessions().end()) {
    for (const auto& script : found->second->scripts()) {
      if (script.type == type) {
        scripts.push_back(script);
      }
    }
  }
  return scripts;
}

void CallElectronSession(content::BrowserContext* context,
                         const std::string& container_id,
                         base::Value arguments,
                         ElectronSessionCallCallback callback,
                         ElectronSessionEventCallback event_callback) {
  if (!context || !arguments.is_dict() ||
      !arguments.GetDict().FindString("partition")) {
    std::move(callback).Run(base::Value(), "Invalid Electron session request");
    return;
  }
  auto* partition = ResolveElectronSessionPartition(
      context, container_id, *arguments.GetDict().FindString("partition"));
  auto* state = PartitionSessions().at(partition);
  state->SetEventCallback(std::move(event_callback));
  state->Call(arguments.GetDict(), std::move(callback));
}

void RemoveElectronSessionsForContainer(const std::string& container_id) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  for (auto* registry : SessionRegistries()) {
    std::erase_if(registry->sessions, [&](const auto& entry) {
      return entry.first.first == container_id;
    });
  }
}

}  // namespace xenon::ipc
