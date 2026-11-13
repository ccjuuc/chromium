// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_session_bridge.h"

#include <memory>

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/run_loop.h"
#include "base/task/single_thread_task_runner.h"
#include "base/test/bind.h"
#include "base/test/mock_log.h"
#include "base/test/test_future.h"
#include "base/threading/sequence_bound.h"
#include "base/threading/thread.h"
#include "components/keyed_service/content/browser_context_dependency_manager.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/site_instance.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/test/browser_task_environment.h"
#include "content/public/test/test_browser_context.h"
#include "content/public/test/test_renderer_host.h"
#include "content/public/test/web_contents_tester.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_permission_broker.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_permissions.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_session_shutdown_notifier_factory.h"
#include "xenon_overlay/common/asar/test_support.h"
#include "xenon_overlay/public/mojom/xenon_permission_check.mojom.h"

namespace xenon::ipc {
namespace {

// TestBrowserContext does not own KeyedServices. Match Profile's real shutdown
// order explicitly, including keeping its user data until after partitions.
class SessionTestBrowserContext : public content::TestBrowserContext {
 public:
  SessionTestBrowserContext() {
    BrowserContextDependencyManager::GetInstance()->MarkBrowserContextLive(
        this);
  }
  ~SessionTestBrowserContext() override { ShutdownServices(); }

  void ShutdownServices() {
    if (services_shutdown_) {
      return;
    }
    services_shutdown_ = true;
    NotifyWillBeDestroyed();
    BrowserContextDependencyManager::GetInstance()
        ->DestroyBrowserContextServices(this);
  }

 private:
  bool services_shutdown_ = false;
};

class PermissionCheckService : public mojom::ElectronPermissionCheck {
 public:
  explicit PermissionCheckService(
      mojo::PendingReceiver<mojom::ElectronPermissionCheck> receiver)
      : receiver_(this, std::move(receiver)) {}
  void Check(base::Value request, CheckCallback callback) override;

 private:
  mojo::Receiver<mojom::ElectronPermissionCheck> receiver_;
};

void PermissionCheckService::Check(base::Value request,
                                   CheckCallback callback) {
  std::move(callback).Run(true, "");
}

class ElectronSessionBridgeTest : public testing::Test {
 public:
  static void SetUpTestSuite() {
    // Match Chrome startup: factories must be registered before any Profile
    // exists and before the first TH/Electron session is requested.
    ElectronSessionShutdownNotifierFactory::GetInstance();
    BrowserContextDependencyManager::GetInstance()
        ->DisallowKeyedServiceFactoryRegistration(
            "EnsureBrowserContextKeyedServiceFactoriesBuilt");
  }

 protected:
  void SetUp() override {
    EXPECT_CALL(log_,
                Log(testing::_, testing::_, testing::_, testing::_, testing::_))
        .Times(testing::AnyNumber())
        .WillRepeatedly(testing::Return(false));
    EXPECT_CALL(
        log_, Log(testing::_, testing::HasSubstr("observer_list.h"), testing::_,
                  testing::_, testing::HasSubstr("observers_.empty()")))
        .Times(0);
    log_.StartCapturingLogs();
  }

  struct Reply {
    base::Value value;
    std::string error;
  };
  Reply Call(const std::string& container,
             const std::string& partition,
             const std::string& method,
             base::DictValue args = {}) {
    return CallOn(&context_, container, partition, method, std::move(args));
  }
  Reply CallOn(content::BrowserContext* context,
               const std::string& container,
               const std::string& partition,
               const std::string& method,
               base::DictValue args = {}) {
    args.Set("partition", partition);
    args.Set("method", method);
    base::test::TestFuture<base::Value, std::string> future;
    CallElectronSession(context, container, base::Value(std::move(args)),
                        future.GetCallback<base::Value, const std::string&>());
    auto [value, error] = future.Take();
    return {std::move(value), std::move(error)};
  }
  base::DictValue Script(const std::string& id,
                         const std::string& type,
                         const base::FilePath& file) {
    return base::DictValue().Set("script",
                                 base::DictValue()
                                     .Set("id", id)
                                     .Set("type", type)
                                     .Set("filePath", file.AsUTF8Unsafe()));
  }
  content::BrowserTaskEnvironment tasks_;
  // Capture through BrowserContext destruction: unchecked observer failures
  // currently log a dump instead of terminating, so an exit code alone misses
  // a teardown regression.
  base::test::MockLog log_;
  SessionTestBrowserContext context_;
};

TEST_F(ElectronSessionBridgeTest, PartitionIdentityAndPersistenceAreReal) {
  const auto persistent =
      GetElectronSessionPartitionConfig(&context_, "app", "persist:browser");
  const auto ephemeral =
      GetElectronSessionPartitionConfig(&context_, "app", "browser");
  EXPECT_FALSE(persistent.is_default());
  EXPECT_FALSE(persistent.in_memory());
  EXPECT_TRUE(ephemeral.in_memory());
  EXPECT_FALSE(
      GetElectronSessionPartitionConfig(&context_, "app", "").in_memory());
  EXPECT_NE(persistent, ephemeral);
  EXPECT_NE(persistent, GetElectronSessionPartitionConfig(&context_, "other",
                                                          "persist:browser"));
  auto* first =
      ResolveElectronSessionPartition(&context_, "app", "persist:browser");
  EXPECT_EQ(first, ResolveElectronSessionPartition(&context_, "app",
                                                   "persist:browser"));
  EXPECT_NE(first, ResolveElectronSessionPartition(&context_, "app", ""));
  EXPECT_NE(first, ResolveElectronSessionPartition(&context_, "other",
                                                   "persist:browser"));
  EXPECT_NE(first, context_.GetDefaultStoragePartition());
  ASSERT_TRUE(FindElectronSessionIdentity(first));
  EXPECT_EQ(FindElectronSessionIdentity(first)->partition, "persist:browser");
  EXPECT_EQ(FindElectronSessionIdentity(first)->container_id, "app");
  EXPECT_FALSE(
      FindElectronSessionIdentity(context_.GetDefaultStoragePartition()));
}

TEST_F(ElectronSessionBridgeTest,
       RetiredSessionRetainsOwnershipButRevokesExecutableState) {
  auto* partition =
      ResolveElectronSessionPartition(&context_, "retired-app", "persist:view");
  ASSERT_TRUE(FindElectronSessionIdentity(partition));
  RemoveElectronSessionsForContainer("retired-app");
  EXPECT_FALSE(FindElectronSessionIdentity(partition));
  const auto provenance = FindElectronSessionProvenance(partition);
  ASSERT_TRUE(provenance);
  EXPECT_EQ(provenance->container_id, "retired-app");
  EXPECT_EQ(provenance->partition, "persist:view");
  EXPECT_TRUE(GetElectronSessionPreloadScripts(partition, "frame").empty());
  EXPECT_FALSE(
      FindElectronSessionProvenance(context_.GetDefaultStoragePartition()));
  EXPECT_EQ(partition, ResolveElectronSessionPartition(&context_, "retired-app",
                                                       "persist:view"));
  EXPECT_TRUE(FindElectronSessionIdentity(partition));
  context_.ShutdownServices();
  EXPECT_FALSE(FindElectronSessionProvenance(partition));
}

TEST_F(ElectronSessionBridgeTest,
       PermissionPipeBindingCannotWaitForItsOwnUiTaskAndGenerationIsRetained) {
  content::RenderViewHostTestEnabler renderer_hosts;
  SessionTestBrowserContext moved_context;
  ResolveElectronSessionPartition(&context_, "permission-app", "browser");
  auto contents = content::WebContentsTester::CreateTestWebContents(
      &context_, content::SiteInstance::CreateForFixedStoragePartition(
                     &context_, GURL("about:blank"),
                     GetElectronSessionPartitionConfig(
                         &context_, "permission-app", "browser")));
  content::WebContentsTester::For(contents.get())
      ->NavigateAndCommit(GURL("https://permission.example/"));
  auto* frame = contents->GetPrimaryMainFrame();
  base::Thread service_thread("PermissionBindingTest");
  ASSERT_TRUE(service_thread.StartWithOptions(
      base::Thread::Options(base::MessagePumpType::IO, 0)));
  base::SequenceBound<PermissionCheckService> service;
  size_t pipe_bindings = 0;
  const auto bind_owner = [&](const std::string& owner) {
    BindElectronPermissionOwner(
        &context_, "permission-app", owner, base::BindLambdaForTesting([&] {
          ++pipe_bindings;
          mojo::PendingRemote<mojom::ElectronPermissionCheck> remote;
          // Reproduce the real service SharedRemote's mandatory UI queue hop.
          base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
              FROM_HERE,
              base::BindOnce(
                  [](base::SequenceBound<PermissionCheckService>* service,
                     scoped_refptr<base::SingleThreadTaskRunner> runner,
                     mojo::PendingReceiver<mojom::ElectronPermissionCheck>
                         receiver) {
                    service->emplace(std::move(runner), std::move(receiver));
                  },
                  &service, service_thread.task_runner(),
                  remote.InitWithNewPipeAndPassReceiver()));
          return remote;
        }),
        base::BindRepeating([](content::WebContents*) { return 1; }),
        base::BindRepeating([](base::Value) {}));
  };
  bind_owner("permission-app:1");
  base::ScopedClosureRunner cleanup(base::BindLambdaForTesting([&] {
    CloseElectronPermissionOwner("permission-app:1");
    CloseElectronPermissionOwner("permission-app:2");
    tasks_.RunUntilIdle();
    service.SynchronouslyResetForTest();
    service_thread.Stop();
  }));
  const auto registered =
      CallElectronPermissions("permission-app", "permission-app:1",
                              base::Value(base::DictValue()
                                              .Set("method", "set-handler")
                                              .Set("partition", "browser")
                                              .Set("kind", "check")
                                              .Set("registration", 1)
                                              .Set("enabled", true)));
  ASSERT_TRUE(registered.has_value()) << registered.error_or("");
  const auto started = base::TimeTicks::Now();
  EXPECT_FALSE(CheckElectronPermission(
      frame, "notifications", GURL("https://permission.example/"), {}));
  EXPECT_LT(base::TimeTicks::Now() - started, base::Seconds(1));
  tasks_.RunUntilIdle();
  EXPECT_TRUE(CheckElectronPermission(frame, "notifications",
                                      GURL("https://permission.example/"), {}));
  const auto before_context_change = GetElectronPermissionFrameValidator(frame);
  ResolveElectronSessionPartition(&moved_context, "permission-app", "browser");
  auto moved_contents = content::WebContentsTester::CreateTestWebContents(
      &moved_context, content::SiteInstance::CreateForFixedStoragePartition(
                          &moved_context, GURL("about:blank"),
                          GetElectronSessionPartitionConfig(
                              &moved_context, "permission-app", "browser")));
  content::WebContentsTester::For(moved_contents.get())
      ->NavigateAndCommit(GURL("https://permission.example/"));
  auto* moved_frame = moved_contents->GetPrimaryMainFrame();
  ASSERT_TRUE(
      RebindElectronPermissionOwnerContext("permission-app:1", &moved_context)
          .has_value());
  EXPECT_FALSE(before_context_change.Run());
  EXPECT_FALSE(CheckElectronPermission(
      frame, "notifications", GURL("https://permission.example/"), {}));
  EXPECT_TRUE(CheckElectronPermission(moved_frame, "notifications",
                                      GURL("https://permission.example/"), {}));
  EXPECT_EQ(pipe_bindings, 1u);
  ASSERT_TRUE(
      RebindElectronPermissionOwnerContext("permission-app:1", &context_)
          .has_value());
  EXPECT_FALSE(before_context_change.Run());
  moved_contents.reset();
  moved_context.ShutdownServices();
  EXPECT_TRUE(CheckElectronPermission(frame, "notifications",
                                      GURL("https://permission.example/"), {}));
  const auto original_generation = GetElectronPermissionFrameValidator(frame);
  EXPECT_TRUE(original_generation.Run());
  CloseElectronPermissionOwner("permission-app:1");
  RemoveElectronSessionsForContainer("permission-app");
  EXPECT_TRUE(IsElectronPermissionFrame(frame));
  EXPECT_FALSE(IsActiveElectronPermissionFrame(frame));
  EXPECT_FALSE(CheckElectronPermission(
      frame, "notifications", GURL("https://permission.example/"), {}));
  bind_owner("permission-app:2");
  EXPECT_FALSE(original_generation.Run());
  EXPECT_TRUE(IsActiveElectronPermissionFrame(frame));
  cleanup.RunAndReset();
}

TEST_F(ElectronSessionBridgeTest, ReadsRealDiskAndAsarPreloadsAndRevokesOwner) {
  base::ScopedTempDir directory;
  ASSERT_TRUE(directory.CreateUniqueTempDir());
  const auto disk = directory.GetPath().AppendASCII("frame.js");
  const auto archive = directory.GetPath().AppendASCII("preload.asar");
  ASSERT_TRUE(base::WriteFile(disk, "globalThis.diskPreload = true;"));
  asar::TestArchiveBuilder builder;
  ASSERT_TRUE(builder.Write(
      archive, {{"worker.js", "globalThis.workerPreload = true;"}}, false));
  const auto worker_file = archive.AppendASCII("worker.js");
  EXPECT_TRUE(Call("app", "persist:browser", "register-preload",
                   Script("frame", "frame", disk))
                  .error.empty());
  EXPECT_TRUE(Call("app", "persist:browser", "register-preload",
                   Script("worker", "service-worker", worker_file))
                  .error.empty());
  auto* partition =
      ResolveElectronSessionPartition(&context_, "app", "persist:browser");
  auto frames = GetElectronSessionPreloadScripts(partition, "frame");
  auto workers = GetElectronSessionPreloadScripts(partition, "service-worker");
  ASSERT_EQ(frames.size(), 1u);
  ASSERT_EQ(workers.size(), 1u);
  EXPECT_EQ(frames[0].source, "globalThis.diskPreload = true;");
  EXPECT_EQ(workers[0].source, "globalThis.workerPreload = true;");
  EXPECT_TRUE(
      Call("other", "persist:browser", "get-preloads").value.GetList().empty());
  EXPECT_FALSE(Call("app", "persist:browser", "register-preload",
                    Script("frame", "frame", disk))
                   .error.empty());
  EXPECT_FALSE(Call("app", "persist:browser", "register-preload",
                    Script("missing", "frame",
                           directory.GetPath().AppendASCII("missing.js")))
                   .error.empty());
  EXPECT_EQ(
      Call("app", "persist:browser", "get-preloads").value.GetList().size(),
      2u);
  EXPECT_TRUE(Call("app", "persist:browser", "unregister-preload",
                   base::DictValue().Set("id", "frame"))
                  .error.empty());
  EXPECT_TRUE(GetElectronSessionPreloadScripts(partition, "frame").empty());
  EXPECT_EQ(
      GetElectronSessionPreloadScripts(partition, "service-worker").size(), 1u);
  RemoveElectronSessionsForContainer("app");
  EXPECT_FALSE(FindElectronSessionIdentity(partition));
  EXPECT_TRUE(
      GetElectronSessionPreloadScripts(partition, "service-worker").empty());
  EXPECT_TRUE(
      Call("app", "persist:browser", "get-preloads").value.GetList().empty());
}

TEST_F(ElectronSessionBridgeTest,
       OwnerDisconnectDuringReadCannotCommitRegistration) {
  base::ScopedTempDir directory;
  ASSERT_TRUE(directory.CreateUniqueTempDir());
  const auto disk = directory.GetPath().AppendASCII("frame.js");
  ASSERT_TRUE(base::WriteFile(disk, "void 0;"));
  auto args = Script("frame", "frame", disk);
  args.Set("partition", "");
  args.Set("method", "register-preload");
  base::test::TestFuture<base::Value, std::string> future;
  CallElectronSession(&context_, "app", base::Value(std::move(args)),
                      future.GetCallback<base::Value, const std::string&>());
  RemoveElectronSessionsForContainer("app");
  EXPECT_EQ(future.Get<1>(), "Session owner disconnected");
  EXPECT_TRUE(Call("app", "", "get-preloads").value.GetList().empty());
}

TEST_F(ElectronSessionBridgeTest,
       OwnerRevocationCoversEveryLiveContextAndPreservesOtherContainers) {
  base::ScopedTempDir directory;
  ASSERT_TRUE(directory.CreateUniqueTempDir());
  const auto preload = directory.GetPath().AppendASCII("preload.js");
  ASSERT_TRUE(base::WriteFile(preload, "void 0;"));
  SessionTestBrowserContext second_context;
  for (auto* context : {&context_, &second_context}) {
    for (const char* container : {"app", "other"}) {
      ASSERT_TRUE(CallOn(context, container, "browser", "register-preload",
                         Script("frame", "frame", preload))
                      .error.empty());
    }
  }

  // A destroyed BrowserContext must leave no dangling entry in the index used
  // to revoke an owner. The remaining contexts continue to own their states.
  {
    SessionTestBrowserContext transient_context;
    ASSERT_TRUE(CallOn(&transient_context, "app", "browser", "register-preload",
                       Script("frame", "frame", preload))
                    .error.empty());
  }

  auto* first = ResolveElectronSessionPartition(&context_, "app", "browser");
  auto* second =
      ResolveElectronSessionPartition(&second_context, "app", "browser");
  RemoveElectronSessionsForContainer("app");
  EXPECT_FALSE(FindElectronSessionIdentity(first));
  EXPECT_FALSE(FindElectronSessionIdentity(second));
  EXPECT_TRUE(GetElectronSessionPreloadScripts(first, "frame").empty());
  EXPECT_TRUE(GetElectronSessionPreloadScripts(second, "frame").empty());
  for (auto* context : {&context_, &second_context}) {
    EXPECT_TRUE(CallOn(context, "app", "browser", "get-preloads")
                    .value.GetList()
                    .empty());
    EXPECT_EQ(CallOn(context, "other", "browser", "get-preloads")
                  .value.GetList()
                  .size(),
              1u);
  }
}

TEST_F(ElectronSessionBridgeTest,
       BlankWindowRetainsSessionPartitionAndPreloadsAcrossNavigation) {
  base::ScopedTempDir directory;
  ASSERT_TRUE(directory.CreateUniqueTempDir());
  const auto preload = directory.GetPath().AppendASCII("preload.js");
  ASSERT_TRUE(base::WriteFile(preload, "globalThis.sessionPreload = true;"));
  ASSERT_TRUE(Call("app", "browser", "register-preload",
                   Script("frame", "frame", preload))
                  .error.empty());
  auto* expected = ResolveElectronSessionPartition(&context_, "app", "browser");

  content::RenderViewHostTestEnabler renderer_hosts;
  auto contents = content::WebContentsTester::CreateTestWebContents(
      &context_,
      CreateElectronSessionSiteInstance(&context_, "app", "browser",
                                          GURL("about:blank")));
  EXPECT_EQ(contents->GetPrimaryMainFrame()->GetStoragePartition(), expected);
  for (const auto& url : {GURL("about:blank"), GURL("https://one.test/"),
                          GURL("https://two.test/")}) {
    content::WebContentsTester::For(contents.get())->NavigateAndCommit(url);
    auto* partition = contents->GetPrimaryMainFrame()->GetStoragePartition();
    EXPECT_EQ(partition, expected);
    EXPECT_NE(partition, context_.GetDefaultStoragePartition());
    ASSERT_TRUE(FindElectronSessionIdentity(partition));
    EXPECT_EQ(FindElectronSessionIdentity(partition)->partition, "browser");
    const auto scripts = GetElectronSessionPreloadScripts(partition, "frame");
    ASSERT_EQ(scripts.size(), 1u);
    EXPECT_EQ(scripts.front().source, "globalThis.sessionPreload = true;");
  }
}

TEST_F(ElectronSessionBridgeTest,
       ProfileShutdownDetachesSessionsBeforeStoragePartitionsAreDestroyed) {
  auto closing_context = std::make_unique<SessionTestBrowserContext>();
  SessionTestBrowserContext other_context;
  auto* closing =
      ResolveElectronSessionPartition(closing_context.get(), "app", "browser");
  auto* surviving =
      ResolveElectronSessionPartition(&other_context, "app", "browser");
  ASSERT_TRUE(FindElectronSessionIdentity(closing));
  ASSERT_TRUE(FindElectronSessionIdentity(surviving));

  closing_context->ShutdownServices();
  // The real partition is still alive here. The registry and its observer must
  // already be gone, without relying on a later ServiceWorkerContext
  // destructor.
  EXPECT_EQ(closing, closing_context->GetStoragePartition(
                         GetElectronSessionPartitionConfig(
                             closing_context.get(), "app", "browser"),
                         false));
  EXPECT_FALSE(FindElectronSessionIdentity(closing));
  EXPECT_TRUE(FindElectronSessionIdentity(surviving));
  EXPECT_EQ(nullptr, ResolveElectronSessionPartition(closing_context.get(),
                                                     "app", "browser"));
  EXPECT_EQ(
      "Electron session context is shutting down",
      CallOn(closing_context.get(), "app", "browser", "get-running-workers")
          .error);
  // Match ProfileImpl: storage partitions are destroyed before user data. Any
  // retained raw_ptr or worker observer in the registry would fail this order.
  closing_context.reset();
  RemoveElectronSessionsForContainer("app");
  EXPECT_FALSE(FindElectronSessionIdentity(surviving));
}

TEST_F(ElectronSessionBridgeTest,
       ProfileShutdownCancelsInFlightPreloadBeforePartitionTeardown) {
  auto closing_context = std::make_unique<SessionTestBrowserContext>();
  base::ScopedTempDir directory;
  ASSERT_TRUE(directory.CreateUniqueTempDir());
  const auto preload = directory.GetPath().AppendASCII("preload.js");
  ASSERT_TRUE(base::WriteFile(preload, "void 0;"));
  auto args = Script("frame", "frame", preload);
  args.Set("partition", "browser");
  args.Set("method", "register-preload");
  base::test::TestFuture<base::Value, std::string> future;
  CallElectronSession(closing_context.get(), "app",
                      base::Value(std::move(args)),
                      future.GetCallback<base::Value, const std::string&>());
  closing_context->ShutdownServices();
  EXPECT_EQ(
      "Electron session context is shutting down",
      CallOn(closing_context.get(), "app", "browser", "get-preloads").error);
  closing_context.reset();
  EXPECT_EQ("Session owner disconnected", future.Get<1>());
}

}  // namespace
}  // namespace xenon::ipc
