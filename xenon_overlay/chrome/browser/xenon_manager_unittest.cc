// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/xenon_manager.h"

#include "base/functional/bind.h"
#include "base/test/task_environment.h"
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
  manager.OnContainerServiceDisconnected(kContainerId);

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
  EXPECT_EQ(manager.service_generation(kContainerId), 1u);
  EXPECT_EQ(manager.service_generation("other-container"), 0u);
  EXPECT_EQ(manager.FindContainerService(kContainerId), nullptr);
}

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
