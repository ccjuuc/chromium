// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_electron_permission_broker.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <utility>

#include "base/callback_list.h"
#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/task/single_thread_task_runner.h"
#include "base/task/thread_pool.h"
#include "components/keyed_service/core/keyed_service_shutdown_notifier.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/weak_document_ptr.h"
#include "content/public/browser/web_contents.h"
#include "url/gurl.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_permission_check_remote.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_permission_policy.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_permissions.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_session_bridge.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_session_shutdown_notifier_factory.h"

namespace xenon::ipc {
namespace {

auto& ChangeObservers() {
  static base::NoDestructor<base::RepeatingClosureList> observers;
  return *observers;
}

void NotifyChanged() {
  static bool pending = false;
  if (pending) {
    return;
  }
  pending = true;
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce([] {
        pending = false;
        ChangeObservers().Notify();
      }));
}

class PermissionOwner : public base::RefCounted<PermissionOwner> {
 public:
  PermissionOwner(
      content::BrowserContext* context,
      std::string container,
      std::string owner,
      ElectronPermissionRemoteFactory remotes,
      base::RepeatingCallback<int(content::WebContents*)> contents_ids,
      ElectronPermissionPolicy::EventCallback events)
      : context(context->GetWeakPtr()),
        container(std::move(container)),
        owner(std::move(owner)),
        contents_ids(std::move(contents_ids)),
        remotes_(std::move(remotes)),
        transport_(base::ThreadPool::CreateSingleThreadTaskRunner(
            {base::TaskPriority::USER_BLOCKING},
            base::SingleThreadTaskRunnerThreadMode::DEDICATED)) {
    policy = base::MakeRefCounted<ElectronPermissionPolicy>(
        base::BindRepeating(
            [](base::WeakPtr<PermissionOwner> self,
               base::Value request) -> base::expected<bool, std::string> {
              if (!self || !self->active()) {
                return base::unexpected("ERR_ABORTED: permission owner exited");
              }
              scoped_refptr keep_alive(self.get());
              return self->Check(std::move(request));
            },
            weak_factory_.GetWeakPtr()),
        std::move(events), base::BindRepeating(&NotifyChanged),
        base::Seconds(30));
    shutdown_subscription_ =
        ElectronSessionShutdownNotifierFactory::GetInstance()
            ->Get(context)
            ->Subscribe(base::BindOnce(&PermissionOwner::Close,
                                       weak_factory_.GetWeakPtr()));
    PrepareRemote();
  }

  bool active() const {
    return !retired_ && context && !context->ShutdownStarted();
  }
  void Close() {
    if (retired_) {
      return;
    }
    retired_ = true;
    remote_ready_ = false;
    if (remote_) {
      remote_->Close();
    }
    policy->Close();
  }

  base::expected<void, std::string> RebindContext(
      content::BrowserContext* next_context) {
    if (!active() || !next_context || next_context->ShutdownStarted()) {
      return base::unexpected("ERR_ABORTED: permission context is unavailable");
    }
    if (context.get() == next_context) {
      return {};
    }
    shutdown_subscription_ = {};
    context = next_context->GetWeakPtr();
    ++context_generation;
    shutdown_subscription_ =
        ElectronSessionShutdownNotifierFactory::GetInstance()
            ->Get(next_context)
            ->Subscribe(base::BindOnce(&PermissionOwner::Close,
                                       weak_factory_.GetWeakPtr()));
    NotifyChanged();
    return {};
  }

  base::WeakPtr<content::BrowserContext> context;
  uint64_t context_generation = 0;
  const std::string container;
  const std::string owner;
  const base::RepeatingCallback<int(content::WebContents*)> contents_ids;
  scoped_refptr<ElectronPermissionPolicy> policy;

 private:
  friend class base::RefCounted<PermissionOwner>;
  ~PermissionOwner() { Close(); }
  void PrepareRemote() {
    remote_ready_ = false;
    auto pending = remotes_.Run();
    if (!pending.is_valid()) {
      return;
    }
    remote_ = std::make_unique<ElectronPermissionCheckRemote>(
        std::move(pending), transport_, base::Seconds(5));
    // The ordinary service is a SharedRemote bound to Browser UI. Its async
    // Bind message is queued even when called on UI; a synchronous check must
    // not block that queue before the Bind has been forwarded. Initial binding
    // is prepared before the application's main module is sent to the service.
    // Rebinding after a timeout follows the same ordering and fails closed in
    // the brief interval before this task runs.
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&PermissionOwner::RemoteReady,
                                  weak_factory_.GetWeakPtr()));
  }
  void RemoteReady() {
    if (active() && remote_) {
      remote_ready_ = true;
      // Transport recovery is not a policy change. Refreshing subscriptions
      // here could repeatedly invoke a hung handler after every timeout.
    }
  }
  base::expected<bool, std::string> Check(base::Value request) {
    if (!remote_) {
      PrepareRemote();
    }
    if (!remote_ready_) {
      return base::unexpected(
          "ERR_RESOURCE_BUSY: permission transport is binding");
    }
    auto result = remote_->Check(std::move(request));
    if (!result.has_value() && (result.error().starts_with("ERR_TIMED_OUT:") ||
                                result.error().starts_with("ERR_ABORTED:"))) {
      // A failed dedicated pipe is never reused. A subsequent check binds a
      // fresh pipe on the same still-authenticated service generation.
      remote_.reset();
      if (active()) {
        PrepareRemote();
      }
    }
    return result;
  }
  const ElectronPermissionRemoteFactory remotes_;
  const scoped_refptr<base::SingleThreadTaskRunner> transport_;
  std::unique_ptr<ElectronPermissionCheckRemote> remote_;
  bool retired_ = false;
  bool remote_ready_ = false;
  base::CallbackListSubscription shutdown_subscription_;
  base::WeakPtrFactory<PermissionOwner> weak_factory_{this};
};

auto& Owners() {
  static base::NoDestructor<
      std::map<std::string, scoped_refptr<PermissionOwner>>>
      owners;
  return *owners;
}

scoped_refptr<PermissionOwner> FindOwner(
    content::BrowserContext* context,
    const ElectronSessionIdentity& identity) {
  const auto found = Owners().find(identity.container_id);
  if (found == Owners().end() || !found->second->active() ||
      found->second->context.get() != context) {
    return nullptr;
  }
  return found->second;
}

base::DictValue FrameRequest(content::RenderFrameHost* frame,
                             PermissionOwner& owner,
                             const std::string& permission,
                             base::DictValue details) {
  details.Set("requestingUrl", frame->GetLastCommittedURL().spec());
  details.Set("isMainFrame", !frame->GetParent());
  return base::DictValue()
      .Set("webContentsId",
           owner.contents_ids.Run(
               content::WebContents::FromRenderFrameHost(frame)))
      .Set("permission", permission)
      .Set("details", std::move(details));
}

ElectronPermissionPolicy::ValidCallback FrameValid(
    content::RenderFrameHost* frame,
    scoped_refptr<PermissionOwner> owner) {
  return base::BindRepeating(
      [](content::WeakDocumentPtr document,
         scoped_refptr<PermissionOwner> owner, uint64_t context_generation) {
        auto* frame = document.AsRenderFrameHostIfValid();
        return frame && frame->IsActive() && owner->active() &&
               owner->context_generation == context_generation &&
               frame->GetBrowserContext() == owner->context.get();
      },
      frame->GetWeakDocumentPtr(), owner, owner->context_generation);
}

}  // namespace

void BindElectronPermissionOwner(
    content::BrowserContext* context,
    const std::string& container,
    const std::string& owner,
    ElectronPermissionRemoteFactory remotes,
    base::RepeatingCallback<int(content::WebContents*)> contents_ids,
    base::RepeatingCallback<void(base::Value)> events) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  auto previous = Owners().extract(container);
  if (!previous.empty()) {
    previous.mapped()->Close();
  }
  if (!context || context->ShutdownStarted()) {
    return;
  }
  Owners()[container] = base::MakeRefCounted<PermissionOwner>(
      context, container, owner, std::move(remotes), std::move(contents_ids),
      std::move(events));
  NotifyChanged();
}

void CloseElectronPermissionOwner(const std::string& owner) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  for (auto iterator = Owners().begin(); iterator != Owners().end();
       ++iterator) {
    if (iterator->second->owner == owner) {
      auto removed = Owners().extract(iterator);
      removed.mapped()->Close();
      return;
    }
  }
}

base::expected<void, std::string> RebindElectronPermissionOwnerContext(
    const std::string& owner,
    content::BrowserContext* context) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  for (const auto& [container, candidate] : Owners()) {
    if (candidate->owner == owner) {
      return candidate->RebindContext(context);
    }
  }
  return base::unexpected("ERR_ABORTED: permission owner exited");
}

base::expected<base::Value, std::string> CallElectronPermissions(
    const std::string& container,
    const std::string& owner,
    const base::Value& request) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  const auto found = Owners().find(container);
  if (found == Owners().end() || found->second->owner != owner ||
      !found->second->active()) {
    return base::unexpected("ERR_ABORTED: permission owner exited");
  }
  const auto keep_alive = found->second;
  const auto* args = request.GetIfDict();
  const auto* method = args ? args->FindString("method") : nullptr;
  const auto* partition = args ? args->FindString("partition") : nullptr;
  const auto* kind = args ? args->FindString("kind") : nullptr;
  const auto registration = args ? args->FindInt("registration") : std::nullopt;
  if (!method || !partition || !kind || !registration || *registration <= 0) {
    return base::unexpected(
        "ERR_INVALID_ARG_VALUE: invalid permission command");
  }
  if (*method == "set-handler") {
    const auto enabled = args->FindBool("enabled");
    if (!enabled) {
      return base::unexpected("ERR_INVALID_ARG_TYPE: enabled must be boolean");
    }
    auto result = keep_alive->policy->SetHandler(*partition, *kind,
                                                 *registration, *enabled);
    if (!result.has_value()) {
      return base::unexpected(result.error());
    }
    return base::Value();
  }
  if (*method == "response") {
    const auto id = args->FindInt("requestId");
    if (!id || *id <= 0) {
      return base::unexpected(
          "ERR_INVALID_ARG_VALUE: invalid permission request");
    }
    if (*kind == "request") {
      const auto granted = args->FindBool("granted");
      if (!granted) {
        return base::unexpected(
            "ERR_INVALID_ARG_TYPE: granted must be boolean");
      }
      return base::Value(keep_alive->policy->Respond(*partition, *registration,
                                                     *id, *granted));
    }
    if (*kind == "display") {
      const auto* selection = args->FindDict("selection");
      if (!selection) {
        return base::unexpected(
            "ERR_INVALID_ARG_TYPE: invalid display selection");
      }
      return base::Value(keep_alive->policy->RespondDisplay(
          *partition, *registration, *id, selection->Clone()));
    }
  }
  return base::unexpected(
      "ERR_INVALID_ARG_VALUE: unsupported permission command");
}

bool IsElectronPermissionFrame(content::RenderFrameHost* frame) {
  return frame && FindElectronSessionProvenance(frame->GetStoragePartition())
                      .has_value();
}

bool IsElectronPermissionProcess(content::RenderProcessHost* process) {
  return process &&
         FindElectronSessionProvenance(process->GetStoragePartition())
             .has_value();
}

base::RepeatingCallback<bool()> GetElectronPermissionFrameValidator(
    content::RenderFrameHost* frame) {
  const auto identity =
      frame ? FindElectronSessionProvenance(frame->GetStoragePartition())
            : std::nullopt;
  const auto owner =
      identity ? FindOwner(frame->GetBrowserContext(), *identity) : nullptr;
  return owner ? FrameValid(frame, owner)
               : base::BindRepeating([] { return false; });
}

bool IsActiveElectronPermissionFrame(content::RenderFrameHost* frame) {
  return GetElectronPermissionFrameValidator(frame).Run();
}

base::CallbackListSubscription ObserveElectronPermissionChanges(
    base::RepeatingClosure callback) {
  return ChangeObservers().Add(std::move(callback));
}

bool CheckElectronPermission(content::RenderFrameHost* frame,
                             const std::string& permission,
                             const GURL& requesting_origin,
                             base::DictValue details,
                             bool default_granted) {
  if (!frame || frame->IsNestedWithinFencedFrame()) {
    return false;
  }
  const auto identity =
      FindElectronSessionProvenance(frame->GetStoragePartition());
  const auto owner =
      identity ? FindOwner(frame->GetBrowserContext(), *identity) : nullptr;
  if (!owner) {
    return false;
  }
  auto request = FrameRequest(frame, *owner, permission, std::move(details));
  request.Set("requestingOrigin", requesting_origin.spec());
  return owner->policy
      ->Check(identity->partition, std::move(request), FrameValid(frame, owner),
              default_granted)
      .value_or(false);
}

bool CheckElectronWorkerPermission(content::RenderProcessHost* process,
                                   const std::string& permission,
                                   const GURL& requesting_origin,
                                   base::DictValue details,
                                   bool default_granted) {
  if (!process) {
    return false;
  }
  const auto identity =
      FindElectronSessionProvenance(process->GetStoragePartition());
  const auto owner =
      identity ? FindOwner(process->GetBrowserContext(), *identity) : nullptr;
  if (!owner) {
    return false;
  }
  details.Set("isMainFrame", false);
  auto request = base::DictValue()
                     .Set("webContentsId", 0)
                     .Set("permission", permission)
                     .Set("requestingOrigin", requesting_origin.spec())
                     .Set("details", std::move(details));
  auto valid = base::BindRepeating(
      [](int id, scoped_refptr<PermissionOwner> owner,
         uint64_t context_generation) {
        auto* process = content::RenderProcessHost::FromID(id);
        return process && owner->active() &&
               owner->context_generation == context_generation &&
               process->GetBrowserContext() == owner->context.get();
      },
      process->GetDeprecatedID(), owner, owner->context_generation);
  return owner->policy
      ->Check(identity->partition, std::move(request), std::move(valid),
              default_granted)
      .value_or(false);
}

void RequestElectronPermission(content::RenderFrameHost* frame,
                               const std::string& permission,
                               base::DictValue details,
                               base::OnceCallback<void(bool)> callback) {
  if (!frame || frame->IsNestedWithinFencedFrame()) {
    std::move(callback).Run(false);
    return;
  }
  const auto identity =
      FindElectronSessionProvenance(frame->GetStoragePartition());
  const auto owner =
      identity ? FindOwner(frame->GetBrowserContext(), *identity) : nullptr;
  if (!owner) {
    std::move(callback).Run(false);
    return;
  }
  owner->policy->Request(
      identity->partition,
      FrameRequest(frame, *owner, permission, std::move(details)),
      FrameValid(frame, owner), std::move(callback));
}

void RequestElectronDisplayMedia(
    content::RenderFrameHost* frame,
    base::DictValue details,
    base::OnceCallback<void(base::DictValue)> callback) {
  if (!frame || frame->IsNestedWithinFencedFrame()) {
    std::move(callback).Run({});
    return;
  }
  const auto identity =
      FindElectronSessionProvenance(frame->GetStoragePartition());
  const auto owner =
      identity ? FindOwner(frame->GetBrowserContext(), *identity) : nullptr;
  if (!owner) {
    std::move(callback).Run({});
    return;
  }
  details.Set("frame",
              base::DictValue()
                  .Set("processId", frame->GetProcess()->GetDeprecatedID())
                  .Set("routingId", frame->GetRoutingID()));
  owner->policy->RequestDisplay(
      identity->partition,
      FrameRequest(frame, *owner, "media", std::move(details)),
      FrameValid(frame, owner), std::move(callback));
}

}  // namespace xenon::ipc
