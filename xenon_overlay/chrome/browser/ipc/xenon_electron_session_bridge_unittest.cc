// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_session_bridge.h"

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/run_loop.h"
#include "base/test/mock_log.h"
#include "base/test/test_future.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/site_instance.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/test/browser_task_environment.h"
#include "content/public/test/test_browser_context.h"
#include "content/public/test/test_renderer_host.h"
#include "content/public/test/web_contents_tester.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "xenon_overlay/common/asar/test_support.h"

namespace xenon::ipc {
namespace {

class ElectronSessionBridgeTest : public testing::Test {
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
  content::TestBrowserContext context_;
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
  content::TestBrowserContext second_context;
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
    content::TestBrowserContext transient_context;
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
      content::SiteInstance::CreateForFixedStoragePartition(
          &context_, GURL("about:blank"),
          GetElectronSessionPartitionConfig(&context_, "app", "browser")));
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

}  // namespace
}  // namespace xenon::ipc
