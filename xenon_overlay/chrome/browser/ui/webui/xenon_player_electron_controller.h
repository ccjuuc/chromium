// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_UI_WEBUI_XENON_PLAYER_ELECTRON_CONTROLLER_H_
#define XENON_OVERLAY_CHROME_BROWSER_UI_WEBUI_XENON_PLAYER_ELECTRON_CONTROLLER_H_

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "base/files/file_path.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/webui_config.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/bindings/shared_remote.h"
#include "ui/views/widget/widget_observer.h"
#include "ui/webui/mojo_web_ui_controller.h"
#include "xenon_overlay/chrome/browser/ui/webui/xenon_node.mojom.h"
#include "xenon_overlay/public/mojom/xenon_service.mojom.h"

namespace views {
class Widget;
}

namespace xenon {

class XenonPlayerElectronController
    : public ui::MojoWebUIController,
      public xenon_node::mojom::PageHandlerFactory,
      public xenon_node::mojom::PageHandler,
      public mojom::NodeAddonObserver,
      public views::WidgetObserver {
 public:
  explicit XenonPlayerElectronController(content::WebUI* web_ui);
  XenonPlayerElectronController(content::WebUI* web_ui,
                                std::string_view webui_host,
                                std::string_view frontend_dir_switch,
                                std::string_view frontend_dir_name,
                                std::vector<std::string> cors_domains);
  ~XenonPlayerElectronController() override;

  XenonPlayerElectronController(const XenonPlayerElectronController&) = delete;
  XenonPlayerElectronController& operator=(
      const XenonPlayerElectronController&) = delete;

  void BindInterface(
      mojo::PendingReceiver<xenon_node::mojom::PageHandlerFactory> receiver);

  WEB_UI_CONTROLLER_TYPE_DECL();

 private:
  // xenon_node::mojom::PageHandlerFactory:
  void CreatePageHandler(
      mojo::PendingRemote<xenon_node::mojom::Page> page,
      mojo::PendingReceiver<xenon_node::mojom::PageHandler> receiver) override;

  // xenon_node::mojom::PageHandler:
  void PreparePlayerHost(PreparePlayerHostCallback callback) override;
  void DoPreparePlayerHost(PreparePlayerHostCallback callback);
  void BindPlayerVideoWindow(
      const std::string& player_window,
      BindPlayerVideoWindowCallback callback) override;
  void ShowPlayerVideoHost(bool show) override;
  void ControlPlayerWindow(const std::string& action,
                           bool flag,
                           ControlPlayerWindowCallback callback) override;
  void OpenNativeFileDialog(
      const std::string& title,
      const std::vector<std::string>& filter_extensions,
      bool allow_multi,
      OpenNativeFileDialogCallback callback) override;
  void ScanDirectoryVideos(const std::string& dir_path,
                           ScanDirectoryVideosCallback callback) override;
  void RequireNodeModule(const std::string& path) override;
  void InvokeNodeExport(int32_t request_id,
                        const std::string& module_path,
                        const std::string& function_name,
                        std::vector<xenon_node::mojom::NodeInvokeArgPtr> args)
      override;
  void InspectNodeExport(int32_t request_id,
                         const std::string& module_path,
                         const std::string& export_path) override;
  void ConstructNodeExport(
      int32_t request_id,
      const std::string& module_path,
      const std::string& export_path,
      std::vector<xenon_node::mojom::NodeInvokeArgPtr> args) override;
  void InvokeNodeInstance(
      int32_t request_id,
      const std::string& module_path,
      int32_t instance_id,
      const std::string& method_name,
      std::vector<xenon_node::mojom::NodeInvokeArgPtr> args) override;
  void GetNodeInstanceProperty(int32_t request_id,
                               const std::string& module_path,
                               int32_t instance_id,
                               const std::string& property_name) override;
  void SetNodeInstanceProperty(int32_t request_id,
                               const std::string& module_path,
                               int32_t instance_id,
                               const std::string& property_name,
                               base::Value value) override;
  void ReleaseNodeInstance(const std::string& module_path,
                           int32_t instance_id) override;
  void GetNodeExportProperty(int32_t request_id,
                             const std::string& module_path,
                             const std::string& object_path,
                             const std::string& property_name) override;
  void SetNodeExportProperty(int32_t request_id,
                             const std::string& module_path,
                             const std::string& object_path,
                             const std::string& property_name,
                             base::Value value) override;
  void InvokeNodeExports(
      int32_t request_id,
      const std::string& module_path,
      std::vector<xenon_node::mojom::NodeInvokeCallPtr> calls) override;

  // mojom::NodeAddonObserver:
  void OnCallback(int32_t callback_id, std::vector<base::Value> args) override;
  void OnCallbackReleased(int32_t callback_id) override;

  // views::WidgetObserver:
  void OnWidgetActivationChanged(views::Widget* widget, bool active) override;
  void OnWidgetBoundsChanged(views::Widget* widget,
                             const gfx::Rect& new_bounds) override;
  void OnWidgetDestroying(views::Widget* widget) override;
  void OnWidgetShowStateChanged(views::Widget* widget) override;
  void OnWidgetVisibilityChanged(views::Widget* widget, bool visible) override;

  mojo::SharedRemote<mojom::XenonMainService> GetBoundServiceRemote();
  void ReplayLoadedModules(
      const mojo::SharedRemote<mojom::XenonMainService>& remote);
  void OnNodeAddonObserverDisconnected();
  void UpdatePlayerWindow();
  void SyncPlayerHostWindow();
  void DetachPlayerControlWindow();
  void OnPlayerHostClosed();

  void OnNodeModuleLoaded(const std::string& path,
                          bool success,
                          const std::string& error_msg,
                          std::vector<mojom::NodeExportInfoPtr> exports);
  void OnNodeExportInvoked(
      int32_t request_id,
      bool success,
      base::Value result,
      std::vector<mojom::NodeCallbackResultPtr> callback_results,
      const std::string& error_msg);
  void OnNodeExportInspected(int32_t request_id,
                             bool success,
                             const std::string& error_msg,
                             mojom::NodeExportInfoPtr info);
  void OnNodeExportConstructed(int32_t request_id,
                               bool success,
                               int32_t instance_id,
                               const std::string& error_msg);
  void OnNodeExportsInvoked(
      int32_t request_id,
      std::vector<mojom::NodeInvokeCallResultPtr> results);
  void OnNodePropertyRead(int32_t request_id,
                          bool success,
                          base::Value value,
                          const std::string& error_msg);
  void OnNodePropertyWritten(int32_t request_id,
                             bool success,
                             const std::string& error_msg);

  const int32_t client_id_;
  const std::string node_context_id_;
  base::FilePath player_frontend_dir_;
  mojo::Receiver<xenon_node::mojom::PageHandlerFactory> page_factory_receiver_{this};
  mojo::Receiver<xenon_node::mojom::PageHandler> page_handler_receiver_{this};
  mojo::Remote<xenon_node::mojom::Page> page_;
  mojo::Receiver<mojom::NodeAddonObserver> node_addon_observer_receiver_{this};
  std::set<std::string> loaded_module_paths_;
  uint64_t service_generation_ = 0;

  uintptr_t player_window_ = 0;
  bool player_window_requested_visible_ = false;
  bool syncing_player_windows_ = false;
  raw_ptr<views::Widget> player_host_widget_ = nullptr;
  raw_ptr<views::Widget> player_widget_ = nullptr;

  base::WeakPtrFactory<XenonPlayerElectronController> weak_ptr_factory_{this};
};

class XenonPlayerElectronConfig : public content::WebUIConfig {
 public:
  XenonPlayerElectronConfig();
  ~XenonPlayerElectronConfig() override;

  std::unique_ptr<content::WebUIController> CreateWebUIController(
      content::WebUI* web_ui,
      const GURL& url) override;
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_UI_WEBUI_XENON_PLAYER_ELECTRON_CONTROLLER_H_
