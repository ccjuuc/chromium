// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_app_runtime.h"

#include <string_view>

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "build/build_config.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "xenon_overlay/common/asar/archive.h"
#include "xenon_overlay/common/asar/test_support.h"

#if BUILDFLAG(IS_WIN)
#include "base/test/file_path_reparse_point_win.h"
#endif

namespace xenon::ipc {
namespace {

class ElectronApplicationTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(temp_.CreateUniqueTempDir());
    ASSERT_TRUE(base::NormalizeFilePath(temp_.GetPath(), &root_));
  }
  void TearDown() override {
    asar::RemoveArchivePublicKeys("application-runtime-test");
  }
  bool WritePackage(const base::FilePath& directory, std::string_view source) {
    return base::CreateDirectory(directory) &&
           base::WriteFile(directory.AppendASCII("package.json"), source);
  }
  base::ScopedTempDir temp_;
  base::FilePath root_;
};

TEST_F(ElectronApplicationTest, ResolvesSourceAndPackagedLayouts) {
  const auto source = root_.AppendASCII("source");
  ASSERT_TRUE(WritePackage(source, R"({"name":"source-app","version":"1.2"})"));
  ElectronApplicationInfo result;
  std::string error;
  ASSERT_TRUE(ResolveElectronApplication(source, &result, &error)) << error;
  EXPECT_EQ(source, result.app_path);
  EXPECT_EQ("source-app", result.name);
  EXPECT_EQ("1.2", result.version);
  EXPECT_EQ(source.AppendASCII("index.js"), result.main_script_path);
  EXPECT_FALSE(result.is_packaged);

  const auto release = root_.AppendASCII("release");
  const auto resources = release.AppendASCII("resources");
  const auto app = resources.AppendASCII("app");
  ASSERT_TRUE(WritePackage(
      app, R"({"name":"app","productName":"Display App","main":"dist"})"));
  for (const auto& entry : {release, resources, app}) {
    ASSERT_TRUE(ResolveElectronApplication(entry, &result, &error)) << error;
    EXPECT_EQ(app, result.app_path);
    EXPECT_EQ(resources, result.resources_directory);
    EXPECT_EQ(release, result.runtime_directory);
    EXPECT_EQ(app.AppendASCII("dist"), result.main_script_path);
    EXPECT_EQ("Display App", result.name);
    EXPECT_TRUE(result.is_packaged);
  }
}

TEST_F(ElectronApplicationTest, PrefersOriginalEncryptedArchiveOverDirectory) {
  const auto resources = root_.AppendASCII("resources");
  ASSERT_TRUE(
      WritePackage(resources.AppendASCII("app"), R"({"name":"directory"})"));
  asar::TestArchiveBuilder builder;
  const auto archive = resources.AppendASCII("app.asar");
  ASSERT_TRUE(builder.Write(
      archive,
      {{"package.json",
        R"({"name":"archive","version":"2.0","main":"dist/main"})", true}}));
  ASSERT_TRUE(asar::SetArchivePublicKeys(
      "application-runtime-test", {{resources, builder.public_key_pem()}}));
  for (const auto& entry : {root_, resources, archive}) {
    ElectronApplicationInfo result;
    std::string error;
    ASSERT_TRUE(ResolveElectronApplication(entry, &result, &error)) << error;
    EXPECT_EQ(archive, result.app_path);
    EXPECT_EQ(resources, result.resources_directory);
    EXPECT_EQ(root_, result.runtime_directory);
    EXPECT_EQ("archive", result.name);
    EXPECT_EQ("2.0", result.version);
    EXPECT_TRUE(result.is_packaged);
  }
}

TEST_F(ElectronApplicationTest,
       ResolvesMacBundleResourcesWithoutPlatformAssumptions) {
  const auto bundle = root_.AppendASCII("Fixture.app");
  const auto contents = bundle.AppendASCII("Contents");
  const auto resources = contents.AppendASCII("Resources");
  const auto app = resources.AppendASCII("app");
  ASSERT_TRUE(WritePackage(app, R"({"name":"mac-app"})"));
  for (const auto& entry : {bundle, resources, app}) {
    ElectronApplicationInfo result;
    std::string error;
    ASSERT_TRUE(ResolveElectronApplication(entry, &result, &error)) << error;
    EXPECT_EQ(app, result.app_path);
    EXPECT_EQ(resources, result.resources_directory);
    EXPECT_EQ(contents.AppendASCII("MacOS"), result.runtime_directory);
    EXPECT_TRUE(result.is_packaged);
  }
}

TEST_F(ElectronApplicationTest,
       InvalidManifestDoesNotReplaceResolvedInformation) {
  const auto app = root_.AppendASCII("bad");
  ElectronApplicationInfo result;
  result.name = "preserved";
  std::string error;
  for (std::string_view manifest : {"[]", "{invalid}", "{\"main\":42}"}) {
    ASSERT_TRUE(WritePackage(app, manifest));
    EXPECT_FALSE(ResolveElectronApplication(app, &result, &error));
    EXPECT_FALSE(error.empty());
    EXPECT_EQ("preserved", result.name);
  }
}

TEST_F(ElectronApplicationTest,
       DiscoversSingleNestedReleaseWithoutProductNames) {
  const auto release = root_.AppendASCII("release");
  const auto runtime = release.AppendASCII("opaque-build-id");
  const auto resources = runtime.AppendASCII("resources");
  const auto app = resources.AppendASCII("app");
  ASSERT_TRUE(
      WritePackage(app, R"({"name":"nested-app","main":"out/main.js"})"));
  ASSERT_TRUE(base::CreateDirectory(release.AppendASCII("unrelated-assets")));
  ElectronApplicationInfo result;
  std::string error;
  ASSERT_TRUE(ResolveElectronApplication(release, &result, &error)) << error;
  EXPECT_EQ(app, result.app_path);
  EXPECT_EQ(resources, result.resources_directory);
  EXPECT_EQ(runtime, result.runtime_directory);
  EXPECT_EQ(
      app.AppendASCII("out").AppendASCII("main.js").NormalizePathSeparators(),
      result.main_script_path.NormalizePathSeparators());
  EXPECT_TRUE(result.is_packaged);
}

TEST_F(ElectronApplicationTest, NestedReleaseKeepsArchivePrecedence) {
  const auto release = root_.AppendASCII("release");
  const auto resources = release.AppendASCII("build").AppendASCII("resources");
  ASSERT_TRUE(
      WritePackage(resources.AppendASCII("app"), R"({"name":"directory"})"));
  asar::TestArchiveBuilder builder;
  const auto archive = resources.AppendASCII("app.asar");
  ASSERT_TRUE(builder.Write(archive,
                            {{"package.json", R"({"name":"archive"})", true}}));
  ASSERT_TRUE(asar::SetArchivePublicKeys(
      "application-runtime-test", {{resources, builder.public_key_pem()}}));
  ElectronApplicationInfo result;
  std::string error;
  ASSERT_TRUE(ResolveElectronApplication(release, &result, &error)) << error;
  EXPECT_EQ(archive, result.app_path);
  EXPECT_EQ("archive", result.name);
}

TEST_F(ElectronApplicationTest, DirectLayoutsWinOverNestedCandidates) {
  const auto release = root_.AppendASCII("release");
  ASSERT_TRUE(WritePackage(release, R"({"name":"source"})"));
  ASSERT_TRUE(WritePackage(release.AppendASCII("version")
                               .AppendASCII("resources")
                               .AppendASCII("app"),
                           R"({"name":"nested"})"));
  ElectronApplicationInfo result;
  std::string error;
  ASSERT_TRUE(ResolveElectronApplication(release, &result, &error)) << error;
  EXPECT_EQ(release, result.app_path);
  EXPECT_EQ("source", result.name);

  const auto app = release.AppendASCII("resources").AppendASCII("app");
  ASSERT_TRUE(WritePackage(app, R"({"name":"direct-release"})"));
  ASSERT_TRUE(ResolveElectronApplication(release, &result, &error)) << error;
  EXPECT_EQ(app, result.app_path);
  EXPECT_EQ("direct-release", result.name);
}

TEST_F(ElectronApplicationTest, InvalidRootManifestNeverFallsBackToNestedApp) {
  const auto release = root_.AppendASCII("release");
  ASSERT_TRUE(WritePackage(release, "invalid"));
  ASSERT_TRUE(WritePackage(release.AppendASCII("version")
                               .AppendASCII("resources")
                               .AppendASCII("app"),
                           R"({"name":"nested"})"));
  ElectronApplicationInfo result;
  std::string error;
  EXPECT_FALSE(ResolveElectronApplication(release, &result, &error));
  EXPECT_NE(std::string::npos, error.find("package.json is invalid"));
}

TEST_F(ElectronApplicationTest, AmbiguousNestedReleasesRequireExplicitChoice) {
  const auto release = root_.AppendASCII("release");
  const auto older =
      release.AppendASCII("first").AppendASCII("resources").AppendASCII("app");
  const auto newer =
      release.AppendASCII("second").AppendASCII("resources").AppendASCII("app");
  ASSERT_TRUE(WritePackage(older, R"({"name":"older"})"));
  ASSERT_TRUE(WritePackage(newer, R"({"name":"newer"})"));
  ElectronApplicationInfo result;
  result.name = "preserved";
  std::string error;
  EXPECT_FALSE(ResolveElectronApplication(release, &result, &error));
  EXPECT_EQ("preserved", result.name);
  EXPECT_NE(std::string::npos, error.find("Multiple Electron applications"));
  EXPECT_NE(std::string::npos, error.find(older.AsUTF8Unsafe()));
  EXPECT_NE(std::string::npos, error.find(newer.AsUTF8Unsafe()));
  ASSERT_TRUE(ResolveElectronApplication(newer, &result, &error)) << error;
  EXPECT_EQ("newer", result.name);
}

TEST_F(ElectronApplicationTest, DiscoveryDoesNotRecurseIntoArbitraryTrees) {
  const auto release = root_.AppendASCII("release");
  ASSERT_TRUE(WritePackage(release.AppendASCII("outer")
                               .AppendASCII("inner")
                               .AppendASCII("resources")
                               .AppendASCII("app"),
                           R"({"name":"too-deep"})"));
  ElectronApplicationInfo result;
  std::string error;
  EXPECT_FALSE(ResolveElectronApplication(release, &result, &error));
  EXPECT_NE(std::string::npos, error.find("no readable package.json"));
}

TEST_F(ElectronApplicationTest, DiscoveryDoesNotFollowLinkedLayoutComponents) {
  const auto external = root_.AppendASCII("external");
  const auto external_resources = external.AppendASCII("resources");
  const auto external_app = external_resources.AppendASCII("app");
  ASSERT_TRUE(WritePackage(external_app, R"({"name":"external"})"));
  for (int depth = 0; depth < 3; ++depth) {
    const auto release = root_.AppendASCII("release-" + std::to_string(depth));
    auto linked = release.AppendASCII("version");
    auto target = external;
    if (depth >= 1) {
      linked = linked.AppendASCII("resources");
      target = external_resources;
    }
    if (depth >= 2) {
      linked = linked.AppendASCII("app");
      target = external_app;
    }
    ASSERT_TRUE(base::CreateDirectory(linked.DirName()));
#if BUILDFLAG(IS_WIN)
    ASSERT_TRUE(base::CreateDirectory(linked));
    auto junction = base::test::FilePathReparsePoint::Create(linked, target);
    ASSERT_TRUE(junction.has_value());
#else
    ASSERT_TRUE(base::CreateSymbolicLink(target, linked));
#endif
    ElectronApplicationInfo result;
    std::string error;
    EXPECT_FALSE(ResolveElectronApplication(release, &result, &error)) << depth;
    EXPECT_NE(std::string::npos, error.find("no readable package.json"));
  }
}

TEST_F(ElectronApplicationTest,
       RejectsWorkingDirectoryDependentApplicationPath) {
  ElectronApplicationInfo result;
  std::string error;
  EXPECT_FALSE(ResolveElectronApplication(
      base::FilePath(FILE_PATH_LITERAL(".")), &result, &error));
  EXPECT_NE(std::string::npos, error.find("absolute"));
}

}  // namespace
}  // namespace xenon::ipc
