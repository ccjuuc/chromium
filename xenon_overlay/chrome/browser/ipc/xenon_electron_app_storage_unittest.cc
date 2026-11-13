// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_electron_app_storage.h"

#include <memory>
#include <optional>
#include <utility>

#include "base/command_line.h"
#include "base/files/file.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/test/bind.h"
#include "base/test/multiprocess_test.h"
#include "base/test/test_future.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "components/keyed_service/content/browser_context_dependency_manager.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/site_instance.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/storage_partition_path.h"
#include "content/public/browser/storage_service_context.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_task_environment.h"
#include "content/public/test/test_browser_context.h"
#include "content/public/test/test_renderer_host.h"
#include "content/public/test/web_contents_tester.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "testing/multiprocess_func_list.h"
#include "url/gurl.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_app_config.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_session_bridge.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_session_shutdown_notifier_factory.h"
#include "xenon_overlay/public/mojom/xenon_ipc.mojom.h"

#if BUILDFLAG(IS_WIN)
#include "base/test/file_path_reparse_point_win.h"
#endif

namespace xenon::ipc {
namespace {

class StorageTestContext : public content::TestBrowserContext {
 public:
  explicit StorageTestContext(const base::FilePath& path)
      : TestBrowserContext(path), path_(TakePath()) {
    BrowserContextDependencyManager::GetInstance()->MarkBrowserContextLive(
        this);
  }
  ~StorageTestContext() override {
    NotifyWillBeDestroyed();
    BrowserContextDependencyManager::GetInstance()
        ->DestroyBrowserContextServices(this);
  }
  base::FilePath GetPath() const override { return path_; }

 private:
  const base::FilePath path_;
};

using Ready = base::test::TestFuture<content::BrowserContext*, std::string>;
using PathReply = base::test::TestFuture<base::Value, std::string>;

class ElectronAppStorageTest : public testing::Test {
 public:
  static void SetUpTestSuite() {
    ElectronSessionShutdownNotifierFactory::GetInstance();
  }

 protected:
  void SetUp() override {
    ASSERT_TRUE(root_.CreateUniqueTempDir());
    ASSERT_TRUE(base::NormalizeFilePath(root_.GetPath(), &canonical_root_));
    host_ = std::make_unique<StorageTestContext>(Path("host"));
    SetElectronAppStorageContextFactoryForTesting(
        base::BindLambdaForTesting(
            [this](const base::FilePath& path,
                   ElectronAppStorageContextCreated callback) {
              ++creations_;
              if (hold_creation_) {
                held_path_ = path;
                held_creation_ = std::move(callback);
              } else if (fail_creation_) {
                std::move(callback).Run(
                    nullptr, "ERR_FAILED: test profile creation failed");
              } else {
                std::move(callback).Run(
                    std::make_unique<StorageTestContext>(path), "");
              }
            }),
        base::BindLambdaForTesting(
            [this](std::unique_ptr<content::BrowserContext> context) {
              ++destructions_;
              context.reset();
            }));
    ASSERT_TRUE(Register("one"));
  }

  void TearDown() override {
    RetireElectronAppStorage("one");
    RetireElectronAppStorage("two");
    if (held_creation_) {
      std::move(held_creation_).Run(nullptr, "ERR_ABORTED: test ended");
    }
    environment_.RunUntilIdle();
    host_.reset();
    environment_.RunUntilIdle();
    SetElectronAppStorageContextFactoryForTesting({}, {});
  }

  bool Register(const std::string& owner) {
    return RegisterElectronAppStorage(host_.get(), owner + "-container", owner,
                                      Path("legacy-user-data"))
        .has_value();
  }
  base::FilePath Path(const char* name) const {
    return canonical_root_.AppendASCII(name);
  }
  base::FilePath Get(const std::string& name,
                     const std::string& owner = "one") {
    PathReply reply;
    CallElectronAppStoragePath(
        owner,
        base::Value(base::DictValue().Set("method", "get").Set("name", name)),
        reply.GetCallback());
    EXPECT_TRUE(reply.Get<1>().empty());
    return reply.Get<0>().is_string()
               ? base::FilePath::FromUTF8Unsafe(reply.Get<0>().GetString())
               : base::FilePath();
  }
  std::string Set(const std::string& name,
                  const base::FilePath& path,
                  const std::string& owner = "one") {
    PathReply reply;
    CallElectronAppStoragePath(
        owner,
        base::Value(base::DictValue()
                        .Set("method", "set")
                        .Set("name", name)
                        .Set("path", path.AsUTF8Unsafe())),
        reply.GetCallback());
    return reply.Get<1>();
  }
  int PartitionCount() {
    return static_cast<int>(host_->GetLoadedStoragePartitionCount());
  }

  void UseAliasedHost() {
    RetireElectronAppStorage("one");
    environment_.RunUntilIdle();
    host_.reset();
    ASSERT_TRUE(base::CreateDirectory(Path("real-host")));
#if BUILDFLAG(IS_WIN)
    ASSERT_TRUE(base::CreateDirectory(Path("host-alias")));
    host_alias_ = base::test::FilePathReparsePoint::Create(Path("host-alias"),
                                                           Path("real-host"));
    ASSERT_TRUE(host_alias_.has_value());
#elif BUILDFLAG(IS_POSIX)
    ASSERT_TRUE(
        base::CreateSymbolicLink(Path("real-host"), Path("host-alias")));
#endif
    host_ = std::make_unique<StorageTestContext>(Path("host-alias"));
  }

  content::BrowserTaskEnvironment environment_;
  base::ScopedTempDir root_;
  base::FilePath canonical_root_;
  std::unique_ptr<StorageTestContext> host_;
#if BUILDFLAG(IS_WIN)
  std::optional<base::test::FilePathReparsePoint> host_alias_;
#endif
  bool hold_creation_ = false;
  bool fail_creation_ = false;
  int creations_ = 0;
  int destructions_ = 0;
  base::FilePath held_path_;
  ElectronAppStorageContextCreated held_creation_;
};

TEST_F(ElectronAppStorageTest, ReadingPathsDoesNotCreateOrFreezeStorage) {
  const int before = PartitionCount();
  PathReply synchronous;
  CallElectronAppStoragePath(
      "one",
      base::Value(
          base::DictValue().Set("method", "get").Set("name", "userData")),
      synchronous.GetCallback());
  EXPECT_TRUE(synchronous.IsReady());
  EXPECT_EQ(Get("userData"), Path("legacy-user-data"));
  const auto config =
      GetElectronSessionPartitionConfig(host_.get(), "one-container", "");
  EXPECT_EQ(Get("sessionData"),
            content::GetStoragePartitionPath(host_.get(), config));
  EXPECT_EQ(PeekElectronAppStorageContext("one"), host_.get());
  EXPECT_EQ(PartitionCount(), before);
  EXPECT_TRUE(Set("userData", Path("custom")).empty());
  EXPECT_FALSE(base::PathExists(Path("custom")));
  EXPECT_EQ(creations_, 0);
}

TEST_F(ElectronAppStorageTest,
       DefaultUserDataIdentityIsStableAndNamesDoNotCollide) {
  auto config = mojom::IpcMainConfig::New();
  config->host_user_data_directory = Path("legacy-user-data").AsUTF8Unsafe();
  config->container_id = "one-container";
  config->app_path = Path("release-v1").AsUTF8Unsafe();
  config->app_name = "Same display name";
  auto first = ResolveElectronAppStorageUserDataPath(*config);
  ASSERT_TRUE(first.has_value());
  EXPECT_TRUE(
      Path("legacy-user-data").AppendASCII("ElectronApps").IsParent(*first));
  EXPECT_FALSE(base::PathExists(*first));

  config->app_path = Path("release-v2").AsUTF8Unsafe();
  config->app_version = "2.0";
  EXPECT_EQ(ResolveElectronAppStorageUserDataPath(*config), first);
  config->container_id = "two-container";
  EXPECT_NE(ResolveElectronAppStorageUserDataPath(*config), first);

  config->storage_id = "org.example.stable";
  const auto explicit_identity = ResolveElectronAppStorageUserDataPath(*config);
  config->container_id = "default";
  config->app_path = Path("moved-release").AsUTF8Unsafe();
  EXPECT_EQ(ResolveElectronAppStorageUserDataPath(*config), explicit_identity);
  config->storage_id.clear();
  const auto installation = ResolveElectronAppStorageUserDataPath(*config);
  config->app_path = Path("another-app").AsUTF8Unsafe();
  EXPECT_NE(ResolveElectronAppStorageUserDataPath(*config), installation);

  config->user_data_policy = mojom::UserDataPolicy::kLegacyHost;
  EXPECT_EQ(ResolveElectronAppStorageUserDataPath(*config),
            Path("legacy-user-data"));
  config->host_user_data_directory = "relative";
  EXPECT_FALSE(ResolveElectronAppStorageUserDataPath(*config).has_value());
}

TEST_F(ElectronAppStorageTest,
       PreparingIsolatedUserDataPreservesLegacyFilesAndSessionPartitions) {
  const auto original_session = Get("sessionData");
  ASSERT_TRUE(base::CreateDirectory(Path("legacy-user-data")));
  const auto old_file = Path("legacy-user-data").AppendASCII("config.json");
  ASSERT_TRUE(base::WriteFile(old_file, "legacy settings"));
  RetireElectronAppStorage("one");
  environment_.RunUntilIdle();
  auto config = mojom::IpcMainConfig::New();
  config->host_user_data_directory = Path("legacy-user-data").AsUTF8Unsafe();
  config->container_id = "one-container";
  auto first = ResolveElectronAppStorageUserDataPath(*config);
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(RegisterElectronAppStorage(host_.get(), config->container_id,
                                         "one", *first)
                  .has_value());
  config->container_id = "two-container";
  auto second = ResolveElectronAppStorageUserDataPath(*config);
  ASSERT_TRUE(second.has_value());
  ASSERT_TRUE(RegisterElectronAppStorage(host_.get(), config->container_id,
                                         "two", *second)
                  .has_value());

  base::test::TestFuture<std::string> first_ready;
  base::test::TestFuture<std::string> second_ready;
  const auto partitions_before = PartitionCount();
  PrepareElectronAppStorageUserData("one", first_ready.GetCallback());
  PrepareElectronAppStorageUserData("two", second_ready.GetCallback());
  EXPECT_FALSE(first_ready.IsReady());
  EXPECT_TRUE(first_ready.Get().empty());
  EXPECT_TRUE(second_ready.Get().empty());
  EXPECT_TRUE(base::DirectoryExists(*first));
  EXPECT_TRUE(base::DirectoryExists(*second));
  EXPECT_EQ(Get("userData"), *first);
  EXPECT_EQ(Get("sessionData"), original_session);
  EXPECT_EQ(PartitionCount(), partitions_before);
  EXPECT_EQ(creations_, 0);
  ASSERT_TRUE(base::WriteFile(first->AppendASCII("config.json"), "first"));
  ASSERT_TRUE(base::WriteFile(second->AppendASCII("config.json"), "second"));
  std::string bytes;
  ASSERT_TRUE(base::ReadFileToString(old_file, &bytes));
  EXPECT_EQ(bytes, "legacy settings");
  ASSERT_TRUE(
      base::ReadFileToString(first->AppendASCII("config.json"), &bytes));
  EXPECT_EQ(bytes, "first");
  ASSERT_TRUE(
      base::ReadFileToString(second->AppendASCII("config.json"), &bytes));
  EXPECT_EQ(bytes, "second");

  // Startup preparation does not freeze storage or weaken explicit setPath.
  EXPECT_TRUE(Set("userData", Path("explicit-app-path")).empty());
  EXPECT_EQ(Get("sessionData"), Path("explicit-app-path"));
}

TEST_F(ElectronAppStorageTest, UserDataPreparationReportsFailureAndRetirement) {
  ASSERT_TRUE(base::WriteFile(Path("legacy-user-data"), "not a directory"));
  base::test::TestFuture<std::string> failed;
  PrepareElectronAppStorageUserData("one", failed.GetCallback());
  EXPECT_TRUE(failed.Get().starts_with("ERR_FS_OPERATION_FAILED:"));
  EXPECT_EQ(Get("userData"), Path("legacy-user-data"));
  EXPECT_EQ(creations_, 0);

  base::test::TestFuture<std::string> retired;
  PrepareElectronAppStorageUserData("one", retired.GetCallback());
  RetireElectronAppStorage("one");
  EXPECT_TRUE(retired.Get().starts_with("ERR_ABORTED:"));
}

TEST_F(ElectronAppStorageTest,
       ReadOnlyChromiumPathMatchesActualPersistentPartition) {
  const auto default_config =
      content::StoragePartitionConfig::CreateDefault(host_.get());
  const auto named = GetElectronSessionPartitionConfig(
      host_.get(), "one-container", "persist:files");
  const auto memory =
      GetElectronSessionPartitionConfig(host_.get(), "one-container", "memory");
  const int before = PartitionCount();
  const auto default_path =
      content::GetStoragePartitionPath(host_.get(), default_config);
  const auto named_path = content::GetStoragePartitionPath(host_.get(), named);
  EXPECT_EQ(default_path, host_->GetPath());
  EXPECT_TRUE(content::GetStoragePartitionPath(host_.get(), memory).empty());
  EXPECT_EQ(PartitionCount(), before);
  EXPECT_EQ(host_->GetStoragePartition(default_config)->GetPath(),
            default_path);
  EXPECT_EQ(host_->GetStoragePartition(named)->GetPath(), named_path);
}

TEST_F(ElectronAppStorageTest,
       LegacyOwnerKeepsExistingContextAndFreezesAtFirstUse) {
  Ready reply;
  EnsureElectronAppStorageContext("one", reply.GetCallback());
  EXPECT_FALSE(reply.IsReady());
  EXPECT_EQ(reply.Get<0>(), host_.get());
  EXPECT_TRUE(reply.Get<1>().empty());
  EXPECT_EQ(creations_, 0);
  EXPECT_TRUE(Set("userData", Path("legacy-user-data")).empty());
  EXPECT_TRUE(Set("sessionData", Get("sessionData")).empty());
  EXPECT_TRUE(Set("userData", Path("independent-user-data")).empty());
  EXPECT_TRUE(
      Set("sessionData", Path("late")).starts_with("ERR_INVALID_STATE:"));
}

TEST_F(ElectronAppStorageTest,
       CustomDefaultSessionUsesActualRootAndNamedSessionsRemainIsolated) {
  ASSERT_TRUE(Set("userData", Path("custom")).empty());
  Ready reply;
  EnsureElectronAppStorageContext("one", reply.GetCallback());
  auto* context = reply.Get<0>();
  ASSERT_NE(context, nullptr);
  ASSERT_TRUE(reply.Get<1>().empty());
  EXPECT_NE(context, host_.get());
  EXPECT_EQ(context->GetPath(), Get("sessionData"));
  EXPECT_EQ(GetElectronAppStorageUserDataPath("one"), Get("userData"));
  auto config = GetElectronSessionPartitionConfig(context, "one-container", "");
  EXPECT_TRUE(config.is_default());
  EXPECT_EQ(
      ResolveElectronSessionPartition(context, "one-container", "")->GetPath(),
      Path("custom"));
  const auto named = GetElectronSessionPartitionConfig(context, "one-container",
                                                       "persist:named");
  const auto memory =
      GetElectronSessionPartitionConfig(context, "one-container", "memory");
  EXPECT_FALSE(named.is_default());
  EXPECT_FALSE(named.in_memory());
  EXPECT_TRUE(memory.in_memory());
  EXPECT_FALSE(GetElectronAppStorageSessionConfig(context, "two-container", "")
                   .has_value());
  EXPECT_TRUE(Set("userData", Path("late")).starts_with("ERR_INVALID_STATE:"));
  EXPECT_EQ(creations_, 1);
}

TEST_F(ElectronAppStorageTest,
       NamedSessionsDoNotDuplicateApplicationNamespace) {
  const auto legacy = GetElectronSessionPartitionConfig(
      host_.get(), "one-container", "persist:named");
  const auto legacy_path =
      content::GetStoragePartitionPath(host_.get(), legacy);
  ASSERT_TRUE(Set("userData", Path("independent-one")).empty());
  Ready first;
  EnsureElectronAppStorageContext("one", first.GetCallback());
  auto* first_context = first.Get<0>();
  ASSERT_NE(first_context, nullptr);
  const auto named = GetElectronSessionPartitionConfig(
      first_context, "one-container", "persist:named");
  const auto named_path =
      content::GetStoragePartitionPath(first_context, named);
  base::FilePath relative_path;
  ASSERT_TRUE(
      first_context->GetPath().AppendRelativePath(named_path, &relative_path));
  // Reserve space for the origin directory, a 52-character SQLite database
  // name and its WAL suffix even when the selected profile root is not short.
  EXPECT_LT(relative_path.value().size(), 40u);
  EXPECT_EQ(named.partition_name(), legacy.partition_name());
  EXPECT_FALSE(named.in_memory());
  EXPECT_NE(named, GetElectronSessionPartitionConfig(
                       first_context, "one-container", "persist:other"));
  const auto memory = GetElectronSessionPartitionConfig(
      first_context, "one-container", "named");
  EXPECT_TRUE(memory.in_memory());
  EXPECT_TRUE(content::GetStoragePartitionPath(first_context, memory).empty());

  ASSERT_TRUE(Register("two"));
  ASSERT_TRUE(Set("userData", Path("independent-two"), "two").empty());
  Ready second;
  EnsureElectronAppStorageContext("two", second.GetCallback());
  auto* second_context = second.Get<0>();
  ASSERT_NE(second_context, nullptr);
  const auto second_named = GetElectronSessionPartitionConfig(
      second_context, "two-container", "persist:named");
  EXPECT_EQ(named, second_named);
  EXPECT_NE(named_path,
            content::GetStoragePartitionPath(second_context, second_named));

  RetireElectronAppStorage("one");
  environment_.RunUntilIdle();
  ASSERT_TRUE(Register("one"));
  ASSERT_TRUE(Set("userData", Path("independent-one")).empty());
  Ready reopened;
  EnsureElectronAppStorageContext("one", reopened.GetCallback());
  ASSERT_NE(reopened.Get<0>(), nullptr);
  const auto reopened_named = GetElectronSessionPartitionConfig(
      reopened.Get<0>(), "one-container", "persist:named");
  EXPECT_EQ(named_path, content::GetStoragePartitionPath(reopened.Get<0>(),
                                                         reopened_named));
  // Redirected sessions must not change the path mapping for old host data.
  EXPECT_EQ(legacy, GetElectronSessionPartitionConfig(
                        host_.get(), "one-container", "persist:named"));
  EXPECT_EQ(legacy_path, content::GetStoragePartitionPath(host_.get(), legacy));
}

TEST_F(ElectronAppStorageTest,
       DefaultSiteInstanceUsesRedirectedProfileDefaultPartition) {
  ASSERT_TRUE(Set("userData", Path("default-window")).empty());
  Ready ready;
  EnsureElectronAppStorageContext("one", ready.GetCallback());
  auto* context = ready.Get<0>();
  ASSERT_NE(context, nullptr);
  auto* expected =
      ResolveElectronSessionPartition(context, "one-container", "");
  ASSERT_EQ(expected, context->GetDefaultStoragePartition());
  content::RenderViewHostTestEnabler renderer_hosts;
  auto contents = content::WebContentsTester::CreateTestWebContents(
      context, CreateElectronSessionSiteInstance(context, "one-container", "",
                                                 GURL("about:blank")));
  EXPECT_EQ(contents->GetPrimaryMainFrame()->GetStoragePartition(), expected);
  for (const auto& url :
       {GURL("https://one.test/"), GURL("https://two.test/")}) {
    content::WebContentsTester::For(contents.get())->NavigateAndCommit(url);
    EXPECT_EQ(contents->GetPrimaryMainFrame()->GetStoragePartition(), expected);
  }
}

TEST_F(ElectronAppStorageTest,
       NamedSiteInstancesKeepFixedPartitionsAcrossNavigation) {
  ASSERT_TRUE(Set("userData", Path("named-window")).empty());
  Ready ready;
  EnsureElectronAppStorageContext("one", ready.GetCallback());
  auto* context = ready.Get<0>();
  ASSERT_NE(context, nullptr);
  content::RenderViewHostTestEnabler renderer_hosts;
  for (const char* partition : {"persist:named", "memory"}) {
    auto* expected =
        ResolveElectronSessionPartition(context, "one-container", partition);
    ASSERT_NE(expected, context->GetDefaultStoragePartition());
    auto contents = content::WebContentsTester::CreateTestWebContents(
        context, CreateElectronSessionSiteInstance(
                     context, "one-container", partition, GURL("about:blank")));
    EXPECT_EQ(contents->GetPrimaryMainFrame()->GetStoragePartition(), expected);
    for (const auto& url :
         {GURL("https://one.test/"), GURL("https://two.test/")}) {
      content::WebContentsTester::For(contents.get())->NavigateAndCommit(url);
      EXPECT_EQ(contents->GetPrimaryMainFrame()->GetStoragePartition(),
                expected);
    }
  }
}

TEST_F(ElectronAppStorageTest,
       ExplicitSessionPathAllowsIndependentUserDataChanges) {
  ASSERT_TRUE(Set("sessionData", Path("sessions")).empty());
  ASSERT_TRUE(Set("userData", Path("user-files")).empty());
  Ready reply;
  EnsureElectronAppStorageContext("one", reply.GetCallback());
  ASSERT_NE(reply.Get<0>(), nullptr);
  EXPECT_EQ(reply.Get<0>()->GetPath(), Path("sessions"));
  EXPECT_TRUE(Set("userData", Path("later-user-files")).empty());
  EXPECT_EQ(Get("userData"), Path("later-user-files"));
  EXPECT_EQ(Get("sessionData"), Path("sessions"));
  EXPECT_TRUE(Set("sessionData", Path("other-sessions"))
                  .starts_with("ERR_INVALID_STATE:"));
}

TEST_F(ElectronAppStorageTest,
       InvalidAndConflictingPathsLeaveOriginalConfigurationIntact) {
  ASSERT_TRUE(base::WriteFile(Path("file"), "content"));
  EXPECT_TRUE(Set("userData", Path("file")).starts_with("ENOTDIR:"));
  EXPECT_TRUE(Set("userData", base::FilePath(FILE_PATH_LITERAL("relative")))
                  .starts_with("ERR_INVALID_ARG_VALUE:"));
  EXPECT_EQ(Get("userData"), Path("legacy-user-data"));
  EXPECT_TRUE(
      Set("userData", host_->GetPath()).starts_with("ERR_RESOURCE_BUSY:"));
  ASSERT_TRUE(Register("two"));
  ASSERT_TRUE(Set("userData", Path("exclusive")).empty());
  EXPECT_TRUE(Set("userData", Path("exclusive"), "two")
                  .starts_with("ERR_RESOURCE_BUSY:"));
  EXPECT_EQ(Get("userData", "two"), Path("legacy-user-data"));
}

TEST_F(ElectronAppStorageTest,
       OverlappingDirectoriesUseTheSameAliasRulesAsReservations) {
  ASSERT_TRUE(Register("two"));
  ASSERT_TRUE(Set("userData", Path("exclusive")).empty());
  EXPECT_TRUE(Set("userData", Path("exclusive").AppendASCII("child"), "two")
                  .starts_with("ERR_RESOURCE_BUSY:"));
#if BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC)
  // These paths do not exist yet, so canonicalizing their existing ancestor
  // cannot resolve the differing spellings for us.
  EXPECT_TRUE(Set("userData", Path("EXCLUSIVE").AppendASCII("child"), "two")
                  .starts_with("ERR_RESOURCE_BUSY:"));
#endif
  ASSERT_TRUE(Set("userData", Path("exclusive").AppendASCII("child")).empty());
  EXPECT_TRUE(Set("userData", Path("exclusive"), "two")
                  .starts_with("ERR_RESOURCE_BUSY:"));
#if BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC)
  EXPECT_TRUE(Set("userData", Path("EXCLUSIVE"), "two")
                  .starts_with("ERR_RESOURCE_BUSY:"));
#endif
  EXPECT_EQ(Get("userData", "two"), Path("legacy-user-data"));
}

TEST_F(ElectronAppStorageTest,
       ResolvesExistingDirectoryAliasesBeforeReservingPaths) {
#if BUILDFLAG(IS_WIN)
  ASSERT_TRUE(base::CreateDirectory(Path("directory-alias")));
  auto alias = base::test::FilePathReparsePoint::Create(Path("directory-alias"),
                                                        root_.GetPath());
  ASSERT_TRUE(alias.has_value());
#elif BUILDFLAG(IS_POSIX)
  ASSERT_TRUE(
      base::CreateSymbolicLink(root_.GetPath(), Path("directory-alias")));
#else
  GTEST_SKIP() << "Directory alias test requires junction or symlink support";
#endif
  EXPECT_TRUE(Set("userData", Path("directory-alias").AppendASCII("host"))
                  .starts_with("ERR_RESOURCE_BUSY:"));
  ASSERT_TRUE(
      Set("userData", Path("directory-alias").AppendASCII("selected")).empty());
  base::FilePath canonical_root;
  ASSERT_TRUE(base::NormalizeFilePath(root_.GetPath(), &canonical_root));
  EXPECT_EQ(Get("userData"), canonical_root.AppendASCII("selected"));
  EXPECT_FALSE(base::PathExists(Path("selected")));
  ASSERT_TRUE(Register("two"));
  EXPECT_TRUE(Set("userData", Path("selected"), "two")
                  .starts_with("ERR_RESOURCE_BUSY:"));
  EXPECT_EQ(Get("userData", "two"), Path("legacy-user-data"));
}

#if BUILDFLAG(IS_WIN) || BUILDFLAG(IS_POSIX)
TEST_F(ElectronAppStorageTest, HostDirectoryAliasesCannotBypassOverlapChecks) {
  ASSERT_NO_FATAL_FAILURE(UseAliasedHost());
  ASSERT_TRUE(Register("one"));
  EXPECT_TRUE(
      Set("userData", Path("real-host")).starts_with("ERR_RESOURCE_BUSY:"));
  EXPECT_TRUE(Set("userData", Path("real-host").AppendASCII("child"))
                  .starts_with("ERR_RESOURCE_BUSY:"));
  EXPECT_EQ(Get("userData"), Path("legacy-user-data"));
}

TEST_F(ElectronAppStorageTest, LegacyPathAliasesRemainIdempotent) {
  ASSERT_NO_FATAL_FAILURE(UseAliasedHost());
  ASSERT_TRUE(
      RegisterElectronAppStorage(host_.get(), "one-container", "one",
                                 Path("host-alias").AppendASCII("user-data"))
          .has_value());
  ASSERT_TRUE(
      Set("userData", Path("real-host").AppendASCII("user-data")).empty());
  ASSERT_TRUE(Set("sessionData", Get("sessionData")).empty());
  Ready ready;
  EnsureElectronAppStorageContext("one", ready.GetCallback());
  EXPECT_EQ(ready.Get<0>(), host_.get());
  EXPECT_TRUE(ready.Get<1>().empty());
  EXPECT_EQ(creations_, 0);
}
#endif

TEST_F(ElectronAppStorageTest,
       FailedProfileCreationCanBeReconfiguredAndRetried) {
  ASSERT_TRUE(Set("userData", Path("first")).empty());
  fail_creation_ = true;
  Ready failed;
  EnsureElectronAppStorageContext("one", failed.GetCallback());
  EXPECT_EQ(failed.Get<0>(), nullptr);
  EXPECT_TRUE(failed.Get<1>().starts_with("ERR_FAILED:"));
  environment_.RunUntilIdle();
  fail_creation_ = false;
  ASSERT_TRUE(Set("userData", Path("retry")).empty());
  Ready retry;
  EnsureElectronAppStorageContext("one", retry.GetCallback());
  ASSERT_NE(retry.Get<0>(), nullptr);
  EXPECT_EQ(retry.Get<0>()->GetPath(), Path("retry"));
}

TEST_F(ElectronAppStorageTest,
       RetirementCancelsCreationAndKeepsLeaseUntilContextIsDestroyed) {
  ASSERT_TRUE(Set("userData", Path("held")).empty());
  hold_creation_ = true;
  Ready ready;
  EnsureElectronAppStorageContext("one", ready.GetCallback());
  environment_.RunUntilIdle();
  ASSERT_TRUE(held_creation_);
  RetireElectronAppStorage("one");
  EXPECT_EQ(ready.Get<0>(), nullptr);
  EXPECT_TRUE(ready.Get<1>().starts_with("ERR_ABORTED:"));
  EXPECT_EQ(PeekElectronAppStorageContext("one"), nullptr);
  ASSERT_TRUE(Register("two"));
  EXPECT_TRUE(
      Set("userData", Path("held"), "two").starts_with("ERR_RESOURCE_BUSY:"));
  std::move(held_creation_)
      .Run(std::make_unique<StorageTestContext>(held_path_), "");
  environment_.RunUntilIdle();
  EXPECT_EQ(destructions_, 1);
  EXPECT_TRUE(Set("userData", Path("held"), "two").empty());
}

TEST_F(ElectronAppStorageTest,
       RetiringBeforePostedSuccessRejectsTheQueuedReply) {
  ASSERT_TRUE(Set("userData", Path("held")).empty());
  hold_creation_ = true;
  Ready ready;
  EnsureElectronAppStorageContext("one", ready.GetCallback());
  environment_.RunUntilIdle();
  ASSERT_TRUE(held_creation_);
  std::move(held_creation_)
      .Run(std::make_unique<StorageTestContext>(held_path_), "");
  RetireElectronAppStorage("one");
  EXPECT_EQ(ready.Get<0>(), nullptr);
  EXPECT_TRUE(ready.Get<1>().starts_with("ERR_ABORTED:"));
}

TEST_F(ElectronAppStorageTest, HostShutdownRetiresItsApplicationStorageOwners) {
  Ready ready;
  EnsureElectronAppStorageContext("one", ready.GetCallback());
  ASSERT_EQ(ready.Get<0>(), host_.get());
  host_.reset();
  EXPECT_EQ(PeekElectronAppStorageContext("one"), nullptr);
  EXPECT_TRUE(GetElectronAppStorageUserDataPath("one").empty());
  Ready after;
  EnsureElectronAppStorageContext("one", after.GetCallback());
  EXPECT_EQ(after.Get<0>(), nullptr);
  EXPECT_TRUE(after.Get<1>().starts_with("ERR_ABORTED:"));
}

TEST_F(ElectronAppStorageTest,
       DedicatedStorageShutdownCallbackWaitsForContextRelease) {
  auto context = std::make_unique<StorageTestContext>(Path("dedicated"));
  content::EnableDedicatedStorageServiceForBrowserContext(context.get());
  base::test::TestFuture<void> complete;
  auto shutdown = content::MakeStorageServiceShutdownClosure(
      context.get(), complete.GetCallback());
  std::move(shutdown).Run();
  environment_.RunUntilIdle();
  EXPECT_FALSE(complete.IsReady());
  context.reset();
  environment_.RunUntilIdle();
  EXPECT_TRUE(complete.IsReady());
}

#if !BUILDFLAG(IS_FUCHSIA)
MULTIPROCESS_TEST_MAIN(ElectronStorageLeaseChild) {
  const auto* command = base::CommandLine::ForCurrentProcess();
  base::File file(
      command->GetSwitchValuePath("storage-lock-file"),
      base::File::FLAG_OPEN | base::File::FLAG_READ | base::File::FLAG_WRITE);
  if (!file.IsValid()) {
    return 2;
  }
  const bool locked =
      file.Lock(base::File::LockMode::kExclusive) == base::File::FILE_OK;
  return locked == !command->HasSwitch("expect-storage-locked") ? 0 : 3;
}

TEST_F(ElectronAppStorageTest,
       OperatingSystemLeaseExcludesOtherProcessesUntilRetirement) {
  ASSERT_TRUE(Set("userData", Path("process-lock")).empty());
  Ready ready;
  EnsureElectronAppStorageContext("one", ready.GetCallback());
  ASSERT_NE(ready.Get<0>(), nullptr);
  auto verify = [&](bool expect_locked) {
    auto command = base::GetMultiProcessTestChildBaseCommandLine();
    command.AppendSwitchPath(
        "storage-lock-file",
        Path("process-lock").AppendASCII(".xenon-storage.lock"));
    if (expect_locked) {
      command.AppendSwitch("expect-storage-locked");
    }
    base::LaunchOptions options;
#if BUILDFLAG(IS_WIN)
    options.start_hidden = true;
#endif
    auto child = base::SpawnMultiProcessTestChild("ElectronStorageLeaseChild",
                                                  command, options);
    ASSERT_TRUE(child.IsValid());
    int exit_code = -1;
    const bool exited = base::WaitForMultiprocessTestChildExit(
        child, base::Seconds(10), &exit_code);
    if (!exited)
      EXPECT_TRUE(base::TerminateMultiProcessTestChild(child, 1, true));
    ASSERT_TRUE(exited);
    EXPECT_EQ(exit_code, 0);
  };
  verify(true);
  RetireElectronAppStorage("one");
  environment_.RunUntilIdle();
  verify(false);
}
#endif

}  // namespace
}  // namespace xenon::ipc
