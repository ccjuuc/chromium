// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/services/xenon_service_impl.h"

#include <optional>
#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/memory/ref_counted.h"
#include "base/strings/string_number_conversions.h"
#include "base/values.h"
#include "mojo/public/cpp/bindings/callback_helpers.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/cpp/wrapper_shared_url_loader_factory.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "xenon_overlay/buildflags/buildflags.h"
#include "xenon_overlay/services/xenon_node_executor.h"

namespace xenon {

namespace {

std::vector<mojom::NodeInvokeCallResultPtr> MakeInvokeFailures(
    size_t count,
    const std::string& error_msg) {
  std::vector<mojom::NodeInvokeCallResultPtr> results;
  results.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    auto result = mojom::NodeInvokeCallResult::New();
    result->success = false;
    result->error_msg = error_msg;
    results.push_back(std::move(result));
  }
  return results;
}

}  // namespace

XenonServiceImpl::XenonServiceImpl(
    mojo::PendingReceiver<mojom::XenonMainService> receiver)
    : receiver_(this, std::move(receiver)) {}

XenonServiceImpl::~XenonServiceImpl() = default;

void XenonServiceImpl::Initialize(
    mojo::PendingRemote<network::mojom::URLLoaderFactory> url_loader_factory) {
  url_loader_factory_ =
      base::MakeRefCounted<network::WrapperSharedURLLoaderFactory>(
          std::move(url_loader_factory));
}

void XenonServiceImpl::SetBrowserObserver(
    mojo::PendingRemote<mojom::XenonBrowserObserver> observer) {
#if BUILDFLAG(ENABLE_XENON_BROWSER_OBSERVER)
  browser_observer_.reset();
  if (observer) {
    browser_observer_.Bind(std::move(observer));
  }
#else
  (void)observer;
#endif
}

void XenonServiceImpl::SetNodeAddonObserver(
    int32_t client_id,
    mojo::PendingRemote<mojom::NodeAddonObserver> observer) {
  node_addon_observers_.erase(client_id);
  if (!observer) {
    return;
  }
  mojo::Remote<mojom::NodeAddonObserver>& remote =
      node_addon_observers_[client_id];
  remote.Bind(std::move(observer));
  remote.set_disconnect_handler(
      base::BindOnce(&XenonServiceImpl::OnNodeAddonObserverDisconnected,
                     weak_factory_.GetWeakPtr(), client_id));
}

void XenonServiceImpl::BindAssociatedSide(
    mojo::PendingAssociatedReceiver<mojom::XenonAssociatedSide> receiver) {
#if BUILDFLAG(ENABLE_XENON_ASSOCIATED_SIDE)
  if (associated_receiver_.is_bound()) {
    associated_receiver_.reset();
  }
  associated_receiver_.Bind(std::move(receiver));
#else
  (void)receiver;
#endif
}

#if BUILDFLAG(ENABLE_XENON_ASSOCIATED_SIDE)
void XenonServiceImpl::PingAssociated(PingAssociatedCallback callback) {
  LOG(INFO) << "XenonServiceImpl::PingAssociated received";
  std::move(callback).Run("XenonAssociatedSide ok");
}
#endif

void XenonServiceImpl::Ping(PingCallback callback) {
  LOG(INFO) << "XenonServiceImpl::Ping received";

  if (!url_loader_factory_) {
    std::move(callback).Run("Error: URLLoaderFactory not initialized.");
    return;
  }

  auto resource_request = std::make_unique<network::ResourceRequest>();
  // Generic HTTPS probe; uses the same proxy resolution as all browser traffic.
  resource_request->url = GURL("https://example.com/");
  resource_request->method = "GET";
  resource_request->credentials_mode = network::mojom::CredentialsMode::kOmit;

  std::unique_ptr<network::SimpleURLLoader> loader =
      network::SimpleURLLoader::Create(std::move(resource_request),
                                       MISSING_TRAFFIC_ANNOTATION);

  auto* loader_ptr = loader.get();
  loader_ptr->DownloadToString(
      url_loader_factory_.get(),
      base::BindOnce(
          [](base::WeakPtr<XenonServiceImpl> self,
             std::unique_ptr<network::SimpleURLLoader> keep_alive_loader,
             PingCallback user_callback,
             std::optional<std::string> response_body) {
            (void)keep_alive_loader;
            if (!self) {
              std::move(user_callback).Run("Error: XenonServiceImpl destroyed.");
              return;
            }
            if (response_body) {
              const std::string message = "Probe download success. Size: " +
                                           base::NumberToString(response_body->size());
#if BUILDFLAG(ENABLE_XENON_BROWSER_OBSERVER)
              if (self->browser_observer_.is_connected()) {
                self->browser_observer_->OnServiceEvent(
                    "Utility→Browser: Ping completed, " + message);
              }
#endif
              std::move(user_callback).Run(message);
            } else {
#if BUILDFLAG(ENABLE_XENON_BROWSER_OBSERVER)
              if (self->browser_observer_.is_connected()) {
                self->browser_observer_->OnServiceEvent(
                    "Utility→Browser: Ping failed (no response body)");
              }
#endif
              std::move(user_callback).Run("Probe download failed.");
            }
          },
          weak_factory_.GetWeakPtr(), std::move(loader), std::move(callback)),
      1024 * 1024 /* 1MB limit */);
}

XenonNodeExecutor* XenonServiceImpl::EnsureNodeExecutor() {
  if (node_executor_) {
    return node_executor_.get();
  }

  node_executor_ = std::make_unique<XenonNodeExecutor>();
  node_executor_->SetCallbackHandlers(
      base::BindRepeating(&XenonServiceImpl::OnNodeCallback,
                          weak_factory_.GetWeakPtr()),
      base::BindRepeating(&XenonServiceImpl::OnNodeCallbackReleased,
                          weak_factory_.GetWeakPtr()));
  return node_executor_.get();
}

void XenonServiceImpl::OnNodeCallback(int32_t client_id,
                                      int32_t callback_id,
                                      std::vector<base::Value> args) {
  auto observer = node_addon_observers_.find(client_id);
  if (observer == node_addon_observers_.end() ||
      !observer->second.is_connected()) {
    return;
  }
  observer->second->OnCallback(callback_id, std::move(args));
}

void XenonServiceImpl::OnNodeCallbackReleased(int32_t client_id,
                                              int32_t callback_id) {
  auto observer = node_addon_observers_.find(client_id);
  if (observer == node_addon_observers_.end() ||
      !observer->second.is_connected()) {
    return;
  }
  observer->second->OnCallbackReleased(callback_id);
}

void XenonServiceImpl::OnNodeAddonObserverDisconnected(int32_t client_id) {
  node_addon_observers_.erase(client_id);
}

void XenonServiceImpl::LoadAddon(const std::string& path,
                                 LoadAddonCallback callback) {
  EnsureNodeExecutor()->LoadAddon(
      path, mojo::WrapCallbackWithDefaultInvokeIfNotRun(
                std::move(callback), false,
                "LoadAddon callback dropped before completion",
                std::vector<mojom::NodeExportInfoPtr>()));
}

void XenonServiceImpl::InspectExport(const std::string& module_path,
                                     const std::string& export_path,
                                     InspectExportCallback callback) {
  if (!node_executor_) {
    std::move(callback).Run(false, "No Node addon has been loaded", nullptr);
    return;
  }
  node_executor_->InspectExport(
      module_path, export_path,
      mojo::WrapCallbackWithDefaultInvokeIfNotRun(
          std::move(callback), false,
          "InspectExport callback dropped before completion",
          mojom::NodeExportInfoPtr()));
}

void XenonServiceImpl::ConstructExport(
    int32_t client_id,
    const std::string& module_path,
    const std::string& export_path,
    std::vector<mojom::NodeInvokeArgPtr> args,
    ConstructExportCallback callback) {
  if (!node_executor_) {
    std::move(callback).Run(false, 0, "No Node addon has been loaded");
    return;
  }
  node_executor_->ConstructExport(
      module_path, export_path, client_id, std::move(args),
      mojo::WrapCallbackWithDefaultInvokeIfNotRun(
          std::move(callback), false, 0,
          "ConstructExport callback dropped before completion"));
}

void XenonServiceImpl::InvokeInstance(int32_t client_id,
                                      const std::string& module_path,
                                      int32_t instance_id,
                                      const std::string& method_name,
                                      std::vector<mojom::NodeInvokeArgPtr> args,
                                      InvokeInstanceCallback callback) {
  if (!node_executor_) {
    std::move(callback).Run(false, base::Value(), {},
                            "No Node addon has been loaded");
    return;
  }
  node_executor_->InvokeInstance(
      module_path, instance_id, method_name, client_id, std::move(args),
      mojo::WrapCallbackWithDefaultInvokeIfNotRun(
          std::move(callback), false, base::Value(),
          std::vector<mojom::NodeCallbackResultPtr>(),
          "InvokeInstance callback dropped before completion"));
}

void XenonServiceImpl::GetInstanceProperty(
    const std::string& module_path,
    int32_t instance_id,
    const std::string& property_name,
    GetInstancePropertyCallback callback) {
  if (!node_executor_) {
    std::move(callback).Run(false, base::Value(),
                            "No Node addon has been loaded");
    return;
  }
  node_executor_->GetInstanceProperty(
      module_path, instance_id, property_name,
      mojo::WrapCallbackWithDefaultInvokeIfNotRun(
          std::move(callback), false, base::Value(),
          "GetInstanceProperty callback dropped before completion"));
}

void XenonServiceImpl::SetInstanceProperty(
    const std::string& module_path,
    int32_t instance_id,
    const std::string& property_name,
    base::Value value,
    SetInstancePropertyCallback callback) {
  if (!node_executor_) {
    std::move(callback).Run(false, "No Node addon has been loaded");
    return;
  }
  node_executor_->SetInstanceProperty(
      module_path, instance_id, property_name, std::move(value),
      mojo::WrapCallbackWithDefaultInvokeIfNotRun(
          std::move(callback), false,
          "SetInstanceProperty callback dropped before completion"));
}

void XenonServiceImpl::ReleaseInstance(const std::string& module_path,
                                       int32_t instance_id) {
  if (node_executor_) {
    node_executor_->ReleaseInstance(module_path, instance_id);
  }
}

void XenonServiceImpl::InvokeFunction(int32_t client_id,
                                      const std::string& module_path,
                                      const std::string& function_name,
                                      std::vector<mojom::NodeInvokeArgPtr> args,
                                      InvokeFunctionCallback callback) {
  if (!node_executor_) {
    std::move(callback).Run(false, base::Value(), {},
                            "No Node addon has been loaded");
    return;
  }
  node_executor_->InvokeFunction(
      module_path, function_name, client_id, std::move(args),
      mojo::WrapCallbackWithDefaultInvokeIfNotRun(
          std::move(callback), false, base::Value(),
          std::vector<mojom::NodeCallbackResultPtr>(),
          "InvokeFunction callback dropped before completion"));
}

void XenonServiceImpl::GetExportProperty(const std::string& module_path,
                                         const std::string& object_path,
                                         const std::string& property_name,
                                         GetExportPropertyCallback callback) {
  if (!node_executor_) {
    std::move(callback).Run(false, base::Value(),
                            "No Node addon has been loaded");
    return;
  }
  node_executor_->GetExportProperty(
      module_path, object_path, property_name,
      mojo::WrapCallbackWithDefaultInvokeIfNotRun(
          std::move(callback), false, base::Value(),
          "GetExportProperty callback dropped before completion"));
}

void XenonServiceImpl::SetExportProperty(const std::string& module_path,
                                         const std::string& object_path,
                                         const std::string& property_name,
                                         base::Value value,
                                         SetExportPropertyCallback callback) {
  if (!node_executor_) {
    std::move(callback).Run(false, "No Node addon has been loaded");
    return;
  }
  node_executor_->SetExportProperty(
      module_path, object_path, property_name, std::move(value),
      mojo::WrapCallbackWithDefaultInvokeIfNotRun(
          std::move(callback), false,
          "SetExportProperty callback dropped before completion"));
}

void XenonServiceImpl::InvokeMany(const std::string& module_path,
                                  std::vector<mojom::NodeInvokeCallPtr> calls,
                                  InvokeManyCallback callback) {
  if (!node_executor_) {
    std::move(callback).Run(
        MakeInvokeFailures(calls.size(), "No Node addon has been loaded"));
    return;
  }
  const size_t call_count = calls.size();
  node_executor_->InvokeMany(
      module_path, std::move(calls),
      mojo::WrapCallbackWithDefaultInvokeIfNotRun(
          std::move(callback),
          MakeInvokeFailures(call_count,
                             "InvokeMany callback dropped before completion")));
}

}  // namespace xenon
