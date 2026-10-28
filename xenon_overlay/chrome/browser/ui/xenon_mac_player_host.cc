// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ui/xenon_mac_player_host.h"

#include <algorithm>
#include <set>
#include <string_view>
#include <utility>

#include "base/apple/bundle_locations.h"
#include "base/apple/foundation_util.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/task/thread_pool.h"
#include "content/public/browser/browser_thread.h"
#include "xenon_overlay/chrome/browser/ui/xenon_electron_window_host.h"
#include "xenon_overlay/chrome/browser/ui/xenon_mac_player_adapter.h"
#include "xenon_overlay/chrome/browser/ui/xenon_mac_player_view.h"
#include "xenon_overlay/services/xenon_node_executor.h"

namespace xenon {
namespace {
constexpr char kWire[] = "__xenon_node_wire_type__";
constexpr uint64_t kProviderOwner = 1;
constexpr char kDenied[] =
    "ERR_ACCESS_DENIED: Mac player ownership or capability mismatch";

base::Value Describe(const MacPlayerAdapter& adapter,
                     const mojom::NodeExportInfoPtr& info,
                     bool root = false) {
  base::DictValue value;
  value.Set("name", info->name);
  value.Set("kind", info->kind);
  value.Set("enumerable", info->enumerable);
  value.Set("writable", info->writable);
  value.Set("hasValue", info->has_value);
  value.Set("value", info->value.Clone());
  base::ListValue children;
  for (const auto& child : info->children) {
    if (!root || adapter.AllowsExport(child->name)) {
      children.Append(Describe(adapter, child));
    }
  }
  value.Set("children", std::move(children));
  base::ListValue prototype;
  for (const auto& member : info->prototype) {
    prototype.Append(Describe(adapter, member));
  }
  value.Set("prototype", std::move(prototype));
  return base::Value(std::move(value));
}

bool Bounded(const base::Value& value, size_t* budget, int depth = 0) {
  if (depth > 48 || *budget == 0) {
    return false;
  }
  --*budget;
  size_t bytes = value.is_string() ? value.GetString().size()
                 : value.is_blob() ? value.GetBlob().size()
                                   : 0;
  if (bytes > *budget) {
    return false;
  }
  *budget -= bytes;
  if (value.is_list()) {
    for (const auto& item : value.GetList()) {
      if (!Bounded(item, budget, depth + 1)) {
        return false;
      }
    }
  } else if (value.is_dict()) {
    for (const auto [key, item] : value.GetDict()) {
      if (key.size() > *budget) {
        return false;
      }
      *budget -= key.size();
      if (!Bounded(item, budget, depth + 1)) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace

XenonMacPlayerHost* XenonMacPlayerHost::GetInstance() {
  static base::NoDestructor<XenonMacPlayerHost> host;
  return host.get();
}
XenonMacPlayerHost::XenonMacPlayerHost() = default;
XenonMacPlayerHost::~XenonMacPlayerHost() = default;

void XenonMacPlayerHost::Call(const std::string& container,
                              uint64_t generation,
                              const base::FilePath& resources,
                              const base::FilePath& runtime,
                              base::Value request,
                              Event event,
                              Reply reply) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  std::vector<int> callback_ids;
  if (request.is_dict() && request.GetDict().contains("callbackIds")) {
    const auto* ids = request.GetDict().FindList("callbackIds");
    std::set<int> unique;
    if (ids && ids->size() <= 4096) {
      for (const auto& id : *ids) {
        if (!id.is_int() || id.GetInt() <= 0 ||
            !unique.insert(id.GetInt()).second) {
          unique.clear();
          break;
        }
        callback_ids.push_back(id.GetInt());
      }
    }
    if (!ids || ids->size() > 4096 || unique.size() != ids->size()) {
      std::move(reply).Run(
          MakeMacPlayerReply({}, "Invalid Mac player callback receipt request"),
          "");
      return;
    }
  }
  // A request may fail before argument conversion, or native code may retain a
  // callback and then throw. A receipt reflects the executor's actual ownership
  // rather than whether dispatch started or whether the operation succeeded.
  reply = base::BindOnce(
      [](XenonMacPlayerHost* self, int32_t client,
         std::vector<int> callback_ids, Reply reply, base::Value result,
         const std::string& error) {
        base::ListValue accepted;
        for (int id : callback_ids) {
          if (client <= 0) {
            continue;
          }
          if (self->executor_ &&
              self->executor_->HasNativeCallback(client, id)) {
            accepted.Append(id);
          } else {
            self->providers_.erase({client, id});
          }
        }
        std::move(reply).Run(
            MakeMacPlayerReply(std::move(result), error, std::move(accepted)),
            "");
      },
      base::Unretained(this),
      container_ == container && generation_ == generation ? client_ : 0,
      std::move(callback_ids), std::move(reply));
  size_t budget = 8 * 1024 * 1024;
  if (shutdown_ || !request.is_dict() || !Bounded(request, &budget)) {
    std::move(reply).Run({}, "Invalid Mac player request");
    return;
  }
  const std::string* operation = request.GetDict().FindString("operation");
  if (operation && *operation == "capability") {
    const auto bundle = base::apple::AmIBundled()
                            ? base::apple::OuterBundlePath()
                            : base::FilePath();
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce(&ResolveBundledMacPlayerAdapter, bundle, resources),
        base::BindOnce(
            [](Reply reply, ResolvedMacPlayerAdapter resolved) {
              base::Value capability;
              if (resolved.adapter) {
                capability = base::Value(
                    base::DictValue()
                        .Set("version", 1)
                        .Set("adapterId", std::string(resolved.adapter->id))
                        .Set("adapterVersion", resolved.adapter->version));
              }
              std::move(reply).Run(std::move(capability), "");
            },
            std::move(reply)));
    return;
  }
  if (quarantined_) {
    std::move(reply).Run(
        {}, "Mac player native cleanup failed; restart the browser to recover");
    return;
  }
  if (!operation ||
      (!container_.empty() &&
       (container_ != container || generation_ != generation)) ||
      (!runtime_container_.empty() && runtime_container_ != container)) {
    std::move(reply).Run({}, kDenied);
    return;
  }
  if (*operation == "load") {
    const std::string* path = request.GetDict().FindString("path");
    if (!path || loading_) {
      std::move(reply).Run({}, "Mac player load is invalid or already pending");
      return;
    }
    if (container_.empty()) {
      container_ = container;
      generation_ = generation;
      ++owner_;
      ++client_;
      event_ = std::move(event);
    }
    Load(resources, runtime, *path, std::move(reply));
    return;
  }
  if (container_.empty() || !executor_ || !adapter_ || loading_ ||
      module_path_.empty()) {
    std::move(reply).Run({}, "Mac player has not been loaded");
    return;
  }
  Execute(request.GetDict(), std::move(reply));
}

void XenonMacPlayerHost::Load(const base::FilePath& resources,
                              const base::FilePath& runtime,
                              const std::string& requested,
                              Reply reply) {
  loading_ = true;
  const auto bundle = base::apple::AmIBundled() ? base::apple::OuterBundlePath()
                                                : base::FilePath();
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(
          [](base::FilePath bundle, base::FilePath resources,
             base::FilePath requested) {
            auto resolved = ResolveBundledMacPlayerAdapter(bundle, resources);
            if (resolved.adapter &&
                requested != resources.AppendASCII(
                                 resolved.adapter->module_relative_path)) {
              return ResolvedMacPlayerAdapter();
            }
            return resolved;
          },
          bundle, resources, base::FilePath::FromUTF8Unsafe(requested)),
      base::BindOnce(
          [](base::WeakPtr<XenonMacPlayerHost> self, base::FilePath runtime,
             Reply reply, ResolvedMacPlayerAdapter resolved) {
            if (!self) {
              std::move(reply).Run({}, "Mac player owner was closed");
              return;
            }
            self->loading_ = false;
            if (!resolved.adapter ||
                (self->adapter_ && self->adapter_ != resolved.adapter) ||
                (!self->module_path_.empty() &&
                 self->module_path_ != resolved.module_path)) {
              if (self->module_path_.empty()) {
                self->CloseContainer(self->container_);
              }
              std::move(reply).Run({}, kDenied);
              return;
            }
            self->adapter_ = resolved.adapter;
            std::string path = std::move(resolved.module_path);
            if (!self->executor_) {
              self->executor_ = std::make_unique<XenonNodeExecutor>();
              self->executor_->SetRuntimeDirectory(runtime);
              self->executor_->RegisterInstanceOwner(kProviderOwner);
              // This NoDestructor singleton and its executor outlive every
              // Utility. Session WeakPtrs must not disable provider failures.
              self->executor_->SetCallbackHandlers(
                  base::BindRepeating(&XenonMacPlayerHost::OnNativeCallback,
                                      base::Unretained(self.get())),
                  base::BindRepeating(
                      &XenonMacPlayerHost::OnNativeCallbackReleased,
                      base::Unretained(self.get())));
            }
            self->executor_->RegisterInstanceOwner(self->owner_);
            self->loading_ = true;
            self->executor_->LoadAddon(
                path,
                base::BindOnce(
                    [](base::WeakPtr<XenonMacPlayerHost> self, std::string path,
                       Reply reply, bool success, const std::string& error,
                       std::vector<mojom::NodeExportInfoPtr>) {
                      if (!self) {
                        std::move(reply).Run({}, "Mac player owner was closed");
                        return;
                      }
                      self->loading_ = false;
                      if (!success) {
                        // The executor owns the error reference. Cleanup can
                        // reenter it, so keep the original load failure before
                        // releasing this generation's claim and native owner.
                        const std::string load_error = error;
                        self->CloseContainer(self->container_);
                        std::move(reply).Run({}, load_error);
                        return;
                      }
                      self->module_path_ = std::move(path);
                      self->runtime_container_ = self->container_;
                      self->Execute(base::DictValue()
                                        .Set("operation", "describe")
                                        .Set("exportPath", ""),
                                    std::move(reply));
                    },
                    self, path, std::move(reply)),
                false);
          },
          weak_factory_.GetWeakPtr(), runtime, std::move(reply)));
}

bool XenonMacPlayerHost::Invoke(std::string_view name,
                                const base::Value& args,
                                base::Value* result,
                                std::string* error) {
  return executor_->InvokeExportFromCurrentThread(
      module_path_, std::string(name), args, result, error, client_, owner_);
}

void XenonMacPlayerHost::Execute(const base::DictValue& request, Reply reply) {
  const std::string& op = *request.FindString("operation");
  base::Value result;
  std::string error;
  const auto* args = request.Find("args");
  const auto* name = request.FindString("exportPath");
  if (op == "describe" && name &&
      (name->empty() || adapter_->AllowsExport(*name) ||
       *name == std::string(adapter_->root_constructor) + ".prototype")) {
    mojom::NodeExportInfoPtr info;
    if (executor_->InspectExportFromCurrentThread(module_path_, *name, &info,
                                                  &error)) {
      result = Describe(*adapter_, info, name->empty());
    }
  } else if (op == "invoke" && name && args && args->is_list() &&
             adapter_->AllowsExport(*name) &&
             *name != adapter_->root_constructor) {
    if (*name == adapter_->shutdown_export) {
      // MacPlayer/transport is a process singleton. Logical app disposal closes
      // its session; only Browser shutdown may tear down the SDK and N-API env.
      CancelPendingCompletions();
      CloseMedia(&error);
      if (error.empty()) {
        CloseView(&error);
      }
    } else if (*name == adapter_->initialize_export) {
      base::DictValue identity;
      if (args->GetList().size() != 1 || !args->GetList()[0].is_dict()) {
        error = "Invalid Mac player initialization";
      } else {
        const auto& config = args->GetList()[0].GetDict();
        for (std::string_view key : adapter_->initialization_identity_keys) {
          const auto* item = config.FindString(key);
          if (item) {
            identity.Set(std::string(key), *item);
          }
        }
        if (initialized_ && identity != transport_identity_) {
          error =
              "Mac player runtime identity cannot change within a Browser "
              "process";
        } else if (Invoke(*name, *args, &result, &error)) {
          initialized_ = true;
          transport_identity_ = std::move(identity);
          if (!stack_id_) {
            base::Value stack;
            if (executor_->ConstructExportFromCurrentThread(
                    module_path_, std::string(adapter_->root_constructor),
                    base::Value(base::ListValue()), &stack, &error, client_,
                    owner_)) {
              stack_id_ = stack.GetDict().FindInt("instance_id").value_or(0);
            }
          }
        }
      }
    } else if (adapter_->IsProviderRegistration(*name)) {
      std::vector<MacPlayerCallbackRegistration> registrations;
      if (ReadMacPlayerCallbackRegistrations(adapter_->callbacks, *name,
                                             args->GetList(), &registrations,
                                             &error)) {
        for (const auto& registration : registrations) {
          auto existing = providers_.find({client_, registration.callback_id});
          if (existing != providers_.end() &&
              (existing->second.method != registration.method ||
               existing->second.provider != registration.provider)) {
            error = "Mac player callback id cannot change provider contract";
            break;
          }
        }
        if (error.empty()) {
          // Install before invoking native code: it may request data while
          // registration is still on the stack. Retain metadata on an addon
          // exception until the native callback's GC notification arrives.
          for (auto& registration : registrations) {
            const int callback_id = registration.callback_id;
            providers_.insert_or_assign({client_, callback_id},
                                        std::move(registration));
          }
          executor_->InvokeExportFromCurrentThread(module_path_, *name, *args,
                                                   &result, &error, client_,
                                                   kProviderOwner);
        }
      }
    } else {
      Invoke(*name, *args, &result, &error);
    }
  } else if (op == "construct" && name && *name == adapter_->root_constructor &&
             args && args->is_list() && initialized_) {
    executor_->ConstructExportFromCurrentThread(
        module_path_, *name, *args, &result, &error, client_, owner_);
  } else if (op == "invoke-instance" || op == "release") {
    int id = request.FindInt("instanceId").value_or(0);
    const auto* token = request.FindString("token");
    const auto* method = request.FindString("method");
    const auto pending = pending_completions_.find(id);
    if (!id || !token) {
      error = kDenied;
    } else if (pending != pending_completions_.end()) {
      if (pending->second.client != client_ ||
          !executor_->ValidateInstanceOwnerToken(kProviderOwner, *token) ||
          pending->second.completion.owner_token != *token) {
        error = kDenied;
      } else if (op == "release") {
        // A collected JS completion must resume its native coroutine too.
        CancelCompletion(id);
      } else if (!method || *method != "call" || !args || !args->is_list() ||
                 args->GetList().size() !=
                     1 + pending->second.completion.failure_args.size()) {
        error = "Invalid Mac player completion invocation";
      } else {
        // The native function deletes its callback userdata on the first call.
        // Remove it before invocation, which can synchronously reenter us.
        pending_completions_.erase(pending);
        executor_->InvokeInstanceFromCurrentThread(module_path_, id, "call",
                                                   *args, &result, &error,
                                                   client_, kProviderOwner);
        executor_->ReleaseInstance(module_path_, id, kProviderOwner, *token);
      }
    } else if (executor_->ValidateInstanceOwnerToken(kProviderOwner, *token)) {
      // A late GC release is harmless. Calls to consumed completions fail.
      if (op != "release") {
        error = "Mac player completion has already been consumed";
      }
    } else if (!executor_->ValidateInstanceOwnerToken(owner_, *token)) {
      error = kDenied;
    } else if (op == "release") {
      DetachSubscriptions(id);
      if (id != stack_id_) {
        executor_->ReleaseInstance(module_path_, id, owner_, *token);
      }
    } else if (!method || !args || !args->is_list() || method->empty() ||
               method->find_first_of(".$") != std::string::npos ||
               method->starts_with("__") || *method == "constructor" ||
               *method == "prototype" || *method == "bind" ||
               *method == "apply" || *method == "call" ||
               adapter_->ExcludesInstanceMethod(*method) ||
               (!adapter_->DetachMethod(*method).empty() &&
                subscriptions_.size() >= 4096)) {
      error = "Invalid Mac player native method";
    } else {
      executor_->InvokeInstanceFromCurrentThread(
          module_path_, id, *method, *args, &result, &error, client_, owner_);
      if (error.empty()) {
        if (std::string_view detach = adapter_->DetachMethod(*method);
            !detach.empty() && (result.is_int() || result.is_double()) &&
            result.GetDouble() != 0) {
          subscriptions_.push_back({id, std::string(detach), result.Clone()});
        } else if (args->GetList().size() == 1) {
          std::erase_if(subscriptions_, [&](const Subscription& subscription) {
            return subscription.instance_id == id &&
                   subscription.method == *method &&
                   subscription.cookie == args->GetList()[0];
          });
        }
      }
    }
  } else if (op == "attach" || op == "detach") {
    const int id = request.FindInt("windowId").value_or(0);
    if (op == "detach" && id == window_id_) {
      CloseView(&error);
    } else if (op == "detach" && window_id_ == 0) {
      // Idempotent after the native window closed first.
    } else if (op == "attach" && id > 0 && stack_id_) {
      views::Widget* widget = nullptr;
      content::WebContents* contents = nullptr;
      if (XenonElectronWindowHost::GetInstance()->GetOwnedPlaybackTarget(
              container_, id, &widget, &contents, &error)) {
        if (window_id_ != id) {
          if (!CloseView(&error)) {
            // RefreshAttachment clears its error output. Never refresh the
            // old target or bind a new one after SDK detachment failed.
            std::move(reply).Run({}, error);
            return;
          }
          auto view = XenonMacPlayerView::Create(
              widget, contents, &error,
              base::BindOnce(&XenonMacPlayerHost::CloseWindow,
                             weak_factory_.GetWeakPtr(), id));
          if (view) {
            base::ListValue local_args;
            local_args.Append(
                base::DictValue()
                    .Set(kWire, "bigint")
                    .Set("value",
                         base::NumberToString(
                             view->native_view_for_in_process_addon())));
            // This address is created and consumed locally, never sent by IPC.
            if (Invoke(adapter_->bind_view_export,
                       base::Value(std::move(local_args)), &result, &error)) {
              view_ = std::move(view);
              window_id_ = id;
            }
          }
        } else if (view_) {
          view_->RefreshAttachment(&error);
        }
      }
    } else {
      error = kDenied;
    }
  } else {
    error = "Unsupported Mac player operation";
  }
  std::move(reply).Run(std::move(result), error);
}

bool XenonMacPlayerHost::CloseMedia(std::string* error) {
  if (!stack_id_) {
    return true;
  }
  base::Value ignored;
  return executor_->InvokeInstanceFromCurrentThread(
      module_path_, stack_id_, std::string(adapter_->close_media_method),
      base::Value(base::ListValue()), &ignored, error, client_, owner_);
}

bool XenonMacPlayerHost::CloseView(std::string* error) {
  if (!view_) {
    return true;
  }
  base::Value ignored;
  if (!executor_->InvokeInstanceFromCurrentThread(
          module_path_, stack_id_, std::string(adapter_->close_view_method),
          base::Value(base::ListValue()), &ignored, error, client_, owner_)) {
    return false;
  }
  view_.reset();
  window_id_ = 0;
  return true;
}

void XenonMacPlayerHost::CloseWindow(int32_t id) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (id != window_id_ || !view_) {
    return;
  }
  std::string error;
  CloseMedia(&error);
  if (!CloseView(&error)) {
    QuarantineSession(error);
    return;
  }
  if (!error.empty()) {
    LOG(ERROR) << "Mac player window cleanup: " << error;
  }
}

void XenonMacPlayerHost::CloseContainer(const std::string& container) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (container != container_) {
    return;
  }
  // Mark closed before completing requests: a resumed coroutine can request
  // more provider data immediately. Those requests must fail, never reach a
  // newer Utility generation or recreate a session native instance.
  container_.clear();
  event_.Reset();
  weak_factory_.InvalidateWeakPtrs();
  std::string error;
  CancelPendingCompletions();
  DetachSubscriptions();
  CloseMedia(&error);
  if (!CloseView(&error)) {
    // Keep the session owner and stack handle valid for shutdown. Releasing
    // them while the SDK may still reference this view prevents any safe retry.
    QuarantineSession(error);
    return;
  }
  if (!error.empty()) {
    LOG(ERROR) << "Mac player container cleanup: " << error;
  }
  if (executor_) {
    executor_->ReleaseInstanceOwner(owner_);
  }
  stack_id_ = 0;
  loading_ = false;
}

void XenonMacPlayerHost::Shutdown() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (shutdown_) {
    return;
  }
  shutdown_ = true;
  CloseContainer(container_);
  CancelPendingCompletions();
  if (executor_ && initialized_) {
    std::string error;
    base::Value ignored;
    CloseMedia(&error);
    CloseView(&error);
    // owner zero is a local host operation even if the Utility already exited.
    const bool uninitialized = executor_->InvokeExportFromCurrentThread(
        module_path_, std::string(adapter_->shutdown_export),
        base::Value(base::ListValue()), &ignored, &error);
    if (uninitialized) {
      // Successful SDK shutdown also releases a render target whose normal
      // normal view-close path failed. Otherwise retain it until process exit.
      initialized_ = false;
      view_.reset();
      window_id_ = 0;
      executor_->ReleaseInstanceOwner(owner_);
      stack_id_ = 0;
    }
    if (!error.empty()) {
      LOG(ERROR) << "Mac player shutdown: " << error;
    }
  }
  // The SDK retains process-global N-API references and posts cleanup tasks.
  // Keep their environment alive until process termination, as Electron does.
  // The NoDestructor host owns exactly one executor for its entire lifetime.
}

void XenonMacPlayerHost::QuarantineSession(const std::string& error) {
  quarantined_ = true;
  container_.clear();
  event_.Reset();
  weak_factory_.InvalidateWeakPtrs();
  loading_ = false;
  CancelPendingCompletions();
  DetachSubscriptions();
  if (view_) {
    // Remove the visual attachment and observers, but preserve the strong
    // NSView references because native code may still retain their addresses.
    view_->Detach();
  }
  LOG(ERROR) << "Mac player session quarantined after native cleanup failure: "
             << error;
}

void XenonMacPlayerHost::DetachSubscriptions(int32_t instance_id) {
  auto subscriptions = std::exchange(subscriptions_, {});
  for (auto& subscription : subscriptions) {
    if (instance_id && subscription.instance_id != instance_id) {
      subscriptions_.push_back(std::move(subscription));
      continue;
    }
    base::ListValue args;
    args.Append(std::move(subscription.cookie));
    base::Value ignored;
    std::string error;
    executor_->InvokeInstanceFromCurrentThread(
        module_path_, subscription.instance_id, subscription.method,
        base::Value(std::move(args)), &ignored, &error, client_, owner_);
    if (!error.empty()) {
      LOG(ERROR) << "Mac player event cleanup: " << error;
    }
  }
}

void XenonMacPlayerHost::OnNativeCallback(int32_t client,
                                          int32_t id,
                                          std::vector<base::Value> args,
                                          base::Value receiver) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  base::ListValue list;
  for (auto& arg : args) {
    list.Append(std::move(arg));
  }
  const bool active = !shutdown_ && !container_.empty() && client == client_;
  const auto provider = providers_.find({client, id});
  if (provider != providers_.end()) {
    std::optional<MacPlayerCompletion> completion;
    std::string error;
    if (!ReadMacPlayerCompletion(adapter_->callbacks, provider->second, list,
                                 &completion, &error)) {
      LOG(ERROR) << "Mac player provider callback: " << error;
      return;
    }
    if (completion) {
      const int instance_id = completion->instance_id;
      if (!executor_->ValidateInstanceOwnerToken(kProviderOwner,
                                                 completion->owner_token)) {
        LOG(ERROR) << "Mac player provider completion has wrong owner";
        return;
      }
      if (pending_completions_.contains(instance_id)) {
        LOG(ERROR) << "Mac player native completion was delivered twice";
        return;
      }
      pending_completions_.emplace(
          instance_id, PendingCompletion{client, std::move(*completion)});
      if (!active) {
        // Leave the native callback stack before resuming the suspended SDK
        // coroutine. This task deliberately survives session invalidation.
        base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
            FROM_HERE, base::BindOnce(&XenonMacPlayerHost::CancelCompletion,
                                      base::Unretained(this), instance_id));
      }
    }
  }
  if (!active) {
    return;
  }
  QueueEvent(base::Value(base::DictValue()
                             .Set("type", "callback")
                             .Set("callbackId", id)
                             .Set("args", std::move(list))
                             .Set("receiver", std::move(receiver))));
}

void XenonMacPlayerHost::OnNativeCallbackReleased(int32_t client, int32_t id) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  providers_.erase({client, id});
  if (!shutdown_ && !container_.empty() && client == client_) {
    QueueEvent(base::Value(base::DictValue()
                               .Set("type", "release-callback")
                               .Set("callbackId", id)));
  }
}

void XenonMacPlayerHost::CancelCompletion(int32_t instance_id) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  const auto it = pending_completions_.find(instance_id);
  if (it == pending_completions_.end()) {
    return;
  }
  auto pending = std::move(it->second);
  pending_completions_.erase(it);
  base::ListValue args;
  args.Append(base::DictValue().Set(kWire, "undefined"));
  for (auto& arg : pending.completion.failure_args) {
    args.Append(std::move(arg));
  }
  base::Value ignored;
  std::string error;
  executor_->InvokeInstanceFromCurrentThread(
      module_path_, instance_id, "call", base::Value(std::move(args)), &ignored,
      &error, pending.client, kProviderOwner);
  executor_->ReleaseInstance(module_path_, instance_id, kProviderOwner,
                             pending.completion.owner_token);
  if (!error.empty()) {
    LOG(ERROR) << "Mac player completion cleanup: " << error;
  }
}

void XenonMacPlayerHost::CancelPendingCompletions() {
  // Taking an id snapshot avoids iterator invalidation when native completion
  // calls produce another provider request. Requests arriving after closure
  // schedule their own failure task in OnNativeCallback.
  std::vector<int32_t> ids;
  ids.reserve(pending_completions_.size());
  for (const auto& [id, pending] : pending_completions_) {
    ids.push_back(id);
  }
  for (int32_t id : ids) {
    CancelCompletion(id);
  }
}

void XenonMacPlayerHost::QueueEvent(base::Value event) {
  // Never dispatch back into Utility on a synchronous native invocation stack.
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](base::WeakPtr<XenonMacPlayerHost> self, base::Value event) {
            if (self && self->event_) {
              self->event_.Run(std::move(event));
            }
          },
          weak_factory_.GetWeakPtr(), std::move(event)));
}
}  // namespace xenon
