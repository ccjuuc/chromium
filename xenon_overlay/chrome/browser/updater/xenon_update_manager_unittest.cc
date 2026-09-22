// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/updater/xenon_update_manager.h"

#include <string>

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/test/task_environment.h"
#include "mojo/core/embedder/embedder.h"
#include "services/network/public/cpp/weak_wrapper_shared_url_loader_factory.h"
#include "services/network/test/test_url_loader_factory.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "xenon_overlay/chrome/browser/updater/xenon_update_patcher.h"
#include "xenon_overlay/chrome/browser/updater/xenon_update_types.h"

namespace xenon::updater {

namespace {

class TestUpdateObserver : public XenonUpdateManager::Observer {
 public:
  void OnCheckingForUpdate() override { checking_count_++; }
  void OnUpdateAvailable(const UpdateManifest& manifest) override {
    available_manifest_ = manifest;
  }
  void OnUpdateNotAvailable(const std::string& version) override {
    not_available_version_ = version;
  }
  void OnDownloadProgress(const DownloadProgress& progress) override {
    last_progress_ = progress;
  }
  void OnUpdateDownloaded(const UpdateManifest& manifest) override {
    downloaded_manifest_ = manifest;
  }
  void OnUpdateError(const std::string& error_message) override {
    last_error_ = error_message;
  }

  int checking_count_ = 0;
  std::optional<UpdateManifest> available_manifest_;
  std::string not_available_version_;
  std::optional<DownloadProgress> last_progress_;
  std::optional<UpdateManifest> downloaded_manifest_;
  std::string last_error_;
};

}  // namespace

class XenonUpdateManagerTest : public testing::Test {
 public:
  XenonUpdateManagerTest()
      : task_environment_(base::test::TaskEnvironment::MainThreadType::IO),
        shared_factory_(
            base::MakeRefCounted<network::WeakWrapperSharedURLLoaderFactory>(
                &test_url_loader_factory_)) {}

  void SetUp() override {
    static bool mojo_initialized = false;
    if (!mojo_initialized) {
      mojo::core::Init();
      mojo_initialized = true;
    }
    ASSERT_TRUE(temp_dir_.CreateUniqueTempDir());
    manager_ = std::make_unique<XenonUpdateManager>();
    manager_->SetURLLoaderFactoryForTesting(shared_factory_);
    manager_->AddObserver(&observer_);
  }

  void TearDown() override {
    manager_->RemoveObserver(&observer_);
    manager_.reset();
  }

 protected:
  base::test::TaskEnvironment task_environment_;
  base::ScopedTempDir temp_dir_;
  network::TestURLLoaderFactory test_url_loader_factory_;
  scoped_refptr<network::SharedURLLoaderFactory> shared_factory_;
  TestUpdateObserver observer_;
  std::unique_ptr<XenonUpdateManager> manager_;
};

TEST_F(XenonUpdateManagerTest, ParseManifest) {
  std::string json = R"({
    "target_version": "2.0.0.0",
    "is_diff": true,
    "diff_package": {
      "url": "https://example.com/patch.zucc",
      "size": 1024,
      "sha256": "ABCD1234EF"
    },
    "full_package": {
      "url": "https://example.com/full.zip",
      "size": 20480,
      "sha256": "5678WXYZ"
    },
    "changelog": "New features",
    "force_update": false
  })";

  auto manifest = UpdateManifest::FromJson(json);
  ASSERT_TRUE(manifest.has_value());
  EXPECT_EQ("2.0.0.0", manifest->target_version);
  EXPECT_TRUE(manifest->is_diff);
  ASSERT_TRUE(manifest->diff_package.has_value());
  EXPECT_EQ("https://example.com/patch.zucc", manifest->diff_package->url.spec());
  EXPECT_EQ(1024, manifest->diff_package->size);
  ASSERT_TRUE(manifest->full_package.has_value());
  EXPECT_EQ("https://example.com/full.zip", manifest->full_package->url.spec());
}

TEST_F(XenonUpdateManagerTest, ZucchiniPatchGenerationAndApplication) {
  base::FilePath old_file = temp_dir_.GetPath().AppendASCII("old_binary.bin");
  base::FilePath new_file = temp_dir_.GetPath().AppendASCII("new_binary.bin");
  base::FilePath patch_file = temp_dir_.GetPath().AppendASCII("patch.zucc");
  base::FilePath restored_file = temp_dir_.GetPath().AppendASCII("restored.bin");

  std::string old_data = "Hello World! This is Xenon base version 1.0.0.";
  std::string new_data = "Hello World! This is Xenon upgraded version 2.0.0 with new features.";

  ASSERT_TRUE(base::WriteFile(old_file, old_data));
  ASSERT_TRUE(base::WriteFile(new_file, new_data));

  // Generate Zucchini patch
  bool gen_ok = XenonUpdatePatcher::GeneratePatch(old_file, new_file, patch_file);
  ASSERT_TRUE(gen_ok);
  EXPECT_TRUE(base::PathExists(patch_file));

  // Apply Zucchini patch
  bool apply_ok = XenonUpdatePatcher::ApplyPatch(old_file, patch_file, restored_file);
  ASSERT_TRUE(apply_ok);
  EXPECT_TRUE(base::PathExists(restored_file));

  std::string restored_data;
  ASSERT_TRUE(base::ReadFileToString(restored_file, &restored_data));
  EXPECT_EQ(new_data, restored_data);
}

TEST_F(XenonUpdateManagerTest, CheckUpdateAvailableAndNotAvailable) {
  manager_->SetCurrentVersionForTesting("1.0.0.0");

  std::string feed_url = "https://update.xenon.com/check";
  std::string response_newer = R"({
    "target_version": "1.1.0.0",
    "is_diff": false,
    "url": "https://update.xenon.com/package.zip",
    "size": 5000,
    "sha256": "1234"
  })";

  test_url_loader_factory_.AddResponse(feed_url, response_newer);
  manager_->CheckForUpdates(feed_url);

  task_environment_.RunUntilIdle();

  EXPECT_EQ(1, observer_.checking_count_);
  ASSERT_TRUE(observer_.available_manifest_.has_value());
  EXPECT_EQ("1.1.0.0", observer_.available_manifest_->target_version);

  // When version is same or older
  manager_->SetCurrentVersionForTesting("1.1.0.0");
  observer_.available_manifest_.reset();

  manager_->CheckForUpdates(feed_url);
  task_environment_.RunUntilIdle();

  EXPECT_FALSE(observer_.available_manifest_.has_value());
  EXPECT_EQ("1.1.0.0", observer_.not_available_version_);
}

TEST_F(XenonUpdateManagerTest, PlanBVersionedDirectoryStaging) {
  base::FilePath install_dir = temp_dir_.GetPath().AppendASCII("app_install");
  base::FilePath old_ver_dir = install_dir.AppendASCII("1.0.0.0");
  ASSERT_TRUE(base::CreateDirectory(old_ver_dir));

  base::FilePath old_dll = old_ver_dir.Append(FILE_PATH_LITERAL("chrome.dll"));
  base::FilePath old_asset = old_ver_dir.AppendASCII("resources.pak");
  std::string old_dll_content = "Xenon Chrome DLL version 1.0.0.0 binary content.";
  std::string asset_content = "PAK resource pack content 12345.";
  ASSERT_TRUE(base::WriteFile(old_dll, old_dll_content));
  ASSERT_TRUE(base::WriteFile(old_asset, asset_content));

  // Prepare new dll content and generate diff patch
  std::string new_dll_content = "Xenon Chrome DLL version 1.1.0.0 updated binary content!";
  base::FilePath temp_new_dll = temp_dir_.GetPath().AppendASCII("temp_new.dll");
  base::FilePath patch_file = temp_dir_.GetPath().AppendASCII("patch.zucc");
  ASSERT_TRUE(base::WriteFile(temp_new_dll, new_dll_content));
  ASSERT_TRUE(XenonUpdatePatcher::GeneratePatch(old_dll, temp_new_dll, patch_file));

  // Run Plan B PrepareVersionDirectory
  base::FilePath target_ver_dir = install_dir.AppendASCII("1.1.0.0");
  base::FilePath temp_staging = temp_dir_.GetPath().AppendASCII("temp_staging");

  auto result = XenonUpdateManager::PrepareVersionDirectory(
      old_ver_dir, old_dll, patch_file, target_ver_dir, temp_staging);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(target_ver_dir, result.staged_dir);
  EXPECT_TRUE(base::DirectoryExists(target_ver_dir));

  // Verify patched DLL exists and has updated content
  base::FilePath new_dll = target_ver_dir.Append(FILE_PATH_LITERAL("chrome.dll"));
  EXPECT_TRUE(base::PathExists(new_dll));
  std::string actual_new_dll;
  ASSERT_TRUE(base::ReadFileToString(new_dll, &actual_new_dll));
  EXPECT_EQ(new_dll_content, actual_new_dll);

  // Verify non-dll asset was replicated into the new version directory
  base::FilePath replicated_asset = target_ver_dir.AppendASCII("resources.pak");
  EXPECT_TRUE(base::PathExists(replicated_asset));
  std::string actual_asset;
  ASSERT_TRUE(base::ReadFileToString(replicated_asset, &actual_asset));
  EXPECT_EQ(asset_content, actual_asset);
}

TEST_F(XenonUpdateManagerTest, PlanBCleanupOldVersions) {
  base::FilePath install_dir = temp_dir_.GetPath().AppendASCII("app_cleanup");
  base::FilePath ver_0_9 = install_dir.AppendASCII("0.9.0.0");
  base::FilePath ver_1_0 = install_dir.AppendASCII("1.0.0.0");
  base::FilePath ver_1_1 = install_dir.AppendASCII("1.1.0.0");
  base::FilePath non_ver_dir = install_dir.AppendASCII("user_data_profile");

  ASSERT_TRUE(base::CreateDirectory(ver_0_9));
  ASSERT_TRUE(base::CreateDirectory(ver_1_0));
  ASSERT_TRUE(base::CreateDirectory(ver_1_1));
  ASSERT_TRUE(base::CreateDirectory(non_ver_dir));

  // Call cleanup with current version 1.1.0.0
  XenonUpdateManager::DoCleanupOldVersions(install_dir, "1.1.0.0");

  // Old versions 0.9 and 1.0 should be removed
  EXPECT_FALSE(base::DirectoryExists(ver_0_9));
  EXPECT_FALSE(base::DirectoryExists(ver_1_0));

  // Current version 1.1 and non-version directory should be preserved
  EXPECT_TRUE(base::DirectoryExists(ver_1_1));
  EXPECT_TRUE(base::DirectoryExists(non_ver_dir));
}

}  // namespace xenon::updater
