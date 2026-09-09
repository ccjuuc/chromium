// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_UI_WEBUI_XENON_NODE_CONTROLLER_H_
#define XENON_OVERLAY_CHROME_BROWSER_UI_WEBUI_XENON_NODE_CONTROLLER_H_

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/files/file_path.h"
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

// WebUI controller for the Node test and player hosts.
enum class XenonNodeHostKind {
  kNodeTest,
  kPlayer,
};

class XenonNodeController : public ui::MojoWebUIController,
                             public xenon_node::mojom::PageHandlerFactory,
                             public xenon_node::mojom::PageHandler,
                             public mojom::NodeAddonObserver,
                             public views::WidgetObserver {
 public:
  explicit XenonNodeController(content::WebUI* web_ui,
                               XenonNodeHostKind host_kind =
                                   XenonNodeHostKind::kNodeTest);
  ~XenonNodeController() override;

  XenonNodeController(const XenonNodeController&) = delete;
  XenonNodeController& operator=(const XenonNodeController&) = delete;

  void BindInterface(
      mojo::PendingReceiver<xenon_node::mojom::PageHandlerFactory> receiver);

 private:
  // xenon_node::mojom::PageHandlerFactory:
  void CreatePageHandler(
      mojo::PendingRemote<xenon_node::mojom::Page> page,
      mojo::PendingReceiver<xenon_node::mojom::PageHandler> receiver) override;

  // xenon_node::mojom::PageHandler:
  void PreparePlayerHost(PreparePlayerHostCallback callback) override;
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
  void OnWidgetVisibilityChanged(views::Widget* widget,
                                 bool visible) override;

  void OnNodeModuleLoaded(
      const std::string& path,
      bool success,
      const std::string& error_msg,
      std::vector<mojom::NodeExportInfoPtr> exports);
  void OnNodeExportInvoked(int32_t request_id,
                           bool success,
                           base::Value result,
                           std::vector<xenon::mojom::NodeCallbackResultPtr>
                               callback_results,
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
                          base::Value result,
                          const std::string& error_msg);
  void OnNodePropertyWritten(int32_t request_id,
                             bool success,
                             const std::string& error_msg);
  void OnPlayerHostClosed();
  void DetachPlayerControlWindow();
  void SyncPlayerHostWindow();
  void UpdatePlayerWindow();

  // Ensures the Utility process is up, then returns a SharedRemote copy (same
  // path as require / xenon_page_handler).
  mojo::SharedRemote<mojom::XenonMainService> GetBoundServiceRemote();
  void OnNodeAddonObserverDisconnected();
  void ReplayLoadedModules(
      const mojo::SharedRemote<mojom::XenonMainService>& remote);
  void OnNodeModuleReloaded(uint64_t service_generation,
                            const std::string& path,
                            bool success,
                            const std::string& error_msg,
                            std::vector<mojom::NodeExportInfoPtr> exports);
  bool DeferUntilModulesReloaded(base::OnceClosure operation);
  void RunDeferredServiceOperations();

  mojo::Receiver<xenon_node::mojom::PageHandlerFactory>
      page_factory_receiver_{this};
  mojo::Receiver<xenon_node::mojom::PageHandler> page_handler_receiver_{this};
  mojo::Remote<xenon_node::mojom::Page> page_;
  mojo::Receiver<mojom::NodeAddonObserver> node_addon_observer_receiver_{this};
  const int32_t client_id_;
  const std::string node_context_id_;
  uint64_t service_generation_ = 0;
  std::set<std::string> loaded_module_paths_;
  size_t pending_module_reloads_ = 0;
  bool reload_in_progress_ = false;
  std::vector<base::OnceClosure> deferred_service_operations_;
  raw_ptr<views::Widget> player_widget_ = nullptr;
  raw_ptr<views::Widget> player_host_widget_ = nullptr;
  uintptr_t player_window_ = 0;
  bool player_window_requested_visible_ = false;
  bool syncing_player_windows_ = false;
  base::FilePath player_frontend_dir_;
  const XenonNodeHostKind host_kind_;

  base::WeakPtrFactory<XenonNodeController> weak_ptr_factory_{this};

  WEB_UI_CONTROLLER_TYPE_DECL();
};

class XenonNodeConfig : public content::WebUIConfig {
 public:
  XenonNodeConfig();
  ~XenonNodeConfig() override;

  std::unique_ptr<content::WebUIController> CreateWebUIController(
      content::WebUI* web_ui,
      const GURL& url) override;
};

class XenonPlayerConfig : public content::WebUIConfig {
 public:
  XenonPlayerConfig();
  ~XenonPlayerConfig() override;

  std::unique_ptr<content::WebUIController> CreateWebUIController(
      content::WebUI* web_ui,
      const GURL& url) override;
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_UI_WEBUI_XENON_NODE_CONTROLLER_H_
