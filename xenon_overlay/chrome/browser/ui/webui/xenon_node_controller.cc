// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ui/webui/xenon_node_controller.h"

#include <mutex>
#include <utility>

#include "base/atomic_sequence_num.h"
#include "base/functional/bind.h"
#include "base/strings/string_number_conversions.h"
#include "build/build_config.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_browser_interface_broker_registry.h"
#include "content/public/browser/web_ui_data_source.h"
#include "content/public/common/url_constants.h"
#include "mojo/public/cpp/bindings/callback_helpers.h"
#include "ui/views/widget/widget.h"
#include "xenon_overlay/chrome/browser/ui/xenon_web_dialog.h"
#include "xenon_overlay/chrome/browser/xenon_manager.h"
#include "xenon_overlay/public/mojom/xenon_service.mojom.h"
#include "xenon_overlay/resources/webui/xenon_node/grit/xenon_node_webui_resources.h"
#include "xenon_overlay/resources/webui/xenon_node/grit/xenon_node_webui_resources_map.h"

#if BUILDFLAG(IS_WIN)
#include "ui/views/win/hwnd_util.h"
#endif

namespace xenon {

namespace {

constexpr char kHost[] = "xenon-node";
constexpr char kServiceRestartingError[] =
    "Utility service restarted; retry the operation";

base::AtomicSequenceNumber& NodeClientIdSequence() {
  static base::AtomicSequenceNumber sequence;
  return sequence;
}

xenon_node::mojom::NodeExportInfoPtr ToPageExportInfo(
    const mojom::NodeExportInfoPtr& export_info) {
  auto page_info = xenon_node::mojom::NodeExportInfo::New();
  page_info->name = export_info->name;
  page_info->kind = export_info->kind;
  page_info->enumerable = export_info->enumerable;
  page_info->writable = export_info->writable;
  page_info->has_value = export_info->has_value;
  page_info->value = export_info->value.Clone();
  page_info->children.reserve(export_info->children.size());
  for (const auto& child : export_info->children) {
    page_info->children.push_back(ToPageExportInfo(child));
  }
  page_info->prototype.reserve(export_info->prototype.size());
  for (const auto& member : export_info->prototype) {
    page_info->prototype.push_back(ToPageExportInfo(member));
  }
  return page_info;
}

std::vector<xenon_node::mojom::NodeExportInfoPtr> ToPageExportInfos(
    const std::vector<mojom::NodeExportInfoPtr>& exports) {
  std::vector<xenon_node::mojom::NodeExportInfoPtr> page_exports;
  page_exports.reserve(exports.size());
  for (const auto& export_info : exports) {
    page_exports.push_back(ToPageExportInfo(export_info));
  }
  return page_exports;
}

std::vector<mojom::NodeInvokeArgPtr> ToServiceInvokeArgs(
    std::vector<xenon_node::mojom::NodeInvokeArgPtr> args) {
  std::vector<mojom::NodeInvokeArgPtr> service_args;
  service_args.reserve(args.size());
  for (const auto& arg : args) {
    auto service_arg = mojom::NodeInvokeArg::New();
    service_arg->is_callback = arg->is_callback;
    service_arg->callback_id = arg->callback_id;
    service_arg->value = arg->value.Clone();
    service_args.push_back(std::move(service_arg));
  }
  return service_args;
}

std::vector<mojom::NodeInvokeCallResultPtr> MakeServiceInvokeFailures(
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

void EnsureTrustedBrokerKnowsXenonNode() {
  static std::once_flag once;
  std::call_once(once, [] {
    content::WebUIBrowserInterfaceBrokerRegistry::GetTrustedRegistry()
        .ForWebUI<XenonNodeController>()
        .Add<xenon_node::mojom::PageHandlerFactory>();
  });
}

}  // namespace

XenonNodeController::XenonNodeController(content::WebUI* web_ui)
    : ui::MojoWebUIController(web_ui, /*enable_chrome_send=*/false),
      client_id_(NodeClientIdSequence().GetNext() + 1) {
  EnsureTrustedBrokerKnowsXenonNode();

  content::WebUIDataSource* source = content::WebUIDataSource::CreateAndAdd(
      web_ui->GetWebContents()->GetBrowserContext(), kHost);

  for (const auto& resource : kXenonNodeWebuiResources) {
    source->AddResourcePath(resource.path, resource.id);
  }
  source->SetDefaultResource(IDR_XENON_NODE_WEBUI_XENON_NODE_HTML);
}

XenonNodeController::~XenonNodeController() {
  if (player_host_widget_) {
    player_host_widget_->CloseNow();
  }
}

WEB_UI_CONTROLLER_TYPE_IMPL(XenonNodeController)

void XenonNodeController::PreparePlayerHost(
    PreparePlayerHostCallback callback) {
#if BUILDFLAG(IS_WIN)
  if (!player_host_widget_) {
    base::DictValue options;
    options.Set("title", "Xenon Player");
    options.Set("width", 960);
    options.Set("height", 540);
    options.Set("modal", false);
    options.Set("frame", true);
    options.Set("resizable", true);
    options.Set("minimizable", true);
    options.Set("maximizable", true);
    options.Set("showCloseButton", true);

    XenonWebDialog::ShowWithOptions(
        web_ui()->GetWebContents()->GetBrowserContext(), GURL("about:blank"),
        options, &player_host_widget_, gfx::NativeView(),
        base::BindOnce(&XenonNodeController::OnPlayerHostClosed,
                       weak_ptr_factory_.GetWeakPtr()));
  } else {
    player_host_widget_->Show();
    player_host_widget_->Activate();
  }

  if (!player_host_widget_ || !player_host_widget_->GetNativeWindow()) {
    std::move(callback).Run("", "", "Failed to create player host window");
    return;
  }

  HWND parent_window =
      views::HWNDForNativeWindow(player_host_widget_->GetNativeWindow());
  HWND float_window = views::HWNDForNativeWindow(
      web_ui()->GetWebContents()->GetTopLevelNativeWindow());
  if (!parent_window) {
    std::move(callback).Run("", "", "Player host has no native window");
    return;
  }
  if (!float_window) {
    float_window = parent_window;
  }

  std::move(callback).Run(
      base::NumberToString(reinterpret_cast<uintptr_t>(float_window)),
      base::NumberToString(reinterpret_cast<uintptr_t>(parent_window)), "");
#else
  std::move(callback).Run("", "",
                          "Native player hosting is only supported on Windows");
#endif
}

void XenonNodeController::OnPlayerHostClosed() {
  player_host_widget_ = nullptr;
}

mojo::SharedRemote<mojom::XenonMainService>
XenonNodeController::GetBoundServiceRemote() {
  XenonManager* manager = XenonManager::GetInstance();
  manager->EnsureServiceStarted(
      web_ui()->GetWebContents()->GetBrowserContext());
  auto remote = manager->DuplicateServiceRemote();
  const uint64_t previous_generation = service_generation_;
  const bool service_changed =
      service_generation_ != manager->service_generation();
  if (service_changed) {
    service_generation_ = manager->service_generation();
    node_addon_observer_receiver_.reset();
    if (previous_generation != 0 && page_.is_bound()) {
      page_->NodeServiceReset();
    }
  }
  if (remote.is_bound() && page_.is_bound() &&
      !node_addon_observer_receiver_.is_bound()) {
    remote->SetNodeAddonObserver(
        client_id_, node_addon_observer_receiver_.BindNewPipeAndPassRemote());
    node_addon_observer_receiver_.set_disconnect_handler(
        base::BindOnce(&XenonNodeController::OnNodeAddonObserverDisconnected,
                       weak_ptr_factory_.GetWeakPtr()));
  }
  if (remote.is_bound() && service_changed) {
    ReplayLoadedModules(remote);
  }
  return remote;
}

void XenonNodeController::ReplayLoadedModules(
    const mojo::SharedRemote<mojom::XenonMainService>& remote) {
  pending_module_reloads_ = 0;
  reload_in_progress_ = false;
  if (loaded_module_paths_.empty()) {
    RunDeferredServiceOperations();
    return;
  }
  reload_in_progress_ = true;
  pending_module_reloads_ = loaded_module_paths_.size();
  const uint64_t replay_generation = service_generation_;
  for (const std::string& path : loaded_module_paths_) {
    remote->LoadAddon(
        path, mojo::WrapCallbackWithDefaultInvokeIfNotRun(
                  base::BindOnce(&XenonNodeController::OnNodeModuleReloaded,
                                 weak_ptr_factory_.GetWeakPtr(),
                                 replay_generation, path),
                  false, "Utility service disconnected during addon reload",
                  std::vector<mojom::NodeExportInfoPtr>()));
  }
}

void XenonNodeController::OnNodeModuleReloaded(
    uint64_t service_generation,
    const std::string& path,
    bool success,
    const std::string& error_msg,
    std::vector<mojom::NodeExportInfoPtr> exports) {
  if (service_generation != service_generation_) {
    return;
  }
  OnNodeModuleLoaded(path, success, error_msg, std::move(exports));
  CHECK_GT(pending_module_reloads_, 0u);
  --pending_module_reloads_;
  if (pending_module_reloads_ == 0) {
    reload_in_progress_ = false;
    RunDeferredServiceOperations();
  }
}

bool XenonNodeController::DeferUntilModulesReloaded(
    base::OnceClosure operation) {
  if (!reload_in_progress_) {
    return false;
  }
  deferred_service_operations_.push_back(std::move(operation));
  return true;
}

void XenonNodeController::RunDeferredServiceOperations() {
  std::vector<base::OnceClosure> operations;
  operations.swap(deferred_service_operations_);
  for (auto& operation : operations) {
    std::move(operation).Run();
  }
}

void XenonNodeController::OnNodeAddonObserverDisconnected() {
  node_addon_observer_receiver_.reset();
}

void XenonNodeController::BindInterface(
    mojo::PendingReceiver<xenon_node::mojom::PageHandlerFactory> receiver) {
  page_factory_receiver_.reset();
  page_factory_receiver_.Bind(std::move(receiver));
}

void XenonNodeController::CreatePageHandler(
    mojo::PendingRemote<xenon_node::mojom::Page> page,
    mojo::PendingReceiver<xenon_node::mojom::PageHandler> receiver) {
  page_.reset();
  page_.Bind(std::move(page));

  page_handler_receiver_.reset();
  page_handler_receiver_.Bind(std::move(receiver));

  node_addon_observer_receiver_.reset();
  GetBoundServiceRemote();
}

void XenonNodeController::RequireNodeModule(const std::string& path) {
  auto remote = GetBoundServiceRemote();
  if (reload_in_progress_) {
    DeferUntilModulesReloaded(
        base::BindOnce(&XenonNodeController::RequireNodeModule,
                       weak_ptr_factory_.GetWeakPtr(), path));
    return;
  }
  if (!remote.is_bound()) {
    if (page_.is_bound()) {
      page_->NodeModuleLoaded(path, false, "Utility service is not running",
                              {});
    }
    return;
  }

  remote->LoadAddon(path,
                    mojo::WrapCallbackWithDefaultInvokeIfNotRun(
                        base::BindOnce(&XenonNodeController::OnNodeModuleLoaded,
                                       weak_ptr_factory_.GetWeakPtr(), path),
                        false, "Utility service disconnected during addon load",
                        std::vector<mojom::NodeExportInfoPtr>()));
}

void XenonNodeController::OnNodeModuleLoaded(
    const std::string& path,
    bool success,
    const std::string& error_msg,
    std::vector<mojom::NodeExportInfoPtr> exports) {
  if (!page_.is_bound()) {
    return;
  }
  if (success) {
    loaded_module_paths_.insert(path);
  } else {
    loaded_module_paths_.erase(path);
  }
  page_->NodeModuleLoaded(path, success, error_msg,
                          ToPageExportInfos(exports));
}

void XenonNodeController::InvokeNodeExport(
    int32_t request_id,
    const std::string& module_path,
    const std::string& function_name,
    std::vector<xenon_node::mojom::NodeInvokeArgPtr> args) {
  auto remote = GetBoundServiceRemote();
  if (reload_in_progress_) {
    if (page_.is_bound()) {
      page_->NodeInvokeResult(request_id, false, base::Value(), {},
                              kServiceRestartingError);
    }
    return;
  }
  if (!remote.is_bound()) {
    if (page_.is_bound()) {
      page_->NodeInvokeResult(request_id, false, base::Value(), {},
                              "Utility service is not running");
    }
    return;
  }

  remote->InvokeFunction(
      client_id_, module_path, function_name,
      ToServiceInvokeArgs(std::move(args)),
      mojo::WrapCallbackWithDefaultInvokeIfNotRun(
          base::BindOnce(&XenonNodeController::OnNodeExportInvoked,
                         weak_ptr_factory_.GetWeakPtr(), request_id),
          false, base::Value(), std::vector<mojom::NodeCallbackResultPtr>(),
          "Utility service disconnected during native invocation"));
}

void XenonNodeController::OnNodeExportInvoked(
    int32_t request_id,
    bool success,
    base::Value result,
    std::vector<mojom::NodeCallbackResultPtr> callback_results,
    const std::string& error_msg) {
  if (!page_.is_bound()) {
    return;
  }

  std::vector<xenon_node::mojom::NodeCallbackResultPtr> page_results;
  page_results.reserve(callback_results.size());
  for (const auto& callback_result : callback_results) {
    auto page_result = xenon_node::mojom::NodeCallbackResult::New();
    page_result->callback_id = callback_result->callback_id;
    page_result->value = callback_result->value.Clone();
    page_results.push_back(std::move(page_result));
  }

  page_->NodeInvokeResult(request_id, success, std::move(result),
                          std::move(page_results), error_msg);
}

void XenonNodeController::InspectNodeExport(int32_t request_id,
                                            const std::string& module_path,
                                            const std::string& export_path) {
  auto remote = GetBoundServiceRemote();
  if (reload_in_progress_) {
    if (page_.is_bound()) {
      page_->NodeInspectResult(request_id, false, kServiceRestartingError,
                               nullptr);
    }
    return;
  }
  if (!remote.is_bound()) {
    if (page_.is_bound()) {
      page_->NodeInspectResult(request_id, false,
                               "Utility service is not running", nullptr);
    }
    return;
  }

  remote->InspectExport(
      module_path, export_path,
      mojo::WrapCallbackWithDefaultInvokeIfNotRun(
          base::BindOnce(&XenonNodeController::OnNodeExportInspected,
                         weak_ptr_factory_.GetWeakPtr(), request_id),
          false, "Utility service disconnected during export inspection",
          mojom::NodeExportInfoPtr()));
}

void XenonNodeController::OnNodeExportInspected(
    int32_t request_id,
    bool success,
    const std::string& error_msg,
    mojom::NodeExportInfoPtr info) {
  if (!page_.is_bound()) {
    return;
  }
  xenon_node::mojom::NodeExportInfoPtr page_info;
  if (info) {
    page_info = ToPageExportInfo(info);
  }
  page_->NodeInspectResult(request_id, success, error_msg, std::move(page_info));
}

void XenonNodeController::ConstructNodeExport(
    int32_t request_id,
    const std::string& module_path,
    const std::string& export_path,
    std::vector<xenon_node::mojom::NodeInvokeArgPtr> args) {
  auto remote = GetBoundServiceRemote();
  if (reload_in_progress_) {
    if (page_.is_bound()) {
      page_->NodeConstructResult(request_id, false, 0, kServiceRestartingError);
    }
    return;
  }
  if (!remote.is_bound()) {
    if (page_.is_bound()) {
      page_->NodeConstructResult(request_id, false, 0,
                                 "Utility service is not running");
    }
    return;
  }

  remote->ConstructExport(
      client_id_, module_path, export_path,
      ToServiceInvokeArgs(std::move(args)),
      mojo::WrapCallbackWithDefaultInvokeIfNotRun(
          base::BindOnce(&XenonNodeController::OnNodeExportConstructed,
                         weak_ptr_factory_.GetWeakPtr(), request_id),
          false, 0, "Utility service disconnected during native construction"));
}

void XenonNodeController::OnNodeExportConstructed(
    int32_t request_id,
    bool success,
    int32_t instance_id,
    const std::string& error_msg) {
  if (!page_.is_bound()) {
    return;
  }
  page_->NodeConstructResult(request_id, success, instance_id, error_msg);
}

void XenonNodeController::InvokeNodeInstance(
    int32_t request_id,
    const std::string& module_path,
    int32_t instance_id,
    const std::string& method_name,
    std::vector<xenon_node::mojom::NodeInvokeArgPtr> args) {
  auto remote = GetBoundServiceRemote();
  if (reload_in_progress_) {
    if (page_.is_bound()) {
      page_->NodeInvokeResult(request_id, false, base::Value(), {},
                              kServiceRestartingError);
    }
    return;
  }
  if (!remote.is_bound()) {
    if (page_.is_bound()) {
      page_->NodeInvokeResult(request_id, false, base::Value(), {},
                              "Utility service is not running");
    }
    return;
  }

  remote->InvokeInstance(
      client_id_, module_path, instance_id, method_name,
      ToServiceInvokeArgs(std::move(args)),
      mojo::WrapCallbackWithDefaultInvokeIfNotRun(
          base::BindOnce(&XenonNodeController::OnNodeExportInvoked,
                         weak_ptr_factory_.GetWeakPtr(), request_id),
          false, base::Value(), std::vector<mojom::NodeCallbackResultPtr>(),
          "Utility service disconnected during native instance invocation"));
}

void XenonNodeController::GetNodeInstanceProperty(
    int32_t request_id,
    const std::string& module_path,
    int32_t instance_id,
    const std::string& property_name) {
  auto remote = GetBoundServiceRemote();
  if (reload_in_progress_) {
    OnNodePropertyRead(request_id, false, base::Value(),
                       kServiceRestartingError);
    return;
  }
  if (!remote.is_bound()) {
    OnNodePropertyRead(request_id, false, base::Value(),
                       "Utility service is not running");
    return;
  }
  remote->GetInstanceProperty(
      module_path, instance_id, property_name,
      mojo::WrapCallbackWithDefaultInvokeIfNotRun(
          base::BindOnce(&XenonNodeController::OnNodePropertyRead,
                         weak_ptr_factory_.GetWeakPtr(), request_id),
          false, base::Value(),
          "Utility service disconnected during property read"));
}

void XenonNodeController::SetNodeInstanceProperty(
    int32_t request_id,
    const std::string& module_path,
    int32_t instance_id,
    const std::string& property_name,
    base::Value value) {
  auto remote = GetBoundServiceRemote();
  if (reload_in_progress_) {
    OnNodePropertyWritten(request_id, false, kServiceRestartingError);
    return;
  }
  if (!remote.is_bound()) {
    OnNodePropertyWritten(request_id, false, "Utility service is not running");
    return;
  }
  remote->SetInstanceProperty(
      module_path, instance_id, property_name, std::move(value),
      mojo::WrapCallbackWithDefaultInvokeIfNotRun(
          base::BindOnce(&XenonNodeController::OnNodePropertyWritten,
                         weak_ptr_factory_.GetWeakPtr(), request_id),
          false, "Utility service disconnected during property write"));
}

void XenonNodeController::ReleaseNodeInstance(const std::string& module_path,
                                              int32_t instance_id) {
  auto remote = GetBoundServiceRemote();
  if (reload_in_progress_) {
    return;
  }
  if (remote.is_bound()) {
    remote->ReleaseInstance(module_path, instance_id);
  }
}

void XenonNodeController::GetNodeExportProperty(
    int32_t request_id,
    const std::string& module_path,
    const std::string& object_path,
    const std::string& property_name) {
  auto remote = GetBoundServiceRemote();
  if (reload_in_progress_) {
    OnNodePropertyRead(request_id, false, base::Value(),
                       kServiceRestartingError);
    return;
  }
  if (!remote.is_bound()) {
    OnNodePropertyRead(request_id, false, base::Value(),
                       "Utility service is not running");
    return;
  }
  remote->GetExportProperty(
      module_path, object_path, property_name,
      mojo::WrapCallbackWithDefaultInvokeIfNotRun(
          base::BindOnce(&XenonNodeController::OnNodePropertyRead,
                         weak_ptr_factory_.GetWeakPtr(), request_id),
          false, base::Value(),
          "Utility service disconnected during export property read"));
}

void XenonNodeController::SetNodeExportProperty(
    int32_t request_id,
    const std::string& module_path,
    const std::string& object_path,
    const std::string& property_name,
    base::Value value) {
  auto remote = GetBoundServiceRemote();
  if (reload_in_progress_) {
    OnNodePropertyWritten(request_id, false, kServiceRestartingError);
    return;
  }
  if (!remote.is_bound()) {
    OnNodePropertyWritten(request_id, false, "Utility service is not running");
    return;
  }
  remote->SetExportProperty(
      module_path, object_path, property_name, std::move(value),
      mojo::WrapCallbackWithDefaultInvokeIfNotRun(
          base::BindOnce(&XenonNodeController::OnNodePropertyWritten,
                         weak_ptr_factory_.GetWeakPtr(), request_id),
          false, "Utility service disconnected during export property write"));
}

void XenonNodeController::OnCallback(int32_t callback_id,
                                     std::vector<base::Value> args) {
  if (page_.is_bound()) {
    page_->NodeCallbackInvoked(callback_id, std::move(args));
  }
}

void XenonNodeController::OnCallbackReleased(int32_t callback_id) {
  if (page_.is_bound()) {
    page_->NodeCallbackReleased(callback_id);
  }
}

void XenonNodeController::OnNodePropertyRead(int32_t request_id,
                                             bool success,
                                             base::Value result,
                                             const std::string& error_msg) {
  if (page_.is_bound()) {
    page_->NodePropertyResult(request_id, success, std::move(result),
                              error_msg);
  }
}

void XenonNodeController::OnNodePropertyWritten(int32_t request_id,
                                                bool success,
                                                const std::string& error_msg) {
  if (page_.is_bound()) {
    page_->NodeSetPropertyResult(request_id, success, error_msg);
  }
}

void XenonNodeController::InvokeNodeExports(
    int32_t request_id,
    const std::string& module_path,
    std::vector<xenon_node::mojom::NodeInvokeCallPtr> calls) {
  auto remote = GetBoundServiceRemote();
  if (reload_in_progress_) {
    if (page_.is_bound()) {
      OnNodeExportsInvoked(
          request_id,
          MakeServiceInvokeFailures(calls.size(), kServiceRestartingError));
    }
    return;
  }
  if (!remote.is_bound()) {
    if (page_.is_bound()) {
      OnNodeExportsInvoked(request_id,
                           MakeServiceInvokeFailures(
                               calls.size(), "Utility service is not running"));
    }
    return;
  }

  std::vector<mojom::NodeInvokeCallPtr> service_calls;
  service_calls.reserve(calls.size());
  for (auto& call : calls) {
    auto service_call = mojom::NodeInvokeCall::New();
    service_call->function_name = call->function_name;
    service_call->args = ToServiceInvokeArgs(std::move(call->args));
    service_calls.push_back(std::move(service_call));
  }

  const size_t call_count = service_calls.size();
  remote->InvokeMany(
      module_path, std::move(service_calls),
      mojo::WrapCallbackWithDefaultInvokeIfNotRun(
          base::BindOnce(&XenonNodeController::OnNodeExportsInvoked,
                         weak_ptr_factory_.GetWeakPtr(), request_id),
          MakeServiceInvokeFailures(call_count,
                                    "Utility service disconnected during "
                                    "batched native invocation")));
}

void XenonNodeController::OnNodeExportsInvoked(
    int32_t request_id,
    std::vector<mojom::NodeInvokeCallResultPtr> results) {
  if (!page_.is_bound()) {
    return;
  }

  std::vector<xenon_node::mojom::NodeInvokeCallResultPtr> page_results;
  page_results.reserve(results.size());
  for (auto& result : results) {
    auto page_result = xenon_node::mojom::NodeInvokeCallResult::New();
    page_result->success = result->success;
    page_result->result = std::move(result->result);
    page_result->error_msg = result->error_msg;
    page_result->callback_results.reserve(result->callback_results.size());
    for (auto& callback_result : result->callback_results) {
      auto page_cb = xenon_node::mojom::NodeCallbackResult::New();
      page_cb->callback_id = callback_result->callback_id;
      page_cb->value = std::move(callback_result->value);
      page_result->callback_results.push_back(std::move(page_cb));
    }
    page_results.push_back(std::move(page_result));
  }

  page_->NodeInvokeManyResult(request_id, std::move(page_results));
}

XenonNodeConfig::XenonNodeConfig()
    : content::WebUIConfig(content::kChromeUIScheme, kHost) {}

XenonNodeConfig::~XenonNodeConfig() = default;

std::unique_ptr<content::WebUIController> XenonNodeConfig::CreateWebUIController(
    content::WebUI* web_ui,
    const GURL& url) {
  return std::make_unique<XenonNodeController>(web_ui);
}

}  // namespace xenon
