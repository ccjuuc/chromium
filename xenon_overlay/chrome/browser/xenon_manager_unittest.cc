// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/xenon_manager.h"

#include <memory>
#include <utility>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/test/task_environment.h"
#include "base/threading/thread_restrictions.h"
#include "build/buildflag.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"
#include "url/origin.h"
#include "xenon_overlay/buildflags/buildflags.h"
#include "xenon_overlay/common/asar/archive.h"
#include "xenon_overlay/common/asar/test_support.h"
#include "xenon_overlay/public/mojom/xenon_ipc.mojom.h"

namespace xenon {

TEST(XenonManagerTest, RegisterElectronIpcDoesNotStartService) {
  base::test::TaskEnvironment task_environment;

  constexpr char kContainerId[] = "lazy-registration-test";
  XenonManager* manager = XenonManager::GetInstance();
  auto config = ipc::mojom::IpcMainConfig::New();
  config->container_id = kContainerId;
  config->app_name = "Lazy Registration Test";

  ASSERT_TRUE(manager->RegisterElectronIpc(std::move(config)));
  EXPECT_EQ(0u, manager->service_generation(kContainerId));
  EXPECT_TRUE(manager->GetElectronIpcRendererConfigForContainer(kContainerId));
}

TEST(XenonManagerTest, FileRendererPermissionRequiresDeclaredMapping) {
  base::test::TaskEnvironment task_environment;
  XenonManager manager;
  base::FilePath absolute_root;
  ASSERT_TRUE(base::GetCurrentDirectory(&absolute_root));
  const auto register_mapping = [&manager](const std::string& container,
                                           const std::string& source,
                                           const std::string& target) {
    auto config = ipc::mojom::IpcMainConfig::New();
    config->container_id = container;
    auto mapping = ipc::mojom::IpcRendererUrlMapping::New();
    mapping->source_path_prefix = source;
    mapping->target_base_url = target;
    config->renderer_url_mappings.push_back(std::move(mapping));
    ASSERT_TRUE(manager.RegisterElectronIpc(std::move(config)));
  };
  register_mapping("mapped", absolute_root.AsUTF8Unsafe(),
                   "chrome://hosted-fixture/tools/");
  for (const char* url :
       {"chrome://hosted-fixture/tools/",
        "chrome://hosted-fixture/tools/clipper.html",
        "chrome://HOSTED-FIXTURE/tools/sub/page.html?q=1#x"}) {
    EXPECT_TRUE(manager.IsDeclaredFileRendererURL("mapped", GURL(url))) << url;
  }
  for (const char* url :
       {"chrome://hosted-fixture/tools",
        "chrome://hosted-fixture/tools-other/page.html",
        "chrome://hosted-fixture/Tools/page.html",
        "chrome://hosted-fixture/other/page.html",
        "chrome://hosted-fixture/tools/../other/page.html",
        "chrome://hosted-fixture/tools/%2e%2e/other.html",
        "chrome://hosted-fixture/tools/..%2fother.html",
        "chrome://hosted-fixture/tools/%5Cother.html",
        "chrome://other-host/tools/page.html",
        "https://hosted-fixture/tools/page.html",
        "chrome-untrusted://hosted-fixture/tools/page.html", "about:blank"}) {
    EXPECT_FALSE(manager.IsDeclaredFileRendererURL("mapped", GURL(url))) << url;
  }
  const GURL page("chrome://hosted-fixture/tools/page.html");
  EXPECT_FALSE(manager.IsDeclaredFileRendererURL("other-app", page));
  EXPECT_TRUE(manager.IsDeclaredFileRendererOrigin("mapped",
                                                   url::Origin::Create(page)));
  EXPECT_FALSE(manager.IsDeclaredFileRendererOrigin("other-app",
                                                    url::Origin::Create(page)));
  EXPECT_FALSE(manager.IsDeclaredFileRendererOrigin(
      "mapped", url::Origin::Create(GURL("https://hosted-fixture/"))));
  EXPECT_FALSE(manager.IsDeclaredFileRendererOrigin("mapped", url::Origin()));

  for (const std::string& source :
       {std::string("relative/source"), absolute_root.AppendASCII("..")
                                            .AppendASCII("outside")
                                            .AsUTF8Unsafe()}) {
    register_mapping("invalid-source", source,
                     "chrome://hosted-fixture/tools/");
    EXPECT_FALSE(manager.IsDeclaredFileRendererURL("invalid-source", page));
    EXPECT_FALSE(manager.IsDeclaredFileRendererOrigin(
        "invalid-source", url::Origin::Create(page)));
  }
  for (const char* target : {"https://hosted-fixture/tools/",
                             "chrome-untrusted://hosted-fixture/tools/",
                             "chrome://hosted-fixture/tools/?q=1",
                             "chrome://hosted-fixture/tools/#fragment"}) {
    register_mapping("invalid-target", absolute_root.AsUTF8Unsafe(), target);
    EXPECT_FALSE(manager.IsDeclaredFileRendererURL("invalid-target", page));
  }
  EXPECT_EQ(manager.service_generation("mapped"), 0u);
}

TEST(XenonManagerTest, ParentWindowPairingRequiresExactDeclaredURL) {
  base::test::TaskEnvironment task_environment;
  XenonManager manager;
  auto config = ipc::mojom::IpcMainConfig::New();
  EXPECT_TRUE(config->parent_window_pairing_urls.empty());
  config->container_id = "paired";
  config->parent_window_pairing_urls = {
      "https://APP.test:443/tools/overlay.html?declared=1#declared",
      "chrome://hosted-fixture/control.html",
      "file:///opt/example/control.html",
      "",
      "relative.html",
      "https://"};
  ASSERT_TRUE(manager.RegisterElectronIpc(std::move(config)));

  for (const char* url :
       {"https://app.test/tools/overlay.html",
        "https://app.test/tools/overlay.html?runtime=2#runtime",
        "https://app.test/tools/sub/../overlay.html",
        "chrome://hosted-fixture/control.html?parent=1#preview",
        "file:///opt/example/control.html?mode=1#preview"}) {
    EXPECT_TRUE(manager.ShouldPairElectronWindowWithParent("paired", GURL(url)))
        << url;
  }
  for (const char* url :
       {"https://app.test/tools/", "https://app.test/tools/overlay.html/child",
        "https://app.test/tools/overlay.html-other",
        "https://app.test/tools/Overlay.html",
        "https://other.test/tools/overlay.html",
        "http://app.test/tools/overlay.html",
        "https://app.test:8443/tools/overlay.html",
        "chrome://hosted-fixture/other.html",
        "chrome-untrusted://hosted-fixture/control.html",
        "file:///opt/example/control.html/child", "about:blank", "",
        "relative.html", "https://"}) {
    EXPECT_FALSE(
        manager.ShouldPairElectronWindowWithParent("paired", GURL(url)))
        << url;
  }

  const GURL page("https://app.test/tools/overlay.html");
  EXPECT_FALSE(
      manager.ShouldPairElectronWindowWithParent("unregistered", page));
  auto independent = ipc::mojom::IpcMainConfig::New();
  independent->container_id = "independent";
  ASSERT_TRUE(manager.RegisterElectronIpc(std::move(independent)));
  EXPECT_FALSE(manager.ShouldPairElectronWindowWithParent("independent", page));
  EXPECT_FALSE(manager.ShouldPairElectronWindowWithParent("", page));

  auto legacy = ipc::mojom::IpcMainConfig::New();
  legacy->parent_window_pairing_urls = {page.spec()};
  ASSERT_TRUE(manager.RegisterElectronIpc(std::move(legacy)));
  EXPECT_TRUE(manager.ShouldPairElectronWindowWithParent("", page));
  EXPECT_TRUE(manager.ShouldPairElectronWindowWithParent("default", page));

  auto replacement = ipc::mojom::IpcMainConfig::New();
  replacement->container_id = "paired";
  ASSERT_TRUE(manager.RegisterElectronIpc(std::move(replacement)));
  EXPECT_FALSE(manager.ShouldPairElectronWindowWithParent("paired", page));
  EXPECT_EQ(manager.service_generation("paired"), 0u);
}

TEST(XenonManagerTest, RuntimeMetadataIsCapturedOnceBeforeRendererRequests) {
  base::test::TaskEnvironment task_environment;
  XenonManager manager;
  base::FilePath working_directory;
  ASSERT_TRUE(base::GetCurrentDirectory(&working_directory));
  manager.InitializeRuntimeMetadata();

  constexpr char kContainerId[] = "runtime-metadata-test";
  auto config = ipc::mojom::IpcMainConfig::New();
  config->container_id = kContainerId;
  ASSERT_TRUE(manager.RegisterElectronIpc(std::move(config)));
  // Warm the existing PathService entries independently of the cwd snapshot.
  auto initial = manager.GetElectronIpcRendererConfigForContainer(kContainerId);
  ASSERT_TRUE(initial);
  EXPECT_EQ(initial->working_directory, working_directory.AsUTF8Unsafe());

  base::ScopedDisallowBlocking disallow_blocking;
  manager.InitializeRuntimeMetadata();
  auto renderer =
      manager.GetElectronIpcRendererConfigForContainer(kContainerId);
  ASSERT_TRUE(renderer);
  EXPECT_EQ(renderer->working_directory, initial->working_directory);
  EXPECT_EQ(manager.service_generation(kContainerId), 0u);
}

TEST(XenonManagerTest,
     RendererMetadataDoesNotReadWorkingDirectoryBeforeInitialization) {
  base::test::TaskEnvironment task_environment;
  XenonManager manager;
  constexpr char kContainerId[] = "uninitialized-runtime-metadata-test";
  auto config = ipc::mojom::IpcMainConfig::New();
  config->container_id = kContainerId;
  ASSERT_TRUE(manager.RegisterElectronIpc(std::move(config)));
  auto initial = manager.GetElectronIpcRendererConfigForContainer(kContainerId);
  ASSERT_TRUE(initial);
  EXPECT_TRUE(initial->working_directory.empty());

  base::ScopedDisallowBlocking disallow_blocking;
  auto renderer =
      manager.GetElectronIpcRendererConfigForContainer(kContainerId);
  ASSERT_TRUE(renderer);
  EXPECT_TRUE(renderer->working_directory.empty());
}

TEST(XenonManagerTest, ExplicitRendererRuntimeMetadataDoesNotDependOnMappings) {
  base::test::TaskEnvironment task_environment;
  XenonManager* manager = XenonManager::GetInstance();
  manager->InitializeRuntimeMetadata();
  base::ScopedTempDir temp;
  ASSERT_TRUE(temp.CreateUniqueTempDir());
  base::FilePath root;
  ASSERT_TRUE(base::NormalizeFilePath(temp.GetPath(), &root));
  const auto app = root.AppendASCII("app");
  const auto resources = root.AppendASCII("resources");
  const auto working_directory = root.AppendASCII("launch-directory");
  ASSERT_TRUE(base::CreateDirectory(app));
  ASSERT_TRUE(
      base::WriteFile(app.AppendASCII("package.json"),
                      R"({"name":"manifest-fixture","version":"1.0.0"})"));
  ASSERT_TRUE(base::CreateDirectory(resources));
  ASSERT_TRUE(base::CreateDirectory(working_directory));
  for (bool packaged : {true, false}) {
    SCOPED_TRACE(packaged);
    const std::string container =
        packaged ? "explicit-runtime-metadata-original-release-test"
                 : "explicit-runtime-metadata-mapped-source-test";
    auto config = ipc::mojom::IpcMainConfig::New();
    config->container_id = container;
    config->app_path = app.AsUTF8Unsafe();
    config->app_name = "Explicit fixture";
    config->app_version = "9.8.7";
    config->default_user_agent = "ExplicitFixture/9.8.7";
    config->resources_directory = resources.AsUTF8Unsafe();
    config->working_directory = working_directory.AsUTF8Unsafe();
    config->is_packaged = packaged;
    if (!packaged) {
      config->renderer_url_mappings.push_back(
          ipc::mojom::IpcRendererUrlMapping::New(app.AsUTF8Unsafe(),
                                                 "chrome://source-fixture/"));
    }
    ASSERT_TRUE(manager->RegisterElectronIpc(std::move(config)));
    const auto renderer =
        manager->GetElectronIpcRendererConfigForContainer(container);
    ASSERT_TRUE(renderer);
    EXPECT_EQ(packaged, renderer->is_packaged);
    EXPECT_EQ("Explicit fixture", renderer->app_name);
    EXPECT_EQ("9.8.7", renderer->app_version);
    EXPECT_EQ("ExplicitFixture/9.8.7", manager->GetDefaultUserAgent(container));
    EXPECT_EQ(app.AsUTF8Unsafe(), renderer->app_path);
    EXPECT_EQ(resources.AsUTF8Unsafe(), renderer->resources_directory);
    EXPECT_EQ(working_directory.AsUTF8Unsafe(), renderer->working_directory);
    EXPECT_EQ(packaged, renderer->renderer_url_mappings.empty());
    EXPECT_EQ(0u, manager->service_generation(container));
  }
}

TEST(XenonManagerTest,
     DiskApplicationDefaultsSurviveFailedArchiveKeyReplacement) {
  base::test::TaskEnvironment task_environment;
  XenonManager* manager = XenonManager::GetInstance();
  base::ScopedTempDir temp;
  ASSERT_TRUE(temp.CreateUniqueTempDir());
  base::FilePath release;
  ASSERT_TRUE(base::NormalizeFilePath(temp.GetPath(), &release));
  const auto resources = release.AppendASCII("resources");
  const auto archive_path = resources.AppendASCII("app.asar");
  ASSERT_TRUE(base::CreateDirectory(resources));
  asar::TestArchiveBuilder archive;
  ASSERT_TRUE(archive.Write(
      archive_path, {{"package.json",
                      R"({"name":"manifest-app","version":"3.2.1"})", true}}));

  constexpr char kContainerId[] = "packaged-defaults-key-rollback-test";
  auto config = ipc::mojom::IpcMainConfig::New();
  config->container_id = kContainerId;
  config->app_path = release.AsUTF8Unsafe();
  config->archive_public_keys.push_back(ipc::mojom::IpcArchivePublicKey::New(
      release.AsUTF8Unsafe(), archive.public_key_pem()));
  ASSERT_TRUE(manager->RegisterElectronIpc(std::move(config)));
  const auto renderer =
      manager->GetElectronIpcRendererConfigForContainer(kContainerId);
  ASSERT_TRUE(renderer);
  EXPECT_EQ(archive_path.AsUTF8Unsafe(), renderer->app_path);
  EXPECT_EQ("manifest-app", renderer->app_name);
  EXPECT_EQ("3.2.1", renderer->app_version);
  EXPECT_EQ(resources.AsUTF8Unsafe(), renderer->resources_directory);
  EXPECT_EQ(release.AsUTF8Unsafe(), renderer->working_directory);
  EXPECT_TRUE(renderer->is_packaged);
  EXPECT_TRUE(renderer->renderer_url_mappings.empty());
  const std::string user_agent = manager->GetDefaultUserAgent(kContainerId);
  EXPECT_TRUE(user_agent.starts_with("manifest-app/3.2.1 "));

  // A valid public key for another archive passes registry validation, but
  // application parsing must fail without replacing the current key/config.
  asar::TestArchiveBuilder wrong_key;
  auto replacement = ipc::mojom::IpcMainConfig::New();
  replacement->container_id = kContainerId;
  replacement->app_path = release.AsUTF8Unsafe();
  replacement->app_name = "Rejected replacement";
  replacement->archive_public_keys.push_back(
      ipc::mojom::IpcArchivePublicKey::New(release.AsUTF8Unsafe(),
                                           wrong_key.public_key_pem()));
  EXPECT_FALSE(manager->RegisterElectronIpc(std::move(replacement)));
  const auto preserved =
      manager->GetElectronIpcRendererConfigForContainer(kContainerId);
  ASSERT_TRUE(preserved);
  EXPECT_EQ(renderer->app_path, preserved->app_path);
  EXPECT_EQ(renderer->app_name, preserved->app_name);
  EXPECT_EQ(renderer->app_version, preserved->app_version);
  EXPECT_EQ(renderer->resources_directory, preserved->resources_directory);
  EXPECT_EQ(renderer->working_directory, preserved->working_directory);
  EXPECT_EQ(renderer->is_packaged, preserved->is_packaged);
  EXPECT_EQ(user_agent, manager->GetDefaultUserAgent(kContainerId));
  EXPECT_TRUE(asar::GetOrCreateAsarArchive(archive_path));
  EXPECT_EQ(0u, manager->service_generation(kContainerId));

  // Replacing the app with an embedded diagnostic uses its mapping default
  // for both processes, and revokes the disk app's public key.
  auto embedded = ipc::mojom::IpcMainConfig::New();
  embedded->container_id = kContainerId;
  embedded->embedded_main_source = "void 0;";
  embedded->renderer_base_url = "chrome://embedded-fixture/";
  ASSERT_TRUE(manager->RegisterElectronIpc(std::move(embedded)));
  const auto diagnostic =
      manager->GetElectronIpcRendererConfigForContainer(kContainerId);
  ASSERT_TRUE(diagnostic);
  EXPECT_TRUE(diagnostic->is_packaged);
  EXPECT_FALSE(asar::GetOrCreateAsarArchive(archive_path));
}

TEST(XenonManagerTest, Ping_WhenDisconnected_ReturnsNotRunning) {
  base::test::TaskEnvironment task_environment;

  XenonManager* manager = XenonManager::GetInstance();
  manager->OnDisconnected();

  std::string reply;
  manager->Ping(base::BindOnce(
      [](std::string* out, const std::string& s) { *out = s; }, &reply));
  task_environment.RunUntilIdle();

  EXPECT_EQ(reply, "Error: Service not running.");
}

TEST(XenonManagerTest, DisconnectedContainerRejectsRendererTraffic) {
  base::test::TaskEnvironment task_environment;
  XenonManager manager;
  constexpr char kContainerId[] = "crashed-container";
  manager.container_service_generations_[kContainerId] = 1;
  auto connection =
      std::make_unique<XenonManager::ContainerServiceConnection>();
  connection->generation = 1;
  auto service_pipe =
      connection->remote.BindNewPipeAndPassReceiver().PassPipe();
  manager.container_services_[kContainerId] = std::move(connection);
  manager.OnContainerServiceDisconnected(kContainerId, 1);

  // Re-registering configuration is not permission for a renderer to restart
  // the app. Failed requests must finish instead of launching another main.
  auto config = ipc::mojom::IpcMainConfig::New();
  config->container_id = kContainerId;
  ASSERT_TRUE(manager.RegisterElectronIpc(std::move(config)));
  manager.ElectronIpcSend(kContainerId, "old-page", "late-send", base::Value());
  for (bool synchronous : {false, true}) {
    bool completed = false;
    auto callback = base::BindOnce(
        [](bool* completed, ipc::mojom::IpcResultPtr result) {
          *completed = true;
          ASSERT_TRUE(result);
          EXPECT_FALSE(result->success);
          EXPECT_EQ(result->error, "Utility ipcMain service is unavailable");
        },
        &completed);
    if (synchronous) {
      manager.ElectronIpcSendSync(kContainerId, "old-page", "late-sync",
                                  base::Value(), std::move(callback));
    } else {
      manager.ElectronIpcInvoke(kContainerId, "old-page", "late-invoke",
                                base::Value(), std::move(callback));
    }
    EXPECT_TRUE(completed);
  }

  mojo::PendingRemote<ipc::mojom::IpcRenderer> renderer;
  auto renderer_receiver = renderer.InitWithNewPipeAndPassReceiver();
  manager.RegisterElectronIpcRenderer(kContainerId, "old-page",
                                      std::move(renderer), 1, 1, 1);
  mojo::Remote<ipc::mojom::NodeAddonHost> addon_host;
  bool addon_disconnected = false;
  mojo::PendingRemote<ipc::mojom::IpcRenderer> callbacks;
  auto callbacks_receiver = callbacks.InitWithNewPipeAndPassReceiver();
  manager.BindNodeAddonHost(kContainerId, "old-page",
                            addon_host.BindNewPipeAndPassReceiver(),
                            std::move(callbacks));
  addon_host.set_disconnect_handler(base::BindOnce(
      [](bool* disconnected) { *disconnected = true; }, &addon_disconnected));
#if BUILDFLAG(ENABLE_XENON_MANAGER_SHARED_REMOTE)
  EXPECT_FALSE(manager.DuplicateServiceRemote(kContainerId).is_bound());
#endif
  task_environment.RunUntilIdle();
  EXPECT_TRUE(addon_disconnected);
  EXPECT_TRUE(service_pipe->QuerySignalsState().peer_closed());
  EXPECT_EQ(manager.service_generation(kContainerId), 1u);
  EXPECT_EQ(manager.service_generation("other-container"), 0u);
  EXPECT_EQ(manager.FindContainerService(kContainerId), nullptr);
}

TEST(XenonManagerTest, StaleDisconnectPreservesRestartedContainer) {
  base::test::TaskEnvironment task_environment;
  XenonManager manager;
  constexpr char kContainerId[] = "restarted-container";
  auto connection =
      std::make_unique<XenonManager::ContainerServiceConnection>();
  connection->generation = 2;
  auto service_pipe =
      connection->remote.BindNewPipeAndPassReceiver().PassPipe();
  auto* current_connection = connection.get();
  manager.container_services_[kContainerId] = std::move(connection);
  manager.container_service_generations_[kContainerId] = 2;

  manager.OnContainerServiceDisconnected(kContainerId, 1);
  task_environment.RunUntilIdle();
  EXPECT_EQ(manager.FindContainerService(kContainerId), current_connection);
  EXPECT_FALSE(service_pipe->QuerySignalsState().peer_closed());

  manager.OnContainerServiceDisconnected(kContainerId, 2);
  task_environment.RunUntilIdle();
  EXPECT_EQ(manager.FindContainerService(kContainerId), nullptr);
  EXPECT_TRUE(service_pipe->QuerySignalsState().peer_closed());
  EXPECT_EQ(manager.service_generation(kContainerId), 2u);
  manager.OnContainerServiceDisconnected(kContainerId, 2);
}

TEST(XenonManagerTest, RendererAddonDisconnectDoesNotRestartOrCloseApp) {
  base::test::TaskEnvironment task_environment;
  XenonManager manager;
  constexpr char kContainerId[] = "addon-owner";
  const XenonManager::RendererAddonKey key{kContainerId, "document-a"};
  auto app = std::make_unique<XenonManager::ContainerServiceConnection>();
  app->generation = 7;
  auto app_pipe = app->remote.BindNewPipeAndPassReceiver().PassPipe();
  manager.container_services_[kContainerId] = std::move(app);
  auto addon = std::make_unique<XenonManager::RendererAddonServiceConnection>();
  addon->container_generation = 7;
  addon->generation = 10;
  auto addon_pipe = addon->remote.BindNewPipeAndPassReceiver().PassPipe();
  auto* existing = addon.get();
  manager.renderer_addon_services_[key] = std::move(addon);

  EXPECT_EQ(manager.EnsureRendererAddonServiceStarted(key), existing);
  manager.OnRendererAddonServiceDisconnected(key, 9);
  task_environment.RunUntilIdle();
  EXPECT_FALSE(addon_pipe->QuerySignalsState().peer_closed());

  manager.OnRendererAddonServiceDisconnected(key, 10);
  task_environment.RunUntilIdle();
  EXPECT_TRUE(addon_pipe->QuerySignalsState().peer_closed());
  EXPECT_FALSE(app_pipe->QuerySignalsState().peer_closed());
  EXPECT_EQ(manager.EnsureRendererAddonServiceStarted(key), nullptr);
  EXPECT_EQ(manager.renderer_addon_services_.at(key)->generation, 10u);

  // Registering the same endpoint cannot erase its failed-process marker.
  mojo::PendingRemote<ipc::mojom::IpcRenderer> renderer;
  auto renderer_receiver = renderer.InitWithNewPipeAndPassReceiver();
  manager.RegisterElectronIpcRenderer(kContainerId, key.second,
                                      std::move(renderer), 1, 1, 1);
  EXPECT_EQ(manager.EnsureRendererAddonServiceStarted(key), nullptr);
  for (const char* channel : {"__xenon:node-addon:invoke-export",
                              "__xenon:node-addon:construct-export",
                              "__xenon:node-addon:invoke-instance"}) {
    bool completed = false;
    manager.ElectronIpcInvoke(
        kContainerId, key.second, channel, base::Value(),
        base::BindOnce(
            [](bool* completed, ipc::mojom::IpcResultPtr result) {
              *completed = true;
              EXPECT_FALSE(result->success);
              EXPECT_EQ(result->error, "Renderer addon service is unavailable");
            },
            &completed));
    EXPECT_TRUE(completed);
  }
}

TEST(XenonManagerTest, NativeInvokesUseDocumentServiceWhileAppIpcStaysInMain) {
  base::test::TaskEnvironment task_environment;
  for (const char* channel :
       {"__xenon:node-addon:invoke-export",
        "__xenon:node-addon:construct-export",
        "__xenon:node-addon:invoke-instance", "application-channel"}) {
    XenonManager manager;
    auto app = std::make_unique<XenonManager::ContainerServiceConnection>();
    app->generation = 1;
    auto app_pipe = app->remote.BindNewPipeAndPassReceiver().PassPipe();
    manager.container_services_["app"] = std::move(app);
    auto addon =
        std::make_unique<XenonManager::RendererAddonServiceConnection>();
    addon->container_generation = 1;
    addon->generation = 1;
    auto addon_pipe = addon->remote.BindNewPipeAndPassReceiver().PassPipe();
    manager.renderer_addon_services_[{"app", "page"}] = std::move(addon);

    manager.ElectronIpcInvoke("app", "page", channel, base::Value(),
                              base::BindOnce([](ipc::mojom::IpcResultPtr) {}));
    task_environment.RunUntilIdle();
    const bool native_call = std::string(channel) != "application-channel";
    EXPECT_EQ(addon_pipe->QuerySignalsState().readable(), native_call);
    EXPECT_EQ(app_pipe->QuerySignalsState().readable(), !native_call);
  }
}

TEST(XenonManagerTest, RendererAddonRemovalAndAppExitCloseOnlyOwnedServices) {
  base::test::TaskEnvironment task_environment;
  XenonManager manager;
  const auto add_app = [&manager](const std::string& container_id) {
    auto app = std::make_unique<XenonManager::ContainerServiceConnection>();
    app->generation = 1;
    auto pipe = app->remote.BindNewPipeAndPassReceiver().PassPipe();
    manager.container_services_[container_id] = std::move(app);
    return pipe;
  };
  const auto add_document = [&manager](const std::string& container_id,
                                       const std::string& endpoint) {
    auto addon =
        std::make_unique<XenonManager::RendererAddonServiceConnection>();
    addon->container_generation = 1;
    addon->generation = ++manager.renderer_addon_service_generation_;
    auto pipe = addon->remote.BindNewPipeAndPassReceiver().PassPipe();
    manager.renderer_addon_services_[{container_id, endpoint}] =
        std::move(addon);
    return pipe;
  };
  auto app_a = add_app("app-a");
  auto app_b = add_app("app-b");
  auto page_a = add_document("app-a", "page-a");
  auto page_b = add_document("app-a", "page-b");
  auto other_page = add_document("app-b", "page-a");
#if BUILDFLAG(ENABLE_XENON_MANAGER_SHARED_REMOTE)
  auto retained =
      manager.renderer_addon_services_.at({"app-a", "page-b"})->remote;
#endif

  manager.RemoveElectronIpcRenderer("app-a", "page-a");
  task_environment.RunUntilIdle();
  EXPECT_TRUE(page_a->QuerySignalsState().peer_closed());
  EXPECT_FALSE(page_b->QuerySignalsState().peer_closed());
  EXPECT_FALSE(other_page->QuerySignalsState().peer_closed());
  EXPECT_EQ(manager.EnsureRendererAddonServiceStarted({"app-a", "page-a"}),
            nullptr);

  manager.CloseContainerService("app-a");
  task_environment.RunUntilIdle();
  EXPECT_TRUE(app_a->QuerySignalsState().peer_closed());
  EXPECT_TRUE(page_b->QuerySignalsState().peer_closed());
  EXPECT_FALSE(app_b->QuerySignalsState().peer_closed());
  EXPECT_FALSE(other_page->QuerySignalsState().peer_closed());
  EXPECT_EQ(manager.renderer_addon_services_.size(), 1u);
#if BUILDFLAG(ENABLE_XENON_MANAGER_SHARED_REMOTE)
  EXPECT_TRUE(retained.is_bound());
#endif
}

TEST(XenonManagerTest,
     ResetClosesRendererAddonServicesWithoutRevivingDocuments) {
  base::test::TaskEnvironment task_environment;
  XenonManager manager;
  auto app = std::make_unique<XenonManager::ContainerServiceConnection>();
  app->generation = 1;
  auto app_pipe = app->remote.BindNewPipeAndPassReceiver().PassPipe();
  manager.container_services_["app"] = std::move(app);
  auto addon = std::make_unique<XenonManager::RendererAddonServiceConnection>();
  addon->container_generation = 1;
  addon->generation = 1;
  auto addon_pipe = addon->remote.BindNewPipeAndPassReceiver().PassPipe();
  manager.renderer_addon_services_[{"app", "document"}] = std::move(addon);

  manager.ResetServiceConnection();
  task_environment.RunUntilIdle();
  EXPECT_TRUE(addon_pipe->QuerySignalsState().peer_closed());
  EXPECT_FALSE(app_pipe->QuerySignalsState().peer_closed());
  EXPECT_TRUE(manager.renderer_addon_services_.empty());
  EXPECT_EQ(manager.EnsureRendererAddonServiceStarted({"app", "document"}),
            nullptr);
}

#if BUILDFLAG(ENABLE_XENON_BROWSER_OBSERVER)
TEST(XenonManagerTest, AppExitDisconnectsAllCopiesAndPreservesConfiguration) {
  base::test::TaskEnvironment task_environment;
  XenonManager manager;
  constexpr char kContainerId[] = "exiting-container";
  auto config = ipc::mojom::IpcMainConfig::New();
  config->container_id = kContainerId;
  ASSERT_TRUE(manager.RegisterElectronIpc(std::move(config)));

  auto connection =
      std::make_unique<XenonManager::ContainerServiceConnection>();
  connection->generation = 1;
  auto service_pipe =
      connection->remote.BindNewPipeAndPassReceiver().PassPipe();
#if BUILDFLAG(ENABLE_XENON_MANAGER_SHARED_REMOTE)
  auto retained_copy = connection->remote;
#endif
  mojo::Remote<mojom::XenonBrowserObserver> observer;
  connection->observer_receiver_id = manager.browser_observer_receivers_.Add(
      &manager, observer.BindNewPipeAndPassReceiver(), kContainerId);
  manager.container_services_[kContainerId] = std::move(connection);
  manager.container_service_generations_[kContainerId] = 1;

  auto core_pipe =
      manager.service_remote_.BindNewPipeAndPassReceiver().PassPipe();
  auto other = std::make_unique<XenonManager::ContainerServiceConnection>();
  auto other_pipe = other->remote.BindNewPipeAndPassReceiver().PassPipe();
  manager.container_services_["other-container"] = std::move(other);

  observer->OnElectronAppExit(kContainerId, 7);
  task_environment.RunUntilIdle();

  EXPECT_EQ(manager.FindContainerService(kContainerId), nullptr);
  EXPECT_TRUE(service_pipe->QuerySignalsState().peer_closed());
  EXPECT_FALSE(observer.is_connected());
  EXPECT_FALSE(core_pipe->QuerySignalsState().peer_closed());
  EXPECT_FALSE(other_pipe->QuerySignalsState().peer_closed());
  EXPECT_TRUE(manager.FindContainerService("other-container"));
  EXPECT_TRUE(manager.GetElectronIpcRendererConfigForContainer(kContainerId));
  EXPECT_EQ(manager.service_generation(kContainerId), 1u);
#if BUILDFLAG(ENABLE_XENON_MANAGER_SHARED_REMOTE)
  // A controller may keep this copy alive. It must no longer keep Utility
  // running, and it must not allow late renderer traffic to restart the app.
  EXPECT_TRUE(retained_copy.is_bound());
  EXPECT_FALSE(manager.DuplicateServiceRemote(kContainerId).is_bound());
#endif
  EXPECT_EQ(manager.EnsureContainerServiceStarted(kContainerId), nullptr);
  bool completed = false;
  manager.ElectronIpcInvoke(
      kContainerId, "old-page", "late-invoke", base::Value(),
      base::BindOnce(
          [](bool* completed, ipc::mojom::IpcResultPtr result) {
            *completed = true;
            EXPECT_FALSE(result->success);
          },
          &completed));
  EXPECT_TRUE(completed);
}

TEST(XenonManagerTest, AppExitRejectsOtherAndStaleObservers) {
  base::test::TaskEnvironment task_environment;
  XenonManager manager;
  constexpr char kContainerId[] = "running-container";
  auto connection =
      std::make_unique<XenonManager::ContainerServiceConnection>();
  connection->generation = 2;
  auto service_pipe =
      connection->remote.BindNewPipeAndPassReceiver().PassPipe();
  mojo::Remote<mojom::XenonBrowserObserver> observer;
  connection->observer_receiver_id = manager.browser_observer_receivers_.Add(
      &manager, observer.BindNewPipeAndPassReceiver(), kContainerId);
  auto* current_connection = connection.get();
  manager.container_services_[kContainerId] = std::move(connection);
  manager.container_service_generations_[kContainerId] = 2;

  mojo::Remote<mojom::XenonBrowserObserver> stale_observer;
  manager.browser_observer_receivers_.Add(
      &manager, stale_observer.BindNewPipeAndPassReceiver(), kContainerId);
  mojo::Remote<mojom::XenonBrowserObserver> other_observer;
  manager.browser_observer_receivers_.Add(
      &manager, other_observer.BindNewPipeAndPassReceiver(), "other-container");
  mojo::Remote<mojom::XenonBrowserObserver> core_observer;
  manager.browser_observer_receivers_.Add(
      &manager, core_observer.BindNewPipeAndPassReceiver(), "core");

  stale_observer->OnElectronAppExit(kContainerId, 0);
  other_observer->OnElectronAppExit(kContainerId, 0);
  core_observer->OnElectronAppExit(kContainerId, 0);
  observer->OnElectronAppExit("other-container", 0);
  task_environment.RunUntilIdle();
  EXPECT_EQ(manager.FindContainerService(kContainerId), current_connection);
  EXPECT_FALSE(service_pipe->QuerySignalsState().peer_closed());

  observer->OnElectronAppExit(kContainerId, 0);
  task_environment.RunUntilIdle();
  EXPECT_EQ(manager.FindContainerService(kContainerId), nullptr);
  EXPECT_TRUE(service_pipe->QuerySignalsState().peer_closed());
}
#endif

#if BUILDFLAG(ENABLE_XENON_ASSOCIATED_SIDE)
TEST(XenonManagerTest, PingAssociated_WhenDisconnected_ReturnsError) {
  base::test::TaskEnvironment task_environment;

  XenonManager* manager = XenonManager::GetInstance();
  manager->OnDisconnected();

  std::string reply;
  manager->PingAssociated(base::BindOnce(
      [](std::string* out, const std::string& s) { *out = s; }, &reply));
  task_environment.RunUntilIdle();

  EXPECT_EQ(reply, "Error: Xenon associated side not bound.");
}
#endif

}  // namespace xenon
