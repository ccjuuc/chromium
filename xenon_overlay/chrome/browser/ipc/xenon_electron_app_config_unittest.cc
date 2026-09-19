// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_electron_app_config.h"

#include <map>
#include <string>
#include <string_view>
#include <utility>

#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/json/json_writer.h"
#include "base/values.h"
#include "net/base/filename_util.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"
#include "xenon_overlay/common/asar/archive.h"
#include "xenon_overlay/common/asar/test_support.h"

namespace xenon::ipc {
namespace {

class ElectronAppConfigTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(temp_.CreateUniqueTempDir());
    ASSERT_TRUE(base::NormalizeFilePath(temp_.GetPath(), &root_));
    descriptors_ = root_.AppendASCII("host-configs");
    ASSERT_TRUE(base::CreateDirectory(descriptors_));
    descriptor_ = descriptors_.AppendASCII("application.json");
  }

  void TearDown() override {
    asar::RemoveArchivePublicKeys("app-config-test-existing");
    asar::RemoveArchivePublicKeys("app-config-test-registered");
  }

  bool WriteApplication(const base::FilePath& app) {
    return base::CreateDirectory(app.AppendASCII("dist")) &&
           base::WriteFile(
               app.AppendASCII("package.json"),
               R"({"name":"original","version":"2.5","main":"missing.js"})") &&
           base::WriteFile(app.AppendASCII("dist").AppendASCII("entry.js"),
                           "module.exports = 42;\n") &&
           base::WriteFile(app.AppendASCII("untouched.bin"),
                           std::string("\0\1\xfforiginal\0", 13));
  }

  bool WriteDescriptor(base::DictValue descriptor) {
    auto text = base::WriteJson(descriptor);
    return text && base::WriteFile(descriptor_, *text);
  }

  std::map<base::FilePath, std::string> Snapshot(
      const base::FilePath& directory) {
    std::map<base::FilePath, std::string> files;
    base::FileEnumerator entries(directory, true, base::FileEnumerator::FILES);
    for (auto path = entries.Next(); !path.empty(); path = entries.Next()) {
      std::string bytes;
      EXPECT_TRUE(base::ReadFileToString(path, &bytes));
      files.emplace(path, std::move(bytes));
    }
    return files;
  }

  base::ScopedTempDir temp_;
  base::FilePath root_;
  base::FilePath descriptors_;
  base::FilePath descriptor_;
};

TEST_F(ElectronAppConfigTest,
       DirectSourceDirectoryKeepsApplicationFilesUntouched) {
  const auto app = root_.AppendASCII("source");
  ASSERT_TRUE(WriteApplication(app));
  const auto before = Snapshot(app);
  mojom::IpcMainConfigPtr config;
  std::string error;
  ASSERT_TRUE(LoadElectronAppConfig(app, "source-container", &config, &error))
      << error;
  ASSERT_TRUE(config);
  EXPECT_EQ("source-container", config->container_id);
  EXPECT_EQ(app.AsUTF8Unsafe(), config->app_path);
  EXPECT_EQ("original", config->app_name);
  EXPECT_EQ("2.5", config->app_version);
  EXPECT_EQ(false, config->is_packaged);
  EXPECT_TRUE(config->embedded_main_source.empty());
  EXPECT_TRUE(config->virtual_main_path.empty());
  EXPECT_TRUE(config->renderer_url_mappings.empty());
  EXPECT_TRUE(config->archive_public_keys.empty());
  EXPECT_TRUE(
      base::FilePath::FromUTF8Unsafe(config->executable_path).IsAbsolute());
  EXPECT_EQ(before, Snapshot(app));
}

TEST_F(ElectronAppConfigTest,
       ExternalDescriptorPreservesReleaseAndWindowPairing) {
  const auto release = root_.AppendASCII("release");
  const auto resources = release.AppendASCII("resources");
  const auto app = resources.AppendASCII("app");
  ASSERT_TRUE(WriteApplication(app));
  const auto executable = release.AppendASCII("original.bin");
  ASSERT_TRUE(base::WriteFile(executable, "original executable identity"));
  const auto before = Snapshot(release);
  base::ListValue pages;
  pages.Append("dist/clipper.html");
  pages.Append("dist/gif clipper.html");
  ASSERT_TRUE(
      WriteDescriptor(base::DictValue()
                          .Set("application", "../release")
                          .Set("executable", "../release/original.bin")
                          .Set("main", "dist/entry.js")
                          .Set("name", "Hosted name")
                          .Set("version", "3.2")
                          .Set("versionFromExecutable", false)
                          .Set("parentWindowPairing", std::move(pages))));
  mojom::IpcMainConfigPtr config;
  std::string error;
  ASSERT_TRUE(LoadElectronAppConfig(descriptor_, "hosted", &config, &error))
      << error;
  EXPECT_EQ(app.AsUTF8Unsafe(), config->app_path);
  EXPECT_EQ(resources.AsUTF8Unsafe(), config->resources_directory);
  EXPECT_EQ(release.AsUTF8Unsafe(), config->runtime_directory);
  EXPECT_EQ(release.AsUTF8Unsafe(), config->working_directory);
  EXPECT_EQ(executable.AsUTF8Unsafe(), config->executable_path);
  EXPECT_EQ(app.AppendASCII("dist").AppendASCII("entry.js").AsUTF8Unsafe(),
            config->main_script_path);
  EXPECT_EQ("Hosted name", config->app_name);
  EXPECT_EQ("3.2", config->app_version);
  EXPECT_EQ(true, config->is_packaged);
  EXPECT_TRUE(config->embedded_main_source.empty());
  ASSERT_EQ(2u, config->parent_window_pairing_urls.size());
  EXPECT_EQ(net::FilePathToFileURL(
                app.AppendASCII("dist").AppendASCII("clipper.html"))
                .spec(),
            config->parent_window_pairing_urls[0]);
  EXPECT_EQ(net::FilePathToFileURL(
                app.AppendASCII("dist").AppendASCII("gif clipper.html"))
                .spec(),
            config->parent_window_pairing_urls[1]);
  EXPECT_EQ(before, Snapshot(release));
}

TEST_F(ElectronAppConfigTest, InvalidDescriptorsKeepPreviousConfiguration) {
  ASSERT_TRUE(WriteApplication(root_.AppendASCII("source")));
  for (
      std::string_view json :
      {"[]", "{invalid}", "{}", R"({"application":42})",
       R"({"application":""})", R"({"application":"../source","unknown":true})",
       R"({"application":"../source","main":false})",
       R"({"application":"../source","main":"../escape.js"})",
       R"({"application":"../source","archivePublicKey":[]})",
       R"({"application":"../source","versionFromExecutable":"true"})",
       R"({"application":"../source","parentWindowPairing":"dist/page.html"})",
       R"({"application":"../source","parentWindowPairing":[42]})",
       R"({"application":"../source","parentWindowPairing":["../escape.html"]})"}) {
    SCOPED_TRACE(json);
    ASSERT_TRUE(base::WriteFile(descriptor_, json));
    auto config = mojom::IpcMainConfig::New();
    config->container_id = "previous";
    std::string error;
    EXPECT_FALSE(LoadElectronAppConfig(descriptor_, "new", &config, &error));
    EXPECT_FALSE(error.empty());
    ASSERT_TRUE(config);
    EXPECT_EQ("previous", config->container_id);
  }
  ASSERT_TRUE(WriteDescriptor(
      base::DictValue()
          .Set("application", "../source")
          .Set("main", root_.AppendASCII("outside.js").AsUTF8Unsafe())));
  mojom::IpcMainConfigPtr config;
  std::string error;
  EXPECT_FALSE(LoadElectronAppConfig(descriptor_, "new", &config, &error));
  EXPECT_FALSE(config);
}

TEST_F(ElectronAppConfigTest,
       InvalidPublicKeyAndExecutableVersionFailExplicitly) {
  ASSERT_TRUE(WriteApplication(root_.AppendASCII("source")));
  ASSERT_TRUE(base::WriteFile(descriptors_.AppendASCII("invalid.pem"),
                              "not a public key"));
  ASSERT_TRUE(WriteDescriptor(base::DictValue()
                                  .Set("application", "../source")
                                  .Set("archivePublicKey", "invalid.pem")));
  mojom::IpcMainConfigPtr config;
  std::string error;
  EXPECT_FALSE(LoadElectronAppConfig(descriptor_, "bad-key", &config, &error));
  EXPECT_FALSE(config);
  EXPECT_NE(std::string::npos, error.find("public key"));

  ASSERT_TRUE(base::WriteFile(descriptors_.AppendASCII("unversioned.bin"),
                              "no executable version resource"));
  ASSERT_TRUE(WriteDescriptor(base::DictValue()
                                  .Set("application", "../source")
                                  .Set("executable", "unversioned.bin")
                                  .Set("versionFromExecutable", true)));
  EXPECT_FALSE(
      LoadElectronAppConfig(descriptor_, "no-version", &config, &error));
  EXPECT_FALSE(config);
  EXPECT_NE(std::string::npos, error.find("version"));
}

TEST_F(ElectronAppConfigTest,
       EncryptedArchiveKeyIsTemporaryAndFilesStayOriginal) {
  const auto release = root_.AppendASCII("release");
  const auto resources = release.AppendASCII("resources");
  ASSERT_TRUE(base::CreateDirectory(resources));
  const auto archive = resources.AppendASCII("app.asar");
  asar::TestArchiveBuilder builder;
  ASSERT_TRUE(builder.Write(
      archive, {{"package.json",
                 R"({"name":"sealed","version":"4.2","main":"bad.js"})", true},
                {"dist/entry.js", "module.exports = 42;", true},
                {"original.bin", "original data", false, true}}));
  ASSERT_TRUE(base::WriteFile(descriptors_.AppendASCII("public.pem"),
                              builder.public_key_pem()));
  ASSERT_TRUE(WriteDescriptor(base::DictValue()
                                  .Set("application", "../release")
                                  .Set("archivePublicKey", "public.pem")
                                  .Set("main", "dist/entry.js")));
  const auto before = Snapshot(release);
  EXPECT_FALSE(asar::GetOrCreateAsarArchive(archive));
  mojom::IpcMainConfigPtr config;
  std::string error;
  ASSERT_TRUE(LoadElectronAppConfig(descriptor_, "sealed-app", &config, &error))
      << error;
  EXPECT_EQ(archive.AsUTF8Unsafe(), config->app_path);
  EXPECT_EQ("sealed", config->app_name);
  EXPECT_EQ("4.2", config->app_version);
  ASSERT_EQ(1u, config->archive_public_keys.size());
  EXPECT_EQ(release.AsUTF8Unsafe(), config->archive_public_keys[0]->root_path);
  EXPECT_EQ(builder.public_key_pem(),
            config->archive_public_keys[0]->public_key_pem);
  // Inspecting a descriptor is not permanent registration. This also checks
  // that the successful metadata read cannot leak a decrypted cache entry.
  EXPECT_FALSE(asar::GetOrCreateAsarArchive(archive));
  EXPECT_EQ(before, Snapshot(release));

  // Registering the returned configuration, as the manager does, is sufficient
  // to read the same unmodified package afterwards.
  ASSERT_TRUE(asar::SetArchivePublicKeys(
      "app-config-test-registered",
      {{base::FilePath::FromUTF8Unsafe(
            config->archive_public_keys[0]->root_path),
        config->archive_public_keys[0]->public_key_pem}}));
  EXPECT_TRUE(asar::GetOrCreateAsarArchive(archive));
}

TEST_F(ElectronAppConfigTest, FailedDescriptorRevokesItsTemporaryKeyOnly) {
  const auto release = root_.AppendASCII("release");
  ASSERT_TRUE(base::CreateDirectory(release));
  const auto archive = release.AppendASCII("app.asar");
  asar::TestArchiveBuilder builder;
  ASSERT_TRUE(builder.Write(
      archive,
      {{"package.json", R"({"name":"sealed","main":"entry.js"})", true}}));
  ASSERT_TRUE(base::WriteFile(descriptors_.AppendASCII("public.pem"),
                              builder.public_key_pem()));
  ASSERT_TRUE(WriteDescriptor(base::DictValue()
                                  .Set("application", "../release/app.asar")
                                  .Set("archivePublicKey", "public.pem")
                                  .Set("main", "../escape.js")));
  mojom::IpcMainConfigPtr config;
  std::string error;
  EXPECT_FALSE(
      LoadElectronAppConfig(descriptor_, "failed-app", &config, &error));
  EXPECT_FALSE(config);
  EXPECT_FALSE(asar::GetOrCreateAsarArchive(archive));

  ASSERT_TRUE(asar::SetArchivePublicKeys(
      "app-config-test-existing", {{release, builder.public_key_pem()}}));
  ASSERT_TRUE(asar::GetOrCreateAsarArchive(archive));
  EXPECT_FALSE(
      LoadElectronAppConfig(descriptor_, "failed-app", &config, &error));
  // The local cleanup cannot revoke another running application's owner.
  EXPECT_TRUE(asar::GetOrCreateAsarArchive(archive));
}

}  // namespace
}  // namespace xenon::ipc
