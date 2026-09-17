// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/xenon_manager.h"

#include <memory>
#include <utility>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/test/task_environment.h"
#include "base/threading/thread_restrictions.h"
#include "build/buildflag.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "xenon_overlay/buildflags/buildflags.h"
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
  manager.BindNodeAddonHost(kContainerId, "old-page",
                            addon_host.BindNewPipeAndPassReceiver());
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
