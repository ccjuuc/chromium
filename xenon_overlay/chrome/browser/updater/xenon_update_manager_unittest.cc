// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/updater/xenon_update_manager.h"

#include <string>
#include <utility>
#include <vector>

#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/memory/raw_ref.h"
#include "base/strings/string_number_conversions.h"
#include "base/test/bind.h"
#include "base/test/task_environment.h"
#include "base/threading/thread_restrictions.h"
#include "base/values.h"
#include "build/build_config.h"
#include "crypto/sha2.h"
#include "mojo/core/embedder/embedder.h"
#include "net/base/url_util.h"
#include "services/network/public/cpp/weak_wrapper_shared_url_loader_factory.h"
#include "services/network/test/test_url_loader_factory.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/zlib/google/zip.h"
#include "url/gurl.h"
#include "xenon_overlay/chrome/browser/updater/xenon_update_installer.h"
#include "xenon_overlay/chrome/browser/updater/xenon_update_patcher.h"
#include "xenon_overlay/chrome/browser/updater/xenon_update_types.h"
#include "xenon_overlay/chrome/browser/updater/xenon_updater_test_support.h"

#if BUILDFLAG(IS_WIN)
#include <array>
#include <map>

#include "base/base_paths.h"
#include "base/environment.h"
#include "base/files/file.h"
#include "base/test/scoped_path_override.h"
#include "base/test/test_reg_util_win.h"
#include "base/version.h"
#include "crypto/hash.h"
#endif

#if BUILDFLAG(IS_MAC)
#include <sys/stat.h>

#include "third_party/zlib/contrib/minizip/zip.h"
#endif

namespace xenon::updater {

namespace {

std::string Sha256(const std::string& contents) {
  return base::HexEncode(crypto::SHA256HashString(contents));
}

UpdatePackageInfo TestPackage(const char* url, const std::string& contents) {
  return {GURL(url), static_cast<int64_t>(contents.size()), Sha256(contents)};
}

const char* TestPlatform() {
#if BUILDFLAG(IS_MAC)
  return "mac";
#elif BUILDFLAG(IS_WIN)
  return "win";
#else
  return "linux";
#endif
}

#if BUILDFLAG(IS_WIN)
using FileHashes = std::map<base::FilePath, std::string>;

bool AddFileHash(const base::FilePath& file,
                 const base::FilePath& relative,
                 FileHashes* hashes) {
  base::File input(file, base::File::FLAG_OPEN | base::File::FLAG_READ);
  std::array<uint8_t, crypto::hash::kSha256Size> digest;
  if (!crypto::hash::HashFile(crypto::hash::kSha256, &input, digest)) {
    return false;
  }
  hashes->emplace(relative, base::HexEncode(digest));
  return true;
}

bool SnapshotTree(const base::FilePath& root, FileHashes* hashes) {
  hashes->clear();
  if (!base::DirectoryExists(root)) {
    return false;
  }
  base::FileEnumerator files(
      root, true, base::FileEnumerator::FILES, base::FilePath::StringType(),
      base::FileEnumerator::FolderSearchPolicy::ALL,
      base::FileEnumerator::ErrorPolicy::STOP_ENUMERATION);
  for (base::FilePath file = files.Next(); !file.empty(); file = files.Next()) {
    base::FilePath relative;
    if (!root.AppendRelativePath(file, &relative) ||
        !AddFileHash(file, relative, hashes)) {
      return false;
    }
  }
  return files.GetError() == base::File::FILE_OK;
}
#endif

base::DictValue RegularAction(const std::string& target,
                              const std::string& type,
                              const std::string& input,
                              const std::string& contents) {
  base::DictValue action;
  action.Set("target", target);
  action.Set("action", type);
  action.Set(type == "add" ? "source" : "base", input);
  action.Set("sha256", Sha256(contents));
  return action;
}

base::DictValue PatchManifest(base::ListValue actions,
                              const std::string& platform = "mac") {
  base::DictValue manifest;
  manifest.Set("platform", platform);
  manifest.Set("base_version", "1.0.0.1");
  manifest.Set("target_version", "1.0.0.2");
  manifest.Set("actions", std::move(actions));
  return manifest;
}

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
  void OnUpdateStateChanged(UpdateState state) override {
    phase_states_.push_back(state);
  }
  void OnUpdateDownloaded(const UpdateManifest& manifest) override {
    downloaded_count_++;
    downloaded_manifest_ = manifest;
  }
  void OnUpdateError(const std::string& error_message) override {
    last_error_ = error_message;
  }
  void OnInstallerOpened() override { installer_opened_count_++; }

  void OnElevationApprovalRequired() override { elevation_approval_count_++; }

  int elevation_approval_count_ = 0;
  int checking_count_ = 0;
  int downloaded_count_ = 0;
  int installer_opened_count_ = 0;
  std::optional<UpdateManifest> available_manifest_;
  std::string not_available_version_;
  std::optional<DownloadProgress> last_progress_;
  std::vector<UpdateState> phase_states_;
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
    InitializeUpdaterMojoForTesting();
    ASSERT_TRUE(temp_dir_.CreateUniqueTempDir());
    manager_ = std::make_unique<XenonUpdateManager>();
    manager_->SetSystemUpdaterForTesting(nullptr);
    manager_->SetURLLoaderFactoryForTesting(shared_factory_);
    manager_->SetAutoDownload(false);
    manager_->AddObserver(&observer_);
  }

  void TearDown() override {
    manager_->RemoveObserver(&observer_);
    manager_.reset();
  }

 protected:
  using Payloads = std::vector<std::pair<std::string, std::string>>;

  base::FilePath WriteRelative(const base::FilePath& root,
                               const std::string& relative,
                               const std::string& contents) {
    const base::FilePath file =
        root.Append(base::FilePath::FromUTF8Unsafe(relative));
    EXPECT_TRUE(base::CreateDirectory(file.DirName()));
    EXPECT_TRUE(base::WriteFile(file, contents));
    return file;
  }

  base::FilePath MakePatch(const base::DictValue& manifest,
                           const Payloads& payloads = {}) {
    const std::string name = "patch_" + base::NumberToString(++patch_sequence_);
    const base::FilePath bundle = temp_dir_.GetPath().AppendASCII(name);
    EXPECT_TRUE(base::CreateDirectory(bundle));
    for (const auto& [relative, contents] : payloads) {
      WriteRelative(bundle, relative, contents);
    }
    std::string json;
    EXPECT_TRUE(base::JSONWriter::Write(manifest, &json));
    WriteRelative(bundle, "manifest.json", json);
    const base::FilePath archive =
        temp_dir_.GetPath().AppendASCII(name + ".zip");
    EXPECT_TRUE(zip::Zip(bundle, archive, /*include_hidden_files=*/true));
    return archive;
  }

  XenonUpdateManager::VersionStagingResult StageManifest(
      const base::DictValue& manifest,
      const Payloads& payloads = {},
      const std::string& expected_base = {},
      const std::string& expected_target = {}) {
    const base::FilePath source = temp_dir_.GetPath().AppendASCII("Source.app");
    return XenonUpdateManager::PrepareVersionDirectory(
        source, source.AppendASCII("base"), MakePatch(manifest, payloads),
        temp_dir_.GetPath().AppendASCII("Target.app"),
        temp_dir_.GetPath().AppendASCII("staging"), {}, expected_base,
        expected_target);
  }

  void ExpectRejected(const XenonUpdateManager::VersionStagingResult& result) {
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.error.empty());
    EXPECT_FALSE(
        base::PathExists(temp_dir_.GetPath().AppendASCII("Target.app")));
    EXPECT_FALSE(base::PathExists(temp_dir_.GetPath().AppendASCII("staging")));
  }

  void BeginFeedDownload(const UpdateManifest& feed) {
    manager_->SetCurrentVersionForTesting("1.0.0.1");
    const base::FilePath install =
        temp_dir_.GetPath().AppendASCII("Installed.app");
    manager_->SetInstallDirForTesting(install);
    manager_->SetBaseFileForTesting(
        WriteRelative(install, "Contents/MacOS/app", "installed executable"));
    manager_->SetAutoDownload(true);
    std::string response;
    ASSERT_TRUE(base::JSONWriter::Write(feed.ToValue(), &response));
    const std::string url = "https://update.xenon.test/check?platform=" +
                            std::string(TestPlatform()) + "&version=1.0.0.1";
    test_url_loader_factory_.AddResponse(url, response);
    manager_->CheckForUpdates(url);
    task_environment_.RunUntilIdle();
  }

#if BUILDFLAG(IS_WIN)
  void VerifyWindowsDiffFallback(bool corrupt_download) {
    const base::FilePath full_tree = temp_dir_.GetPath().AppendASCII("full");
    WriteRelative(full_tree, "Chrome-bin/1.0.0.2/resource.dat",
                  "full resource");
    WriteRelative(full_tree, "Chrome-bin/browser.exe", "full launcher");
    const base::FilePath full_archive =
        temp_dir_.GetPath().AppendASCII("full.zip");
    ASSERT_TRUE(
        zip::Zip(full_tree, full_archive, /*include_hidden_files=*/true));
    std::string full;
    ASSERT_TRUE(base::ReadFileToString(full_archive, &full));
    base::ListValue actions;
    actions.Append(RegularAction("1.0.0.2/resource.dat", "copy",
                                 "1.0.0.1/missing.dat", "missing base"));
    std::string diff;
    ASSERT_TRUE(base::ReadFileToString(
        MakePatch(PatchManifest(std::move(actions), "win")), &diff));
    UpdateManifest feed;
    feed.target_version = "1.0.0.2";
    feed.is_diff = true;
    feed.diff_package = TestPackage("https://update.xenon.test/diff.zip", diff);
    feed.full_package = TestPackage("https://update.xenon.test/full.zip", full);
    if (corrupt_download) {
      diff[0] ^= 1;  // Same byte count, wrong SHA-256.
    }
    test_url_loader_factory_.AddResponse(feed.diff_package->url.spec(), diff);
    // Let both downloads finish in the same run so the failed diff's deferred
    // deletion can overlap full-package verification and extraction.
    test_url_loader_factory_.AddResponse(feed.full_package->url.spec(), full);
    BeginFeedDownload(feed);
    ASSERT_EQ(UpdateState::kUpdateDownloaded, manager_->GetState())
        << observer_.last_error_;
    ASSERT_TRUE(observer_.downloaded_manifest_);
    EXPECT_FALSE(observer_.downloaded_manifest_->is_diff);
    EXPECT_EQ(1, observer_.downloaded_count_);
    EXPECT_EQ(3u, test_url_loader_factory_.total_requests());
    EXPECT_TRUE(observer_.last_error_.empty());
    std::string contents;
    ASSERT_TRUE(base::ReadFileToString(
        manager_->GetStagedPath().AppendASCII("1.0.0.2/resource.dat"),
        &contents));
    EXPECT_EQ("full resource", contents);
    ASSERT_TRUE(base::ReadFileToString(
        manager_->GetStagedPath().AppendASCII("browser.exe"), &contents));
    EXPECT_EQ("full launcher", contents);
    // Extraction is under full_<attempt>/Chrome-bin. The successfully verified
    // fallback ZIP must remain after every queued failed-download cleanup.
    const base::FilePath downloads =
        manager_->GetStagedPath().DirName().DirName();
    ASSERT_TRUE(base::ReadFileToString(
        downloads.AppendASCII("2_update_patch.zip"), &contents));
    EXPECT_EQ(full, contents);
    EXPECT_FALSE(base::PathExists(temp_dir_.GetPath()
                                      .AppendASCII("Installed.app")
                                      .AppendASCII("1.0.0.2")));
  }
#endif

#if BUILDFLAG(IS_MAC)
  void StartReadyPrivilegedDiff() {
    manager_->SetDifferentialEligibilityForTesting(
        base::BindLambdaForTesting([](base::FilePath) {
          return XenonUpdateManager::DifferentialEligibilityResult{
              true, true, {}};
        }));
    base::ListValue actions;
    actions.Append(RegularAction("Contents/MacOS/app", "copy",
                                 "Contents/MacOS/app", "installed executable"));
    std::string diff;
    ASSERT_TRUE(base::ReadFileToString(
        MakePatch(PatchManifest(std::move(actions))), &diff));
    const std::string full = "xar!full installer package fixture";
    UpdateManifest feed;
    feed.target_version = "1.0.0.2";
    feed.is_diff = true;
    feed.diff_package = TestPackage("https://update.xenon.test/diff.zip", diff);
    feed.full_package = TestPackage("https://update.xenon.test/full.pkg", full);
    test_url_loader_factory_.AddResponse(feed.diff_package->url.spec(), diff);
    test_url_loader_factory_.AddResponse(feed.full_package->url.spec(), full);
    BeginFeedDownload(feed);
  }

  std::string MakeFullMacZip(const std::string& version = "1.0.0.2") {
    const base::FilePath archive = temp_dir_.GetPath().AppendASCII(
        "full_" + base::NumberToString(++patch_sequence_) + ".zip");
    zipFile file = zipOpen(archive.value().c_str(), APPEND_STATUS_CREATE);
    EXPECT_NE(nullptr, file);
    if (!file) {
      return {};
    }
    auto add = [&](const char* name, const std::string& contents, int mode) {
      zip_fileinfo info = {};
      info.external_fa = static_cast<uLong>(mode) << 16;
      EXPECT_EQ(ZIP_OK, zipOpenNewFileInZip(file, name, &info, nullptr, 0,
                                            nullptr, 0, nullptr, 0, 0));
      EXPECT_EQ(ZIP_OK, zipWriteInFileInZip(
                            file, contents.data(),
                            static_cast<unsigned int>(contents.size())));
      EXPECT_EQ(ZIP_OK, zipCloseFileInZip(file));
    };
    add("Updated.app/Contents/Info.plist",
        "<?xml version=\"1.0\"?><plist version=\"1.0\"><dict>"
        "<key>CFBundleShortVersionString</key><string>" +
            version + "</string></dict></plist>",
        S_IFREG | 0644);
    add("Updated.app/Contents/MacOS/app", "updated executable", S_IFREG | 0755);
    add("Updated.app/Contents/Frameworks/Versions/A/resource", "framework data",
        S_IFREG | 0644);
    add("Updated.app/Contents/Frameworks/Current", "Versions/A",
        S_IFLNK | 0777);
    EXPECT_EQ(ZIP_OK, zipClose(file, nullptr));
    std::string contents;
    EXPECT_TRUE(base::ReadFileToString(archive, &contents));
    return contents;
  }
#endif

  base::test::TaskEnvironment task_environment_;
  base::ScopedTempDir temp_dir_;
  network::TestURLLoaderFactory test_url_loader_factory_;
  scoped_refptr<network::SharedURLLoaderFactory> shared_factory_;
  TestUpdateObserver observer_;
  std::unique_ptr<XenonUpdateManager> manager_;
  int patch_sequence_ = 0;
};

TEST_F(XenonUpdateManagerTest, ParseManifest) {
  std::string json = R"({
    "target_version": "2.0.0.0",
    "is_diff": true,
    "diff_package": {
      "url": "https://example.com/patch.zucc",
      "size": 1024,
      "sha256": "0000000000000000000000000000000000000000000000000000000000000000"
    },
    "full_package": {
      "url": "https://example.com/full.zip",
      "size": 20480,
      "sha256": "0000000000000000000000000000000000000000000000000000000000000000"
    },
    "changelog": "New features",
    "force_update": false
  })";

  auto manifest = UpdateManifest::FromJson(json);
  ASSERT_TRUE(manifest.has_value());
  EXPECT_EQ("2.0.0.0", manifest->target_version);
  EXPECT_TRUE(manifest->is_diff);
  ASSERT_TRUE(manifest->diff_package.has_value());
  EXPECT_EQ("https://example.com/patch.zucc",
            manifest->diff_package->url.spec());
  EXPECT_EQ(1024, manifest->diff_package->size);
  ASSERT_TRUE(manifest->full_package.has_value());
  EXPECT_EQ("https://example.com/full.zip", manifest->full_package->url.spec());
}

TEST_F(XenonUpdateManagerTest, ZucchiniPatchGenerationAndApplication) {
  base::FilePath old_file = temp_dir_.GetPath().AppendASCII("old_binary.bin");
  base::FilePath new_file = temp_dir_.GetPath().AppendASCII("new_binary.bin");
  base::FilePath patch_file = temp_dir_.GetPath().AppendASCII("patch.zucc");
  base::FilePath restored_file =
      temp_dir_.GetPath().AppendASCII("restored.bin");

  std::string old_data = "Hello World! This is Xenon base version 1.0.0.";
  std::string new_data =
      "Hello World! This is Xenon upgraded version 2.0.0 with new features.";

  ASSERT_TRUE(base::WriteFile(old_file, old_data));
  ASSERT_TRUE(base::WriteFile(new_file, new_data));

  // Generate Zucchini patch
  bool gen_ok =
      XenonUpdatePatcher::GeneratePatch(old_file, new_file, patch_file);
  ASSERT_TRUE(gen_ok);
  EXPECT_TRUE(base::PathExists(patch_file));

  // Apply Zucchini patch
  bool apply_ok =
      XenonUpdatePatcher::ApplyPatch(old_file, patch_file, restored_file);
  ASSERT_TRUE(apply_ok);
  EXPECT_TRUE(base::PathExists(restored_file));

  std::string restored_data;
  ASSERT_TRUE(base::ReadFileToString(restored_file, &restored_data));
  EXPECT_EQ(new_data, restored_data);
}

TEST_F(XenonUpdateManagerTest, CheckUpdateAvailableAndNotAvailable) {
  manager_->SetCurrentVersionForTesting("1.0.0.0");

  std::string feed_url = "https://update.xenon.com/check";
  const char* platform =
#if BUILDFLAG(IS_MAC)
      "mac";
#elif BUILDFLAG(IS_WIN)
      "win";
#else
      "linux";
#endif
  auto request_url = [&](const std::string& version) {
    return net::AppendQueryParameter(
               net::AppendQueryParameter(GURL(feed_url), "platform", platform),
               "version", version)
        .spec();
  };
  std::string response_newer = R"({
    "target_version": "1.1.0.0",
    "is_diff": false,
    "url": "https://update.xenon.com/package.zip",
    "size": 5000,
    "sha256": "0000000000000000000000000000000000000000000000000000000000000000"
  })";

  test_url_loader_factory_.AddResponse(request_url("1.0.0.0"), response_newer);
  test_url_loader_factory_.AddResponse(request_url("1.1.0.0"), response_newer);
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
  std::string old_dll_content =
      "Xenon Chrome DLL version 1.0.0.0 binary content.";
  std::string asset_content = "PAK resource pack content 12345.";
  ASSERT_TRUE(base::WriteFile(old_dll, old_dll_content));
  ASSERT_TRUE(base::WriteFile(old_asset, asset_content));

  // Prepare new dll content and generate diff patch
  std::string new_dll_content =
      "Xenon Chrome DLL version 1.1.0.0 updated binary content!";
  base::FilePath temp_new_dll = temp_dir_.GetPath().AppendASCII("temp_new.dll");
  base::FilePath patch_file = temp_dir_.GetPath().AppendASCII("patch.zucc");
  ASSERT_TRUE(base::WriteFile(temp_new_dll, new_dll_content));
  ASSERT_TRUE(
      XenonUpdatePatcher::GeneratePatch(old_dll, temp_new_dll, patch_file));

  // Run Plan B PrepareVersionDirectory
  base::FilePath target_ver_dir = install_dir.AppendASCII("1.1.0.0");
  base::FilePath temp_staging = temp_dir_.GetPath().AppendASCII("temp_staging");

  auto result = XenonUpdateManager::PrepareVersionDirectory(
      old_ver_dir, old_dll, patch_file, target_ver_dir, temp_staging);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(target_ver_dir, result.staged_dir);
  EXPECT_TRUE(base::DirectoryExists(target_ver_dir));

  // Verify patched DLL exists and has updated content
  base::FilePath new_dll =
      target_ver_dir.Append(FILE_PATH_LITERAL("chrome.dll"));
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

TEST_F(XenonUpdateManagerTest, BundleManifestStagesFromAppNotTempParent) {
  base::FilePath app = temp_dir_.GetPath().AppendASCII("Old.app");
  base::FilePath resource = app.AppendASCII("Contents")
                                .AppendASCII("Resources")
                                .AppendASCII("keep.dat");
  ASSERT_TRUE(base::CreateDirectory(resource.DirName()));
  ASSERT_TRUE(base::WriteFile(resource, "keep-me"));

  base::FilePath bundle = temp_dir_.GetPath().AppendASCII("bundle");
  base::FilePath added = bundle.AppendASCII("files")
                             .AppendASCII("Contents")
                             .AppendASCII("Info.plist");
  ASSERT_TRUE(base::CreateDirectory(added.DirName()));
  ASSERT_TRUE(base::WriteFile(added, "plist"));
  const char manifest[] = R"({
    "platform": "mac",
    "base_version": "1.0.0.1",
    "target_version": "1.0.0.2",
    "actions": [
      {
        "target": "Contents/Resources/kept.dat",
        "action": "copy",
        "base": "Contents/Resources/keep.dat",
        "sha256": "c1a66b9d085b43601135241826537b5d5e215143065c41c300a5aa9cb56980ab"
      },
      {
        "target": "Contents/Info.plist",
        "action": "add",
        "source": "files/Contents/Info.plist",
        "sha256": "9ceec13202afbf12ee3abb994c669c711749c18e194326734db6123e94947e04"
      },
      {
        "target": "Contents/Frameworks/Current",
        "action": "symlink",
        "link": "../Resources/kept.dat"
      }
    ]
  })";
  ASSERT_TRUE(base::WriteFile(bundle.AppendASCII("manifest.json"), manifest));

  base::FilePath patch_zip = temp_dir_.GetPath().AppendASCII("patch.zip");
  ASSERT_TRUE(zip::Zip(bundle, patch_zip, /*include_hidden_files=*/true));

  base::FilePath target = temp_dir_.GetPath()
                              .AppendASCII("xenon_update_1.0.0.2")
                              .AppendASCII("New.app");
  auto result = XenonUpdateManager::PrepareVersionDirectory(
      app, resource, patch_zip, target,
      temp_dir_.GetPath().AppendASCII("staging"));
#if BUILDFLAG(IS_WIN)
  // Windows must reject a macOS symbolic-link action, not create a partially
  // reconstructed bundle or depend on POSIX-only filesystem APIs.
  EXPECT_FALSE(result.success);
  EXPECT_EQ("Symbolic link actions are not supported on Windows", result.error);
  EXPECT_FALSE(base::PathExists(target));
#else
  EXPECT_TRUE(result.success) << result.error;
  std::string kept;
  ASSERT_TRUE(base::ReadFileToString(target.AppendASCII("Contents")
                                         .AppendASCII("Resources")
                                         .AppendASCII("kept.dat"),
                                     &kept));
  EXPECT_EQ("keep-me", kept);
  std::string plist;
  ASSERT_TRUE(base::ReadFileToString(
      target.AppendASCII("Contents").AppendASCII("Info.plist"), &plist));
  EXPECT_EQ("plist", plist);
  base::FilePath created_link = target.AppendASCII("Contents")
                                    .AppendASCII("Frameworks")
                                    .AppendASCII("Current");
  EXPECT_TRUE(base::IsLink(created_link));
  base::FilePath link_target;
  ASSERT_TRUE(base::ReadSymbolicLink(created_link, &link_target));
  EXPECT_EQ(FILE_PATH_LITERAL("../Resources/kept.dat"), link_target.value());
#endif
}

TEST_F(XenonUpdateManagerTest, ManifestMissingCopyFailsWithoutCommitting) {
  base::ListValue actions;
  actions.Append(RegularAction("Contents/resource", "copy", "Contents/missing",
                               "expected content"));
  auto result = StageManifest(PatchManifest(std::move(actions)));
  ExpectRejected(result);
  EXPECT_NE(std::string::npos, result.error.find("Missing base file"));
}

TEST_F(XenonUpdateManagerTest, ManifestRejectsWrongCopyAndAddedFileHashes) {
  const base::FilePath source = temp_dir_.GetPath().AppendASCII("Source.app");
  WriteRelative(source, "Contents/resource", "actual content");
  for (const char* type : {"copy", "add"}) {
    SCOPED_TRACE(type);
    base::ListValue actions;
    actions.Append(RegularAction("Contents/resource", type, "Contents/resource",
                                 "different expected content"));
    auto result = StageManifest(PatchManifest(std::move(actions)),
                                {{"Contents/resource", "actual content"}});
    ExpectRejected(result);
    EXPECT_NE(std::string::npos, result.error.find("SHA-256 mismatch"));
  }
}

TEST_F(XenonUpdateManagerTest, ManifestRejectsUnknownAndDuplicateActions) {
  base::ListValue unknown;
  unknown.Append(RegularAction("Contents/resource", "execute", "", "payload"));
  auto result = StageManifest(PatchManifest(std::move(unknown)));
  ExpectRejected(result);
  EXPECT_NE(std::string::npos, result.error.find("Unknown patch action"));

  base::ListValue duplicate;
  auto action =
      RegularAction("Contents/resource", "add", "files/resource", "payload");
  duplicate.Append(action.Clone());
  duplicate.Append(std::move(action));
  result = StageManifest(PatchManifest(std::move(duplicate)),
                         {{"files/resource", "payload"}});
  ExpectRejected(result);
  EXPECT_NE(std::string::npos, result.error.find("Duplicate target"));
}

TEST_F(XenonUpdateManagerTest, ManifestRejectsUnsafeTargetsBasesAndPayloads) {
  const base::FilePath outside =
      WriteRelative(temp_dir_.GetPath(), "outside", "must remain unchanged");
  for (const char* target :
       {"../outside", "/outside", "Contents//resource", "Contents/./resource",
        "C:/outside", "Contents\\resource", ".xenon-root/launcher",
        ".xenon-deletions.json", "Contents/.xenon-reserved"}) {
    SCOPED_TRACE(target);
    base::ListValue actions;
    actions.Append(RegularAction(target, "add", "files/resource", "payload"));
    ExpectRejected(StageManifest(PatchManifest(std::move(actions)),
                                 {{"files/resource", "payload"}}));
  }
  for (const char* type : {"copy", "add"}) {
    SCOPED_TRACE(type);
    base::ListValue actions;
    actions.Append(RegularAction("Contents/resource", type, "../outside",
                                 "must remain unchanged"));
    ExpectRejected(StageManifest(PatchManifest(std::move(actions))));
  }
  std::string contents;
  ASSERT_TRUE(base::ReadFileToString(outside, &contents));
  EXPECT_EQ("must remain unchanged", contents);
}

TEST_F(XenonUpdateManagerTest, ManifestRejectsMissingHashAndInvalidModes) {
  base::ListValue no_hash;
  auto action =
      RegularAction("Contents/resource", "add", "files/resource", "payload");
  action.Remove("sha256");
  no_hash.Append(std::move(action));
  ExpectRejected(StageManifest(PatchManifest(std::move(no_hash)),
                               {{"files/resource", "payload"}}));
#if !BUILDFLAG(IS_WIN)
  base::ListValue invalid_modes;
  invalid_modes.Append(-1);
  invalid_modes.Append(01000);
  invalid_modes.Append("0755");
  invalid_modes.Append(493.5);
  invalid_modes.Append(true);
  for (const auto& mode : invalid_modes) {
    SCOPED_TRACE(mode.DebugString());
    base::ListValue actions;
    auto invalid =
        RegularAction("Contents/resource", "add", "files/resource", "payload");
    invalid.Set("mode", mode.Clone());
    actions.Append(std::move(invalid));
    ExpectRejected(StageManifest(PatchManifest(std::move(actions)),
                                 {{"files/resource", "payload"}}));
  }
#endif
}

TEST_F(XenonUpdateManagerTest, ManifestRejectsMismatchedFeedVersions) {
  for (const auto& [base, target] :
       std::vector<std::pair<std::string, std::string>>{
           {"1.0.0.0", "1.0.0.2"}, {"1.0.0.1", "1.0.0.3"}}) {
    SCOPED_TRACE(base + " -> " + target);
    base::ListValue actions;
    actions.Append(
        RegularAction("resource", "add", "files/resource", "payload"));
    auto result =
        StageManifest(PatchManifest(std::move(actions), TestPlatform()),
                      {{"files/resource", "payload"}}, base, target);
    ExpectRejected(result);
    EXPECT_NE(std::string::npos, result.error.find("mismatch"));
  }
}

TEST_F(XenonUpdateManagerTest, ManifestRejectsMismatchedFeedPlatform) {
  base::ListValue actions;
  actions.Append(RegularAction("resource", "add", "files/resource", "payload"));
  const std::string wrong_platform =
      std::string(TestPlatform()) == "mac" ? "win" : "mac";
  auto result =
      StageManifest(PatchManifest(std::move(actions), wrong_platform),
                    {{"files/resource", "payload"}}, "1.0.0.1", "1.0.0.2");
  ExpectRejected(result);
  EXPECT_NE(std::string::npos, result.error.find("mismatch"));
}

#if !BUILDFLAG(IS_WIN)
TEST_F(XenonUpdateManagerTest, ManifestRejectsEscapingAndDanglingSymlinks) {
  for (const char* link : {"../../outside", "/tmp/outside", "missing"}) {
    SCOPED_TRACE(link);
    base::DictValue action;
    action.Set("action", "symlink");
    action.Set("target", "Contents/link");
    action.Set("link", link);
    base::ListValue actions;
    actions.Append(std::move(action));
    ExpectRejected(StageManifest(PatchManifest(std::move(actions))));
  }
}

TEST_F(XenonUpdateManagerTest, ManifestDoesNotFollowSymlinkedBaseFiles) {
  const base::FilePath source = temp_dir_.GetPath().AppendASCII("Source.app");
  const base::FilePath outside =
      WriteRelative(temp_dir_.GetPath(), "outside", "content");
  ASSERT_TRUE(base::CreateDirectory(source));
  ASSERT_TRUE(base::CreateSymbolicLink(outside, source.AppendASCII("alias")));
  base::ListValue actions;
  actions.Append(
      RegularAction("Contents/resource", "copy", "alias", "content"));
  auto result = StageManifest(PatchManifest(std::move(actions)));
  ExpectRejected(result);
  EXPECT_NE(std::string::npos, result.error.find("symbolic link"));
}

TEST_F(XenonUpdateManagerTest, WindowsManifestRejectsLinkedRootSource) {
  const base::FilePath install =
      temp_dir_.GetPath().AppendASCII("installation");
  const base::FilePath current = install.AppendASCII("1.0.0.1");
  ASSERT_TRUE(base::CreateDirectory(current));
  const base::FilePath outside =
      WriteRelative(temp_dir_.GetPath(), "outside", "keep outside unchanged");
  ASSERT_TRUE(
      base::CreateSymbolicLink(outside, install.AppendASCII("launcher.exe")));
  base::ListValue actions;
  actions.Append(RegularAction("launcher.exe", "copy", "launcher.exe",
                               "keep outside unchanged"));
  const base::FilePath target = install.AppendASCII("1.0.0.2");
  auto result = XenonUpdateManager::PrepareVersionDirectory(
      current, current.AppendASCII("resource"),
      MakePatch(PatchManifest(std::move(actions), "win")), target,
      temp_dir_.GetPath().AppendASCII("staging"));
  ExpectRejected(result);
  EXPECT_FALSE(base::PathExists(target));
  EXPECT_FALSE(base::PathExists(install.AppendASCII(".xenon-root")));
  EXPECT_TRUE(base::IsLink(install.AppendASCII("launcher.exe")));
  std::string contents;
  ASSERT_TRUE(base::ReadFileToString(outside, &contents));
  EXPECT_EQ("keep outside unchanged", contents);
}

TEST_F(XenonUpdateManagerTest, ManifestPatchRestoresDeclaredExecutableMode) {
  const base::FilePath source = temp_dir_.GetPath().AppendASCII("Source.app");
  const base::FilePath old_file =
      WriteRelative(source, "Contents/MacOS/app", "old executable");
  ASSERT_TRUE(base::SetPosixFilePermissions(old_file, 0644));
  const std::string new_contents = "new executable with an additional feature";
  const base::FilePath new_file =
      WriteRelative(temp_dir_.GetPath(), "new_app", new_contents);
  const base::FilePath delta = temp_dir_.GetPath().AppendASCII("app.zucc");
  ASSERT_TRUE(XenonUpdatePatcher::GeneratePatch(old_file, new_file, delta));
  std::string patch_bytes;
  ASSERT_TRUE(base::ReadFileToString(delta, &patch_bytes));
  auto action = RegularAction("Contents/MacOS/app", "patch",
                              "Contents/MacOS/app", new_contents);
  action.Set("patch", "patches/app.zucc");
  action.Set("mode", 0755);
  base::ListValue actions;
  actions.Append(std::move(action));
  auto result = StageManifest(PatchManifest(std::move(actions)),
                              {{"patches/app.zucc", patch_bytes}});
  ASSERT_TRUE(result.success) << result.error;
  const base::FilePath staged = result.staged_dir.AppendASCII("Contents")
                                    .AppendASCII("MacOS")
                                    .AppendASCII("app");
  int mode = 0;
  ASSERT_TRUE(base::GetPosixFilePermissions(staged, &mode));
  EXPECT_EQ(0755, mode);
  std::string contents;
  ASSERT_TRUE(base::ReadFileToString(staged, &contents));
  EXPECT_EQ(new_contents, contents);
}

TEST_F(XenonUpdateManagerTest, LegacyManifestPatchInheritsBaseExecutableMode) {
  const base::FilePath source = temp_dir_.GetPath().AppendASCII("Source.app");
  const base::FilePath old_file =
      WriteRelative(source, "app", "old executable");
  ASSERT_TRUE(base::SetPosixFilePermissions(old_file, 0750));
  const std::string contents = "updated executable";
  const base::FilePath new_file =
      WriteRelative(temp_dir_.GetPath(), "new_app", contents);
  const base::FilePath delta = temp_dir_.GetPath().AppendASCII("app.zucc");
  ASSERT_TRUE(XenonUpdatePatcher::GeneratePatch(old_file, new_file, delta));
  std::string patch_bytes;
  ASSERT_TRUE(base::ReadFileToString(delta, &patch_bytes));
  auto action = RegularAction("app", "patch", "app", contents);
  action.Set("patch", "patches/app.zucc");
  base::ListValue actions;
  actions.Append(std::move(action));
  auto result = StageManifest(PatchManifest(std::move(actions)),
                              {{"patches/app.zucc", patch_bytes}});
  ASSERT_TRUE(result.success) << result.error;
  int mode = 0;
  ASSERT_TRUE(base::GetPosixFilePermissions(
      result.staged_dir.AppendASCII("app"), &mode));
  EXPECT_EQ(0750, mode);
}
#endif

TEST_F(XenonUpdateManagerTest,
       WindowsManifestStagesRootFilesWithoutChangingInstallation) {
  const base::FilePath install =
      temp_dir_.GetPath().AppendASCII("installation");
  const base::FilePath current = install.AppendASCII("1.0.0.1");
  const base::FilePath launcher =
      WriteRelative(install, "launcher.exe", "running launcher");
  const base::FilePath resource =
      WriteRelative(current, "resource", "unchanged");
  base::ListValue actions;
  actions.Append(RegularAction("launcher.exe", "add", "files/launcher.exe",
                               "new launcher"));
  actions.Append(RegularAction("1.0.0.2/resource", "copy", "1.0.0.1/resource",
                               "unchanged"));
  const base::FilePath archive =
      MakePatch(PatchManifest(std::move(actions), "win"),
                {{"files/launcher.exe", "new launcher"}});
  const base::FilePath target = install.AppendASCII("1.0.0.2");
  auto result = XenonUpdateManager::PrepareVersionDirectory(
      current, resource, archive, target,
      temp_dir_.GetPath().AppendASCII("staging"));
  ASSERT_TRUE(result.success) << result.error;
  std::string contents;
  ASSERT_TRUE(base::ReadFileToString(launcher, &contents));
  EXPECT_EQ("running launcher", contents);
  ASSERT_TRUE(base::ReadFileToString(
      target.AppendASCII(".xenon-root").AppendASCII("launcher.exe"),
      &contents));
  EXPECT_EQ("new launcher", contents);
  ASSERT_TRUE(
      base::ReadFileToString(target.AppendASCII("resource"), &contents));
  EXPECT_EQ("unchanged", contents);
}

TEST_F(XenonUpdateManagerTest,
       WindowsManifestRecordsRootDeletionsWithoutDeleting) {
  const base::FilePath install =
      temp_dir_.GetPath().AppendASCII("installation");
  const base::FilePath current = install.AppendASCII("1.0.0.1");
  const base::FilePath resource = WriteRelative(current, "resource", "keep me");
  const base::FilePath obsolete_root =
      WriteRelative(install, "obsolete.dll", "old root file");
  const base::FilePath obsolete_version =
      WriteRelative(current, "obsolete.dat", "old version file");
  base::ListValue actions;
  actions.Append(
      RegularAction("1.0.0.2/resource", "copy", "1.0.0.1/resource", "keep me"));
  auto manifest = PatchManifest(std::move(actions), "win");
  base::ListValue deletions;
  deletions.Append("obsolete.dll");
  deletions.Append("1.0.0.1/obsolete.dat");
  manifest.Set("deletions", std::move(deletions));
  const base::FilePath target = install.AppendASCII("1.0.0.2");
  auto result = XenonUpdateManager::PrepareVersionDirectory(
      current, resource, MakePatch(manifest), target,
      temp_dir_.GetPath().AppendASCII("staging"));
  ASSERT_TRUE(result.success) << result.error;
  std::string contents;
  ASSERT_TRUE(base::ReadFileToString(obsolete_root, &contents));
  EXPECT_EQ("old root file", contents);
  ASSERT_TRUE(base::ReadFileToString(obsolete_version, &contents));
  EXPECT_EQ("old version file", contents);
  EXPECT_FALSE(base::PathExists(target.AppendASCII("obsolete.dat")));
  ASSERT_TRUE(base::ReadFileToString(
      target.AppendASCII(".xenon-deletions.json"), &contents));
  auto recorded = base::JSONReader::Read(contents, base::JSON_PARSE_RFC);
  ASSERT_TRUE(recorded.has_value());
  ASSERT_TRUE(recorded->is_list());
  ASSERT_EQ(1u, recorded->GetList().size());
  EXPECT_EQ("obsolete.dll", recorded->GetList()[0].GetString());
}

#if BUILDFLAG(IS_WIN)
TEST_F(XenonUpdateManagerTest, WindowsCorruptDiffDownloadFallsBackToFullZip) {
  VerifyWindowsDiffFallback(/*corrupt_download=*/true);
}

TEST_F(XenonUpdateManagerTest, WindowsFailedDiffApplicationFallsBackToFullZip) {
  VerifyWindowsDiffFallback(/*corrupt_download=*/false);
}

// Opt-in release validation: all inputs remain read-only, and both staging and
// installation operate on a private copy. No process is launched and no browser
// registry entries are changed. Supply all three XENON_WIN_DIFF_* variables.
TEST_F(XenonUpdateManagerTest, WindowsRealPackageStagesAndInstallsExactTree) {
  registry_util::RegistryOverrideManager registry_override;
  ASSERT_NO_FATAL_FAILURE(registry_override.OverrideRegistry(HKEY_CURRENT_USER));
  ASSERT_NO_FATAL_FAILURE(registry_override.OverrideRegistry(HKEY_LOCAL_MACHINE));
  base::Environment environment;
  const auto old_value = environment.GetVar("XENON_WIN_DIFF_OLD_ROOT");
  const auto new_value = environment.GetVar("XENON_WIN_DIFF_NEW_ROOT");
  const auto patch_value = environment.GetVar("XENON_WIN_DIFF_PATCH");
  if (!old_value && !new_value && !patch_value) {
    GTEST_SKIP() << "Set XENON_WIN_DIFF_OLD_ROOT, XENON_WIN_DIFF_NEW_ROOT and "
                    "XENON_WIN_DIFF_PATCH to validate real release packages";
  }
  ASSERT_TRUE(old_value && new_value && patch_value)
      << "All three real-package paths are required";
  const base::FilePath old_root = base::FilePath::FromUTF8Unsafe(*old_value);
  const base::FilePath new_root = base::FilePath::FromUTF8Unsafe(*new_value);
  const base::FilePath patch_input =
      base::FilePath::FromUTF8Unsafe(*patch_value);
  ASSERT_TRUE(old_root.IsAbsolute() && new_root.IsAbsolute() &&
              patch_input.IsAbsolute());
  ASSERT_TRUE(base::DirectoryExists(old_root));
  ASSERT_TRUE(base::DirectoryExists(new_root));
  ASSERT_TRUE(base::PathExists(patch_input));

  FileHashes old_input, new_input, patch_hash;
  ASSERT_TRUE(SnapshotTree(old_root, &old_input));
  ASSERT_TRUE(SnapshotTree(new_root, &new_input));
  ASSERT_FALSE(old_input.empty());
  ASSERT_FALSE(new_input.empty());
  ASSERT_TRUE(AddFileHash(patch_input, patch_input.BaseName(), &patch_hash));
  const base::FilePath download = temp_dir_.GetPath().AppendASCII("download");
  ASSERT_TRUE(base::CreateDirectory(download));
  const base::FilePath patch = download.AppendASCII("patch.zip");
  ASSERT_TRUE(base::CopyFile(patch_input, patch));
  const base::FilePath inspection =
      temp_dir_.GetPath().AppendASCII("inspection");
  ASSERT_TRUE(zip::Unzip(patch, inspection));
  std::string json;
  ASSERT_TRUE(
      base::ReadFileToString(inspection.AppendASCII("manifest.json"), &json));
  const auto manifest = base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
  ASSERT_TRUE(manifest);
  const auto* base_version = manifest->FindString("base_version");
  const auto* target_version = manifest->FindString("target_version");
  ASSERT_TRUE(base_version && target_version);
  ASSERT_TRUE(base::Version(*base_version).IsValid());
  ASSERT_TRUE(base::Version(*target_version).IsValid());
  ASSERT_GT(base::Version(*target_version), base::Version(*base_version));
  ASSERT_TRUE(base::DirectoryExists(old_root.AppendASCII(*base_version)));
  ASSERT_TRUE(base::DirectoryExists(new_root.AppendASCII(*target_version)));

  const base::FilePath installation =
      temp_dir_.GetPath().AppendASCII("installed");
  ASSERT_TRUE(base::CopyDirectory(old_root, installation, true));
  const base::FilePath launcher = installation.AppendASCII("xlb153.exe");
  ASSERT_TRUE(base::PathExists(launcher));
  ASSERT_TRUE(base::PathExists(new_root.AppendASCII("xlb153.exe")));
  // Mini installers keep setup.exe beside Chrome-bin. The generator includes
  // it virtually; model the real installed location without changing inputs.
  const base::FilePath old_setup_relative =
      base::FilePath::FromASCII(*base_version)
          .AppendASCII("Installer")
          .AppendASCII("setup.exe");
  if (!base::PathExists(installation.Append(old_setup_relative))) {
    ASSERT_TRUE(base::CreateDirectory(
        installation.Append(old_setup_relative).DirName()));
    ASSERT_TRUE(base::CopyFile(old_root.DirName().AppendASCII("setup.exe"),
                               installation.Append(old_setup_relative)));
  }
  FileHashes expected = new_input;
  const base::FilePath setup_relative =
      base::FilePath::FromASCII(*target_version)
          .AppendASCII("Installer")
          .AppendASCII("setup.exe");
  if (!expected.contains(setup_relative)) {
    ASSERT_TRUE(AddFileHash(new_root.DirName().AppendASCII("setup.exe"),
                            setup_relative, &expected));
  }
  FileHashes before;
  ASSERT_TRUE(SnapshotTree(installation, &before));
  const base::ScopedPathOverride executable_override(
      base::FILE_EXE, launcher, /*is_absolute=*/true, /*create=*/false);
  const base::FilePath current = installation.AppendASCII(*base_version);
  const auto staged = XenonUpdateManager::PrepareVersionDirectory(
      current, current.AppendASCII("xlbrowser.dll"), patch,
      temp_dir_.GetPath().AppendASCII("versions").AppendASCII(*target_version),
      temp_dir_.GetPath().AppendASCII("staging"), {}, *base_version,
      *target_version);
  ASSERT_TRUE(staged.success) << staged.error;
  FileHashes after_staging;
  ASSERT_TRUE(SnapshotTree(installation, &after_staging));
  EXPECT_EQ(before, after_staging)
      << "Staging changed the running installation";
  ASSERT_TRUE(XenonUpdateInstaller::InstallAndRelaunch(
      staged.staged_dir, installation,
      /*relaunch_executable=*/base::FilePath()));

  EXPECT_FALSE(
      base::PathExists(installation.AppendASCII(".xenon-install.lock")));
  // Successful Windows transactions retain root-file backups until the old
  // process exits. Verify that precise recovery content before excluding it
  // from the release tree comparison; unknown internal files must still fail.
  base::FileEnumerator recoveries(installation, false,
                                  base::FileEnumerator::DIRECTORIES,
                                  FILE_PATH_LITERAL(".xenon-recovery-*"));
  const base::FilePath recovery = recoveries.Next();
  ASSERT_FALSE(recovery.empty());
  EXPECT_TRUE(recoveries.Next().empty());
  const base::FilePath previous_version =
      base::FilePath::FromASCII(*base_version);
  FileHashes expected_recovery;
  for (const auto& [relative, hash] : before) {
    if (!previous_version.IsParent(relative)) {
      expected_recovery.emplace(
          base::FilePath(FILE_PATH_LITERAL("backup")).Append(relative), hash);
    }
  }
  FileHashes recovery_files;
  ASSERT_TRUE(SnapshotTree(recovery, &recovery_files));
  EXPECT_EQ(expected_recovery, recovery_files);
  FileHashes installed;
  ASSERT_TRUE(SnapshotTree(installation, &installed));
  size_t retained = 0;
  for (auto it = installed.begin(); it != installed.end();) {
    if (previous_version.IsParent(it->first)) {
      const auto original = before.find(it->first);
      ASSERT_NE(before.end(), original);
      EXPECT_EQ(original->second, it->second) << it->first;
      ++retained;
      it = installed.erase(it);
    } else if (recovery.BaseName().IsParent(it->first)) {
      it = installed.erase(it);
    } else {
      ++it;
    }
  }
  size_t old_version_files = 0;
  for (const auto& [relative, hash] : before) {
    if (previous_version.IsParent(relative)) {
      ++old_version_files;
    }
  }
  EXPECT_EQ(old_version_files, retained);
  EXPECT_EQ(expected, installed)
      << "Installed files differ from the new release";
  FileHashes unchanged;
  ASSERT_TRUE(SnapshotTree(old_root, &unchanged));
  EXPECT_EQ(old_input, unchanged);
  ASSERT_TRUE(SnapshotTree(new_root, &unchanged));
  EXPECT_EQ(new_input, unchanged);
  unchanged.clear();
  ASSERT_TRUE(AddFileHash(patch_input, patch_input.BaseName(), &unchanged));
  EXPECT_EQ(patch_hash, unchanged);
  RecordProperty("verified_target_files", static_cast<int>(expected.size()));
  RecordProperty("preserved_previous_files", static_cast<int>(retained));
}
#endif

TEST_F(XenonUpdateManagerTest, InvalidFullZipDoesNotNotifyDownloaded) {
  const std::string invalid = "not a ZIP archive";
  UpdateManifest feed;
  feed.target_version = "1.0.0.2";
  feed.full_package =
      TestPackage("https://update.xenon.test/full.zip", invalid);
  test_url_loader_factory_.AddResponse(feed.full_package->url.spec(), invalid);
  BeginFeedDownload(feed);
  EXPECT_EQ(UpdateState::kError, manager_->GetState());
#if BUILDFLAG(IS_MAC)
  EXPECT_NE(std::string::npos, observer_.last_error_.find("require a .pkg"));
  EXPECT_EQ(1u, test_url_loader_factory_.total_requests());
  EXPECT_FALSE(manager_->GetStagedIsInstallerPackage());
#else
  EXPECT_NE(std::string::npos, observer_.last_error_.find("unpack"));
#endif
  EXPECT_EQ(0, observer_.downloaded_count_);
  EXPECT_FALSE(observer_.downloaded_manifest_.has_value());
  EXPECT_TRUE(manager_->GetStagedPath().empty());
#if !BUILDFLAG(IS_MAC)
  // Receiving all bytes is not equivalent to a prepared, installable update.
  ASSERT_TRUE(manager_->GetDownloadProgress().has_value());
  EXPECT_EQ(100, manager_->GetDownloadProgress()->percent);
  EXPECT_EQ(static_cast<int64_t>(invalid.size()),
            manager_->GetDownloadProgress()->bytes_downloaded);
  ASSERT_FALSE(observer_.phase_states_.empty());
  EXPECT_EQ(UpdateState::kPatching, observer_.phase_states_.back());
#endif
  manager_->CheckForUpdates("https://update.xenon.test/another-check");
  EXPECT_FALSE(manager_->GetDownloadProgress().has_value());
}

#if BUILDFLAG(IS_MAC)
TEST_F(XenonUpdateManagerTest, FullMacPkgPreservesVerifiedInstallerFile) {
  // The downloader verifies bytes; the platform installer validates and opens
  // the PKG later. No application ZIP extraction belongs to this code path.
  const std::string package = "opaque installer package bytes";
  UpdateManifest feed;
  feed.target_version = "1.0.0.2";
  feed.full_package =
      TestPackage("https://update.xenon.test/Release.PKG?download=1", package);
  test_url_loader_factory_.AddResponse(feed.full_package->url.spec(), package);
  BeginFeedDownload(feed);
  ASSERT_EQ(UpdateState::kUpdateDownloaded, manager_->GetState())
      << observer_.last_error_;
  EXPECT_EQ(1, observer_.downloaded_count_);
  ASSERT_TRUE(observer_.downloaded_manifest_.has_value());
  EXPECT_FALSE(observer_.downloaded_manifest_->is_diff);
  ASSERT_TRUE(manager_->GetDownloadProgress().has_value());
  EXPECT_EQ(100, manager_->GetDownloadProgress()->percent);
  EXPECT_EQ(static_cast<int64_t>(package.size()),
            manager_->GetDownloadProgress()->total_bytes);
  for (const auto state : observer_.phase_states_) {
    EXPECT_NE(UpdateState::kPatching, state);
  }
  EXPECT_TRUE(manager_->GetStagedIsInstallerPackage());
  EXPECT_TRUE(
      manager_->GetStagedPath().MatchesExtension(FILE_PATH_LITERAL(".pkg")));
  std::string contents;
  ASSERT_TRUE(base::ReadFileToString(manager_->GetStagedPath(), &contents));
  EXPECT_EQ(package, contents);
}

TEST_F(XenonUpdateManagerTest, MacFullZipIsRejectedBeforeDownload) {
  const std::string archive = MakeFullMacZip();
  UpdateManifest feed;
  feed.target_version = "1.0.0.2";
  feed.full_package =
      TestPackage("https://update.xenon.test/full.zip", archive);
  test_url_loader_factory_.AddResponse(feed.full_package->url.spec(), archive);
  BeginFeedDownload(feed);
  EXPECT_EQ(UpdateState::kError, manager_->GetState());
  EXPECT_NE(std::string::npos, observer_.last_error_.find("require a .pkg"));
  EXPECT_EQ(1u, test_url_loader_factory_.total_requests());
  EXPECT_EQ(0, observer_.downloaded_count_);
  EXPECT_TRUE(manager_->GetStagedPath().empty());
}

TEST_F(XenonUpdateManagerTest,
       FailedDiffStagesFullPkgWithSeparateDownloadFile) {
  base::ListValue actions;
  actions.Append(RegularAction("Contents/resource", "copy", "Contents/missing",
                               "not installed"));
  const base::FilePath diff_path = MakePatch(PatchManifest(std::move(actions)));
  std::string diff;
  ASSERT_TRUE(base::ReadFileToString(diff_path, &diff));
  const std::string full = "full installer package";
  UpdateManifest feed;
  feed.target_version = "1.0.0.2";
  feed.is_diff = true;
  feed.diff_package = TestPackage("https://update.xenon.test/diff.zip", diff);
  feed.full_package = TestPackage("https://update.xenon.test/full.pkg", full);
  test_url_loader_factory_.AddResponse(feed.diff_package->url.spec(), diff);
  BeginFeedDownload(feed);
  ASSERT_EQ(UpdateState::kDownloading, manager_->GetState())
      << observer_.last_error_;
  ASSERT_TRUE(manager_->GetManifest().has_value());
  EXPECT_FALSE(manager_->GetManifest()->is_diff);
  EXPECT_EQ(0, observer_.downloaded_count_);
  ASSERT_TRUE(manager_->GetDownloadProgress().has_value());
  EXPECT_EQ(0, manager_->GetDownloadProgress()->bytes_downloaded);
  EXPECT_EQ(0, manager_->GetDownloadProgress()->percent);
  EXPECT_EQ(static_cast<int64_t>(full.size()),
            manager_->GetDownloadProgress()->total_bytes);
  ASSERT_TRUE(
      test_url_loader_factory_.IsPending(feed.full_package->url.spec()));
  ASSERT_TRUE(test_url_loader_factory_.SimulateResponseForPendingRequest(
      feed.full_package->url.spec(), full));
  task_environment_.RunUntilIdle();
  ASSERT_EQ(UpdateState::kUpdateDownloaded, manager_->GetState())
      << observer_.last_error_;
  EXPECT_EQ(1, observer_.downloaded_count_);
  ASSERT_TRUE(observer_.downloaded_manifest_.has_value());
  EXPECT_FALSE(observer_.downloaded_manifest_->is_diff);
  EXPECT_TRUE(manager_->GetStagedIsInstallerPackage());
  EXPECT_EQ(3u, test_url_loader_factory_.total_requests());

  // The completed fallback must retain both independently named downloads;
  // replacing the patch path in-place can race cleanup of the failed attempt.
  const base::FilePath download_root = manager_->GetStagedPath().DirName();
  base::FileEnumerator archives(download_root, false,
                                base::FileEnumerator::FILES);
  std::vector<base::FilePath> paths;
  bool found_diff = false;
  bool found_full = false;
  for (base::FilePath file = archives.Next(); !file.empty();
       file = archives.Next()) {
    paths.push_back(file);
    std::string contents;
    ASSERT_TRUE(base::ReadFileToString(file, &contents));
    found_diff |= contents == diff;
    found_full |= contents == full;
  }
  ASSERT_EQ(2u, paths.size());
  EXPECT_NE(paths[0], paths[1]);
  EXPECT_TRUE(found_diff);
  EXPECT_TRUE(found_full);
}

TEST_F(XenonUpdateManagerTest, MacPkgHashMismatchNeverBecomesReady) {
  const std::string expected = "correct installer package";
  UpdateManifest feed;
  feed.target_version = "1.0.0.2";
  feed.full_package =
      TestPackage("https://update.xenon.test/full.pkg", expected);
  test_url_loader_factory_.AddResponse(feed.full_package->url.spec(),
                                       "changed installer package");
  BeginFeedDownload(feed);
  EXPECT_EQ(UpdateState::kError, manager_->GetState());
  EXPECT_NE(std::string::npos, observer_.last_error_.find("Hash mismatch"));
  EXPECT_EQ(0, observer_.downloaded_count_);
  EXPECT_FALSE(manager_->GetStagedIsInstallerPackage());
  EXPECT_TRUE(manager_->GetStagedPath().empty());
}

TEST_F(XenonUpdateManagerTest, MacCorruptDiffDownloadFallsBackToPkg) {
  const std::string full = "full installer package";
  UpdateManifest feed;
  feed.target_version = "1.0.0.2";
  feed.is_diff = true;
  feed.diff_package = TestPackage("https://update.xenon.test/diff.zip", "good");
  feed.full_package = TestPackage("https://update.xenon.test/full.pkg", full);
  test_url_loader_factory_.AddResponse(feed.diff_package->url.spec(), "evil");
  test_url_loader_factory_.AddResponse(feed.full_package->url.spec(), full);
  BeginFeedDownload(feed);
  ASSERT_EQ(UpdateState::kUpdateDownloaded, manager_->GetState())
      << observer_.last_error_;
  EXPECT_TRUE(manager_->GetStagedIsInstallerPackage());
  EXPECT_FALSE(manager_->GetManifest()->is_diff);
  EXPECT_EQ(1, observer_.downloaded_count_);
  EXPECT_EQ(3u, test_url_loader_factory_.total_requests());
}

TEST_F(XenonUpdateManagerTest, MacIneligibleDiffUsesPkgWithoutRequestingPatch) {
  // Platform tests cover real ownership and permissions. Simulate both
  // eligibility failures here to verify the manager's asynchronous routing.
  for (const char* reason : {"Application is owned by root",
                             "Installation parent is not writable"}) {
    SCOPED_TRACE(reason);
    const size_t requests_before = test_url_loader_factory_.total_requests();
    const std::string full = "full installer package";
    UpdateManifest feed;
    feed.target_version = "1.0.0.2";
    feed.is_diff = true;
    feed.diff_package =
        TestPackage("https://update.xenon.test/diff.zip", "diff");
    feed.full_package = TestPackage("https://update.xenon.test/full.pkg", full);
    manager_->SetDifferentialEligibilityForTesting(
        base::BindLambdaForTesting([this, reason](base::FilePath target) {
          EXPECT_EQ(temp_dir_.GetPath().AppendASCII("Installed.app"), target);
          return XenonUpdateManager::DifferentialEligibilityResult{
              false, false, std::string(reason)};
        }));
    test_url_loader_factory_.AddResponse(feed.full_package->url.spec(), full);
    BeginFeedDownload(feed);
    ASSERT_EQ(UpdateState::kUpdateDownloaded, manager_->GetState())
        << observer_.last_error_;
    EXPECT_TRUE(manager_->GetStagedIsInstallerPackage());
    EXPECT_FALSE(manager_->GetManifest()->is_diff);
    EXPECT_FALSE(
        test_url_loader_factory_.IsPending(feed.diff_package->url.spec()));
    EXPECT_EQ(requests_before + 2u, test_url_loader_factory_.total_requests());
    // A fresh manager is required because a ready update cannot be replaced.
    manager_->RemoveObserver(&observer_);
    manager_ = std::make_unique<XenonUpdateManager>();
    manager_->SetSystemUpdaterForTesting(nullptr);
    manager_->SetURLLoaderFactoryForTesting(shared_factory_);
    manager_->AddObserver(&observer_);
    test_url_loader_factory_.ClearResponses();
  }
}

TEST_F(XenonUpdateManagerTest, MacIneligibleDiffWithoutPkgReportsReason) {
  manager_->SetDifferentialEligibilityForTesting(
      base::BindLambdaForTesting([](base::FilePath) {
        return XenonUpdateManager::DifferentialEligibilityResult{
            false, false, "Application is owned by root"};
      }));
  UpdateManifest feed;
  feed.target_version = "1.0.0.2";
  feed.is_diff = true;
  feed.diff_package = TestPackage("https://update.xenon.test/diff.zip", "diff");
  BeginFeedDownload(feed);
  EXPECT_EQ(UpdateState::kError, manager_->GetState());
  EXPECT_NE(std::string::npos, observer_.last_error_.find("owned by root"));
  EXPECT_EQ(1u, test_url_loader_factory_.total_requests());
  EXPECT_EQ(0, observer_.downloaded_count_);
  EXPECT_FALSE(manager_->GetStagedIsInstallerPackage());
}

TEST_F(XenonUpdateManagerTest, MacLateDiffInstallFailureFallsBackToPkg) {
  // Reconstruction can succeed before the platform validates the staged app.
  // This fixture has real copied bytes but no valid bundle metadata/signature,
  // so native installation fails before copying or launching any helper.
  base::ListValue actions;
  actions.Append(RegularAction("Contents/MacOS/app", "copy",
                               "Contents/MacOS/app", "installed executable"));
  std::string diff;
  ASSERT_TRUE(base::ReadFileToString(
      MakePatch(PatchManifest(std::move(actions))), &diff));
  const std::string full = "xar!full installer package fixture";
  UpdateManifest feed;
  feed.target_version = "1.0.0.2";
  feed.is_diff = true;
  feed.diff_package = TestPackage("https://update.xenon.test/diff.zip", diff);
  feed.full_package = TestPackage("https://update.xenon.test/full.pkg", full);
  test_url_loader_factory_.AddResponse(feed.diff_package->url.spec(), diff);
  BeginFeedDownload(feed);
  ASSERT_EQ(UpdateState::kUpdateDownloaded, manager_->GetState())
      << observer_.last_error_;
  ASSERT_EQ(1, observer_.downloaded_count_);
  EXPECT_TRUE(manager_->GetManifest()->is_diff);
  EXPECT_FALSE(manager_->GetStagedIsInstallerPackage());
  EXPECT_EQ(2u, test_url_loader_factory_.total_requests());
#if defined(XENON_UPDATER_STANDALONE_TESTS)
  const int exits_before = GetUpdateExitRequestsForTesting();
#endif

  manager_->QuitAndInstall();
  task_environment_.RunUntilIdle();
  ASSERT_EQ(UpdateState::kDownloading, manager_->GetState())
      << manager_->GetLastError();
  EXPECT_FALSE(manager_->GetManifest()->is_diff);
  EXPECT_TRUE(manager_->GetStagedPath().empty());
  ASSERT_TRUE(
      test_url_loader_factory_.IsPending(feed.full_package->url.spec()));
  const base::FilePath journal =
      temp_dir_.GetPath().AppendASCII("Installed.app.xenon-update-result.json");
  std::string journal_json;
  ASSERT_TRUE(base::ReadFileToString(journal, &journal_json));
  const auto result =
      base::JSONReader::ReadDict(journal_json, base::JSON_PARSE_RFC);
  ASSERT_TRUE(result);
  ASSERT_TRUE(result->FindString("status"));
  EXPECT_EQ("validation_failed", *result->FindString("status"));

  ASSERT_TRUE(test_url_loader_factory_.SimulateResponseForPendingRequest(
      feed.full_package->url.spec(), full));
  task_environment_.RunUntilIdle();
  ASSERT_EQ(UpdateState::kUpdateDownloaded, manager_->GetState());
  EXPECT_TRUE(manager_->GetStagedIsInstallerPackage());
  EXPECT_FALSE(manager_->GetManifest()->is_diff);
  EXPECT_FALSE(manager_->GetInstallerOpened());
  EXPECT_TRUE(manager_->GetLastError().empty());
  EXPECT_EQ(2, observer_.downloaded_count_);
  EXPECT_EQ(0, observer_.installer_opened_count_);
  EXPECT_EQ(3u, test_url_loader_factory_.total_requests());
  std::string contents;
  ASSERT_TRUE(base::ReadFileToString(manager_->GetStagedPath(), &contents));
  EXPECT_EQ(full, contents);
#if defined(XENON_UPDATER_STANDALONE_TESTS)
  EXPECT_EQ(exits_before, GetUpdateExitRequestsForTesting());
#endif
}

TEST_F(XenonUpdateManagerTest,
       ElevationApprovalPreservesDiffUntilExplicitRetry) {
  StartReadyPrivilegedDiff();
  ASSERT_EQ(UpdateState::kUpdateDownloaded, manager_->GetState());
  ASSERT_TRUE(manager_->GetRequiresElevation());
  const base::FilePath staged = manager_->GetStagedPath();
  int attempts = 0;
  manager_->SetPrivilegedInstallerForTesting(base::BindLambdaForTesting(
      [&](base::FilePath path, base::FilePath target, base::FilePath executable,
          base::OnceCallback<void(bool, bool, std::string)> completion) {
        EXPECT_EQ(staged, path);
        ++attempts;
        std::move(completion).Run(attempts == 2, attempts == 1, "");
      }));
#if defined(XENON_UPDATER_STANDALONE_TESTS)
  const int exits = GetUpdateExitRequestsForTesting();
#endif
  manager_->InstallOnExit();
  task_environment_.RunUntilIdle();
  EXPECT_EQ(0, attempts);
  manager_->QuitAndInstall();
  manager_->QuitAndInstall();
  task_environment_.RunUntilIdle();
  EXPECT_EQ(1, attempts);
  EXPECT_EQ(1, observer_.elevation_approval_count_);
  EXPECT_TRUE(manager_->GetElevationApprovalRequired());
  EXPECT_TRUE(manager_->GetLastError().empty());
  EXPECT_EQ(UpdateState::kUpdateDownloaded, manager_->GetState());
  EXPECT_EQ(staged, manager_->GetStagedPath());
  EXPECT_TRUE(manager_->GetManifest()->is_diff);
  EXPECT_EQ(2u, test_url_loader_factory_.total_requests());
#if defined(XENON_UPDATER_STANDALONE_TESTS)
  EXPECT_EQ(exits, GetUpdateExitRequestsForTesting());
#endif
  manager_->QuitAndInstall();
  task_environment_.RunUntilIdle();
  EXPECT_EQ(2, attempts);
  EXPECT_FALSE(manager_->GetElevationApprovalRequired());
#if defined(XENON_UPDATER_STANDALONE_TESTS)
  EXPECT_EQ(exits + 1, GetUpdateExitRequestsForTesting());
#endif
}

TEST_F(XenonUpdateManagerTest, AwaitingApprovalCanExplicitlyUseFullPkg) {
  StartReadyPrivilegedDiff();
  ASSERT_EQ(UpdateState::kUpdateDownloaded, manager_->GetState());
  manager_->DownloadFullInstaller();
  EXPECT_TRUE(manager_->GetManifest()->is_diff);
  manager_->SetPrivilegedInstallerForTesting(base::BindLambdaForTesting(
      [](base::FilePath, base::FilePath, base::FilePath,
         base::OnceCallback<void(bool, bool, std::string)> completion) {
        std::move(completion).Run(false, true, "");
      }));
  manager_->QuitAndInstall();
  task_environment_.RunUntilIdle();
  ASSERT_TRUE(manager_->GetElevationApprovalRequired());
  manager_->DownloadFullInstaller();
  manager_->DownloadFullInstaller();
  task_environment_.RunUntilIdle();
  EXPECT_EQ(UpdateState::kUpdateDownloaded, manager_->GetState());
  EXPECT_TRUE(manager_->GetStagedIsInstallerPackage());
  EXPECT_FALSE(manager_->GetRequiresElevation());
  EXPECT_FALSE(manager_->GetElevationApprovalRequired());
  EXPECT_FALSE(manager_->GetManifest()->is_diff);
  EXPECT_EQ(3u, test_url_loader_factory_.total_requests());
}

TEST_F(XenonUpdateManagerTest, PrivilegedPreparationFailureUsesFullPkg) {
  StartReadyPrivilegedDiff();
  ASSERT_EQ(UpdateState::kUpdateDownloaded, manager_->GetState());
  manager_->SetPrivilegedInstallerForTesting(base::BindLambdaForTesting(
      [](base::FilePath, base::FilePath, base::FilePath,
         base::OnceCallback<void(bool, bool, std::string)> completion) {
        std::move(completion)
            .Run(false, false, "Privileged service unavailable");
      }));
#if defined(XENON_UPDATER_STANDALONE_TESTS)
  const int exits = GetUpdateExitRequestsForTesting();
#endif
  manager_->QuitAndInstall();
  task_environment_.RunUntilIdle();
  EXPECT_EQ(UpdateState::kUpdateDownloaded, manager_->GetState());
  EXPECT_TRUE(manager_->GetStagedIsInstallerPackage());
  EXPECT_FALSE(manager_->GetRequiresElevation());
  EXPECT_FALSE(manager_->GetElevationApprovalRequired());
  EXPECT_EQ(3u, test_url_loader_factory_.total_requests());
#if defined(XENON_UPDATER_STANDALONE_TESTS)
  EXPECT_EQ(exits, GetUpdateExitRequestsForTesting());
#endif
}

TEST_F(XenonUpdateManagerTest, MacPkgHandoffIsRetryableAndNeverQuitsBrowser) {
  const std::string package = "xar!installer package fixture";
  UpdateManifest feed;
  feed.target_version = "1.0.0.2";
  feed.full_package =
      TestPackage("https://update.xenon.test/full.pkg", package);
  test_url_loader_factory_.AddResponse(feed.full_package->url.spec(), package);
  const base::FilePath cache = temp_dir_.GetPath().AppendASCII("package-cache");
  manager_->SetInstallerPackageCacheRootForTesting(cache);
  std::vector<base::FilePath> opened_paths;
  base::OnceCallback<void(bool, std::string)> complete_open;
  manager_->SetInstallerPackageOpenerForTesting(base::BindLambdaForTesting(
      [&](base::FilePath path,
          base::OnceCallback<void(bool, std::string)> completion) {
        EXPECT_TRUE(cache.IsParent(path));
        opened_paths.push_back(path);
        complete_open = std::move(completion);
      }));
  BeginFeedDownload(feed);
  ASSERT_EQ(UpdateState::kUpdateDownloaded, manager_->GetState());
  const base::FilePath temporary_package = manager_->GetStagedPath();
#if defined(XENON_UPDATER_STANDALONE_TESTS)
  const int exits_before = GetUpdateExitRequestsForTesting();
#endif
  manager_->InstallOnExit();
  task_environment_.RunUntilIdle();
  EXPECT_TRUE(opened_paths.empty());
  EXPECT_FALSE(base::PathExists(cache));

  manager_->QuitAndInstall();
  manager_->QuitAndInstall();
  task_environment_.RunUntilIdle();
  ASSERT_EQ(1u, opened_paths.size());
  EXPECT_NE(temporary_package, opened_paths.back());
  std::string contents;
  ASSERT_TRUE(base::ReadFileToString(opened_paths.back(), &contents));
  EXPECT_EQ(package, contents);
  ASSERT_TRUE(complete_open);
  std::move(complete_open).Run(false, "Installer could not be opened");
  EXPECT_EQ(UpdateState::kUpdateDownloaded, manager_->GetState());
  EXPECT_EQ("Installer could not be opened", manager_->GetLastError());
  EXPECT_FALSE(manager_->GetInstallerOpened());

  manager_->QuitAndInstall();
  task_environment_.RunUntilIdle();
  ASSERT_EQ(2u, opened_paths.size());
  ASSERT_TRUE(complete_open);
  std::move(complete_open).Run(true, "");
  EXPECT_EQ(UpdateState::kUpdateDownloaded, manager_->GetState());
  EXPECT_TRUE(manager_->GetInstallerOpened());
  EXPECT_TRUE(manager_->GetStagedIsInstallerPackage());
  EXPECT_TRUE(manager_->GetLastError().empty());
  EXPECT_EQ(1, observer_.installer_opened_count_);
  manager_->InstallOnExit();
  task_environment_.RunUntilIdle();
  EXPECT_EQ(2u, opened_paths.size());
#if defined(XENON_UPDATER_STANDALONE_TESTS)
  EXPECT_EQ(exits_before, GetUpdateExitRequestsForTesting());
#endif
  // Handoff is not installation success, and the verified package survives
  // browser shutdown so Installer can consume it after its must-close prompt.
  manager_->RemoveObserver(&observer_);
  manager_.reset();
  EXPECT_TRUE(base::PathExists(opened_paths.back()));
  manager_ = std::make_unique<XenonUpdateManager>();
  manager_->AddObserver(&observer_);
}
#endif

TEST_F(XenonUpdateManagerTest, CheckWhileCheckingPreservesOriginalRequest) {
  manager_->SetCurrentVersionForTesting("1.0.0.1");
  const std::string first = "https://update.xenon.test/first";
  manager_->CheckForUpdates(first);
  manager_->CheckForUpdates("https://update.xenon.test/second");
  task_environment_.RunUntilIdle();
  EXPECT_EQ(UpdateState::kCheckingForUpdate, manager_->GetState());
  EXPECT_EQ(first, manager_->GetFeedURL());
  EXPECT_EQ(1, observer_.checking_count_);
  EXPECT_EQ(1u, test_url_loader_factory_.total_requests());
}

TEST_F(XenonUpdateManagerTest, QuitAndInstallWhileCheckingDoesNotInterrupt) {
  manager_->SetCurrentVersionForTesting("1.0.0.1");
  manager_->CheckForUpdates("https://update.xenon.test/check");
  manager_->QuitAndInstall();
  manager_->DownloadUpdate();
  task_environment_.RunUntilIdle();
  EXPECT_EQ(UpdateState::kCheckingForUpdate, manager_->GetState());
  EXPECT_TRUE(observer_.last_error_.empty());
  EXPECT_EQ(1u, test_url_loader_factory_.total_requests());
}

TEST_F(XenonUpdateManagerTest, DiffResolvesBasePathsWithoutBlockingMainThread) {
  const base::FilePath install =
      temp_dir_.GetPath().AppendASCII("Installed.app");
#if BUILDFLAG(IS_MAC)
  const std::string source_relative = "Contents/resource";
  const std::string target_relative = source_relative;
#else
  const std::string source_relative = "1.0.0.1/resource";
  const std::string target_relative = "1.0.0.2/resource";
#endif
  WriteRelative(install, source_relative, "keep me");
  manager_->SetInstallDirForTesting(install);
  manager_->SetCurrentVersionForTesting("1.0.0.1");
  manager_->SetAutoDownload(true);
  // Deliberately do not set a base-file override. Production resolves the
  // module against the installed paths, which must happen on a MayBlock worker.
  base::ListValue actions;
  actions.Append(
      RegularAction(target_relative, "copy", source_relative, "keep me"));
  const base::FilePath archive =
      MakePatch(PatchManifest(std::move(actions), TestPlatform()));
  std::string archive_contents;
  ASSERT_TRUE(base::ReadFileToString(archive, &archive_contents));
  UpdateManifest feed;
  feed.target_version = "1.0.0.2";
  feed.is_diff = true;
  feed.diff_package =
      TestPackage("https://update.xenon.test/patch.zip", archive_contents);
  std::string response;
  ASSERT_TRUE(base::JSONWriter::Write(feed.ToValue(), &response));
  const std::string url = "https://update.xenon.test/check?platform=" +
                          std::string(TestPlatform()) + "&version=1.0.0.1";
  test_url_loader_factory_.AddResponse(url, response);
  test_url_loader_factory_.AddResponse(feed.diff_package->url.spec(),
                                       archive_contents);
  {
    // Match the browser UI thread's restrictions throughout asynchronous
    // download completion and staging, while ThreadPool tasks may block.
    base::ScopedDisallowBlocking disallow_blocking;
    manager_->CheckForUpdates(url);
    task_environment_.RunUntilIdle();
  }
  ASSERT_EQ(UpdateState::kUpdateDownloaded, manager_->GetState())
      << observer_.last_error_;
  ASSERT_EQ(1, observer_.downloaded_count_);
  ASSERT_TRUE(observer_.downloaded_manifest_.has_value());
  EXPECT_TRUE(observer_.downloaded_manifest_->is_diff);
  std::string contents;
#if BUILDFLAG(IS_MAC)
  ASSERT_TRUE(base::ReadFileToString(
      manager_->GetStagedPath().AppendASCII("Contents").AppendASCII("resource"),
      &contents));
#else
  ASSERT_TRUE(base::ReadFileToString(
      manager_->GetStagedPath().AppendASCII("resource"), &contents));
#endif
  EXPECT_EQ("keep me", contents);
}

TEST_F(XenonUpdateManagerTest,
       ReentrantDownloadDuringFirstTempCreationStagesOnce) {
  const base::FilePath install =
      temp_dir_.GetPath().AppendASCII("Installed.app");
#if BUILDFLAG(IS_MAC)
  const std::string source_relative = "Contents/resource";
  const std::string target_relative = source_relative;
#else
  const std::string source_relative = "1.0.0.1/resource";
  const std::string target_relative = "1.0.0.2/resource";
#endif
  const base::FilePath resource =
      WriteRelative(install, source_relative, "keep me");
  manager_->SetInstallDirForTesting(install);
  manager_->SetCurrentVersionForTesting("1.0.0.1");
  manager_->SetBaseFileForTesting(resource);
  manager_->SetAutoDownload(true);
  base::ListValue actions;
  actions.Append(
      RegularAction(target_relative, "copy", source_relative, "keep me"));
  const base::FilePath archive =
      MakePatch(PatchManifest(std::move(actions), TestPlatform()));
  std::string archive_contents;
  ASSERT_TRUE(base::ReadFileToString(archive, &archive_contents));
  UpdateManifest feed;
  feed.target_version = "1.0.0.2";
  feed.is_diff = true;
  feed.diff_package = UpdatePackageInfo{
      GURL("https://update.xenon.test/patch.zip"),
      static_cast<int64_t>(archive_contents.size()), Sha256(archive_contents)};
  std::string response;
  ASSERT_TRUE(base::JSONWriter::Write(feed.ToValue(), &response));
  const std::string url = "https://update.xenon.test/check?platform=" +
                          std::string(TestPlatform()) + "&version=1.0.0.1";
  test_url_loader_factory_.AddResponse(url, response);

  class ReentrantObserver : public XenonUpdateManager::Observer {
   public:
    explicit ReentrantObserver(XenonUpdateManager* manager)
        : manager_(*manager) {}
    void OnCheckingForUpdate() override {
      manager_->CheckForUpdates("https://update.xenon.test/reentrant-check");
    }
    void OnUpdateAvailable(const UpdateManifest&) override {
      in_available_ = true;
      manager_->DownloadUpdate();
      manager_->DownloadUpdate();
      EXPECT_EQ(0, phase_count_);
      in_available_ = false;
    }
    void OnUpdateStateChanged(UpdateState state) override {
      EXPECT_FALSE(in_available_);
      EXPECT_EQ(manager_->GetState(), state);
      ++phase_count_;
    }

    int phase_count_ = 0;

   private:
    bool in_available_ = false;
    raw_ref<XenonUpdateManager> manager_;
  } reentrant(manager_.get());
  manager_->AddObserver(&reentrant);
  manager_->CheckForUpdates(url);
  task_environment_.RunUntilIdle();
  EXPECT_GT(reentrant.phase_count_, 0);
  manager_->RemoveObserver(&reentrant);
  ASSERT_EQ(UpdateState::kDownloading, manager_->GetState());
  manager_->QuitAndInstall();
  EXPECT_EQ(UpdateState::kDownloading, manager_->GetState());
  EXPECT_TRUE(observer_.last_error_.empty());
  ASSERT_TRUE(
      test_url_loader_factory_.IsPending(feed.diff_package->url.spec()));
  EXPECT_EQ(2u, test_url_loader_factory_.total_requests());
  EXPECT_EQ(1, observer_.checking_count_);

  // A second check during download must not replace the active manifest or
  // create another operation's temporary directory.
  manager_->CheckForUpdates("https://update.xenon.test/different-feed");
  manager_->DownloadUpdate();
  EXPECT_EQ(url, manager_->GetFeedURL());
  ASSERT_TRUE(test_url_loader_factory_.SimulateResponseForPendingRequest(
      feed.diff_package->url.spec(), archive_contents));
  task_environment_.RunUntilIdle();
  ASSERT_EQ(UpdateState::kUpdateDownloaded, manager_->GetState())
      << observer_.last_error_;
  ASSERT_TRUE(observer_.downloaded_manifest_.has_value());
  EXPECT_EQ("1.0.0.2", observer_.downloaded_manifest_->target_version);
  EXPECT_EQ(2u, test_url_loader_factory_.total_requests());
  const base::FilePath staged = manager_->GetStagedPath();
  EXPECT_TRUE(base::DirectoryExists(staged));
  std::string contents;
#if BUILDFLAG(IS_MAC)
  ASSERT_TRUE(base::ReadFileToString(
      staged.AppendASCII("Contents").AppendASCII("resource"), &contents));
#else
  ASSERT_TRUE(
      base::ReadFileToString(staged.AppendASCII("resource"), &contents));
#endif
  EXPECT_EQ("keep me", contents);
  manager_->CheckForUpdates("https://update.xenon.test/after-staging");
  EXPECT_EQ(staged, manager_->GetStagedPath());
  EXPECT_EQ(UpdateState::kUpdateDownloaded, manager_->GetState());
  EXPECT_EQ(url, manager_->GetFeedURL());
}

TEST_F(XenonUpdateManagerTest, DeferredPhaseDoesNotArriveAfterNewAttemptFails) {
  // Complete temp-directory creation once. The next attempt can then fail
  // synchronously while its downloading phase notifications are still queued.
  const std::string invalid = "not a ZIP archive";
  UpdateManifest feed;
  feed.target_version = "1.0.0.2";
  feed.full_package =
      TestPackage("https://update.xenon.test/full.zip", invalid);
  test_url_loader_factory_.AddResponse(feed.full_package->url.spec(), invalid);
  BeginFeedDownload(feed);
  ASSERT_EQ(UpdateState::kError, manager_->GetState());
  observer_.phase_states_.clear();
  feed.full_package =
      TestPackage("https://update.xenon.test/full.bin", invalid);
  BeginFeedDownload(feed);
  ASSERT_EQ(UpdateState::kError, manager_->GetState());
#if BUILDFLAG(IS_MAC)
  EXPECT_NE(std::string::npos, observer_.last_error_.find("require a .pkg"));
#else
  EXPECT_EQ("Unsupported update package type", observer_.last_error_);
#endif
  EXPECT_TRUE(observer_.phase_states_.empty());
}

TEST_F(XenonUpdateManagerTest, ErrorSnapshotSurvivesObserverAttachment) {
  manager_->QuitAndInstall();
  EXPECT_EQ(UpdateState::kError, manager_->GetState());
  EXPECT_EQ("No update ready for installation", manager_->GetLastError());
  manager_->CheckForUpdates("https://update.xenon.test/retry");
  EXPECT_TRUE(manager_->GetLastError().empty());
}

#if BUILDFLAG(IS_MAC)
TEST_F(XenonUpdateManagerTest, StartupRecoversRollbackErrorForLaterPageLoad) {
  const base::FilePath install =
      temp_dir_.GetPath().AppendASCII("Installed.app");
  ASSERT_TRUE(base::CreateDirectory(install));
  manager_->SetInstallDirForTesting(install);
  manager_->SetCurrentVersionForTesting("1.0.0.1");
  base::DictValue result;
  result.Set("schema_version", 1);
  // Foundation preserves the platform's /var spelling when resolving this
  // standard temporary directory, unlike POSIX realpath's /private/var.
  result.Set("target", install.value());
  result.Set("target_version", "1.0.0.2");
  result.Set("status", "relaunch_failed_rolled_back");
  result.Set("message", "New version was blocked; previous version restored");
  result.Set("retained_path", "");
  result.Set("timestamp", 1.0);
  std::string json;
  ASSERT_TRUE(base::JSONWriter::Write(result, &json));
  const base::FilePath journal(install.value() + ".xenon-update-result.json");
  ASSERT_TRUE(base::WriteFile(journal, json));
  ASSERT_TRUE(base::SetPosixFilePermissions(journal, 0600));
  manager_->OnVersionStartup();
  task_environment_.RunUntilIdle();
  EXPECT_EQ(UpdateState::kError, manager_->GetState());
  EXPECT_NE(std::string::npos, manager_->GetLastError().find("1.0.0.2"));
  EXPECT_NE(std::string::npos,
            manager_->GetLastError().find("previous version restored"));
  // A later successful PKG install of the target version supersedes this
  // rollback record even though the helper never wrote its success record.
  EXPECT_TRUE(
      XenonUpdateInstaller::ReadLastInstallError(install, "1.0.0.2").empty());

  // A launch awaiting user approval is not success while the old version is
  // running. Opening the target version later completes the recorded attempt.
  result.Set("status", "awaiting_user_approval");
  result.Set("message", "Check macOS security settings and try opening again");
  ASSERT_TRUE(base::JSONWriter::Write(result, &json));
  ASSERT_TRUE(base::WriteFile(journal, json));
  ASSERT_TRUE(base::SetPosixFilePermissions(journal, 0600));
  EXPECT_FALSE(
      XenonUpdateInstaller::ReadLastInstallError(install, "1.0.0.1").empty());
  EXPECT_TRUE(
      XenonUpdateInstaller::ReadLastInstallError(install, "1.0.0.2").empty());
  ASSERT_TRUE(base::ReadFileToString(journal, &json));
  auto confirmed = base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
  ASSERT_TRUE(confirmed);
  ASSERT_TRUE(confirmed->FindString("status"));
  EXPECT_EQ("success", *confirmed->FindString("status"));
}
#endif

namespace {
class FakeSystemUpdater : public XenonSystemUpdater {
 public:
  std::string GetFeedURL() const override {
    return "https://updates.xenon.test/service/update2/json";
  }
  void Start(bool check_only, Observer observer) override {
    checks.push_back(check_only);
    observer_ = std::move(observer);
  }
  void Emit(Event::Phase phase,
            std::string version = "1.0.0.2",
            std::string error = "") {
    Event event;
    event.phase = phase;
    event.version = std::move(version);
    event.error = std::move(error);
    event.downloaded = 25;
    event.total = 100;
    observer_.Run(event);
  }
  std::vector<bool> checks;

 private:
  Observer observer_;
};
}  // namespace

TEST_F(XenonUpdateManagerTest, SystemUpdaterOwnsFeedAndDoesNotUseLegacyHTTP) {
  auto backend = std::make_unique<FakeSystemUpdater>();
  auto* fake = backend.get();
  manager_->SetSystemUpdaterForTesting(std::move(backend));
  manager_->SetFeedURL("http://untrusted.test/update");
  EXPECT_EQ(manager_->GetFeedURL(), fake->GetFeedURL());
  manager_->CheckForUpdates("http://untrusted.test/update");
  EXPECT_EQ(manager_->GetState(), UpdateState::kError);
  EXPECT_TRUE(fake->checks.empty());
  manager_->CheckForUpdates();
  ASSERT_EQ(fake->checks.size(), 1u);
  EXPECT_TRUE(fake->checks[0]);
  EXPECT_EQ(test_url_loader_factory_.NumPending(), 0);
}

TEST_F(XenonUpdateManagerTest,
       SystemUpdaterWaitsForExitWithoutReportingInstalled) {
  auto backend = std::make_unique<FakeSystemUpdater>();
  auto* fake = backend.get();
  manager_->SetSystemUpdaterForTesting(std::move(backend));
  manager_->SetAutoDownload(true);
  manager_->CheckForUpdates();
  ASSERT_EQ(fake->checks.size(), 1u);
  EXPECT_FALSE(fake->checks[0]);
  fake->Emit(XenonSystemUpdater::Event::Phase::kAvailable);
  manager_->CheckForUpdates();
  EXPECT_EQ(manager_->GetState(), UpdateState::kUpdateAvailable);
  manager_->DownloadUpdate();
  EXPECT_EQ(fake->checks.size(), 1u);
  fake->Emit(XenonSystemUpdater::Event::Phase::kDownloading);
  ASSERT_TRUE(manager_->GetDownloadProgress());
  EXPECT_EQ(manager_->GetDownloadProgress()->percent, 25);
  fake->Emit(XenonSystemUpdater::Event::Phase::kPreparing);
  EXPECT_EQ(manager_->GetState(), UpdateState::kPatching);
  fake->Emit(XenonSystemUpdater::Event::Phase::kAwaitingExit);
  EXPECT_EQ(manager_->GetState(), UpdateState::kUpdateDownloaded);
  EXPECT_FALSE(manager_->SystemUpdateInstalled());
  EXPECT_TRUE(manager_->GetStagedPath().empty());
  EXPECT_FALSE(manager_->GetRequiresElevation());
  EXPECT_FALSE(manager_->GetStagedIsInstallerPackage());
  manager_->InstallOnExit();
  EXPECT_EQ(fake->checks.size(), 1u);
  fake->Emit(XenonSystemUpdater::Event::Phase::kError, "",
             "Installer was blocked");
  EXPECT_EQ(manager_->GetState(), UpdateState::kError);
  EXPECT_FALSE(manager_->SystemUpdateInstalled());
}

TEST_F(XenonUpdateManagerTest, SystemUpdaterExplicitDownloadAfterQuery) {
  auto backend = std::make_unique<FakeSystemUpdater>();
  auto* fake = backend.get();
  manager_->SetSystemUpdaterForTesting(std::move(backend));
  manager_->CheckForUpdates();
  fake->Emit(XenonSystemUpdater::Event::Phase::kAvailable);
  manager_->DownloadUpdate();
  ASSERT_EQ(fake->checks.size(), 2u);
  EXPECT_TRUE(fake->checks[0]);
  EXPECT_FALSE(fake->checks[1]);
  fake->Emit(XenonSystemUpdater::Event::Phase::kInstalled);
  EXPECT_TRUE(manager_->SystemUpdateInstalled());
}

TEST_F(XenonUpdateManagerTest, SystemUpdaterNoUpdateAndFailureCanRetry) {
  auto backend = std::make_unique<FakeSystemUpdater>();
  auto* fake = backend.get();
  manager_->SetSystemUpdaterForTesting(std::move(backend));
  manager_->CheckForUpdates();
  fake->Emit(XenonSystemUpdater::Event::Phase::kNoUpdate, "");
  EXPECT_EQ(manager_->GetState(), UpdateState::kUpdateNotAvailable);
  manager_->CheckForUpdates();
  fake->Emit(XenonSystemUpdater::Event::Phase::kError, "",
             "service unavailable");
  EXPECT_EQ(manager_->GetLastError(), "service unavailable");
  manager_->CheckForUpdates();
  EXPECT_EQ(fake->checks.size(), 3u);
  EXPECT_TRUE(manager_->GetLastError().empty());
}

}  // namespace xenon::updater
