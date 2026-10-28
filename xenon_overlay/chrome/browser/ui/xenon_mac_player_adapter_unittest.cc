// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ui/xenon_mac_player_adapter.h"

#include <string>
#include <string_view>

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace xenon {
namespace {

constexpr char kDeclaration[] =
    R"({"schemaVersion":1,"adapter":{"id":"xunlei.mac-player","version":1}})";

TEST(MacPlayerAdapterTest, UsesOnlyItsDeclaredContract) {
  constexpr std::string_view exports[] = {"initialize", "shutdown"};
  constexpr std::string_view exclusions[] = {"setNativePointer"};
  constexpr MacPlayerSubscriptionContract subscriptions[] = {
      {"subscribe", "unsubscribe"}};
  constexpr MacPlayerCallbackContract callbacks[] = {
      {"registerProvider", "lookup", 2,
       +[]() { return base::ListValue().Append(false); }},
      {"registerNotification", "", 1, nullptr}};
  const MacPlayerAdapter adapter{
      .id = "test.contract",
      .version = 1,
      .allowed_exports = exports,
      .excluded_instance_methods = exclusions,
      .subscriptions = subscriptions,
      .callbacks = callbacks,
  };
  EXPECT_TRUE(adapter.AllowsExport("initialize"));
  EXPECT_FALSE(adapter.AllowsExport("registerProvider"));
  EXPECT_TRUE(adapter.IsProviderRegistration("registerProvider"));
  EXPECT_TRUE(adapter.IsProviderRegistration("registerNotification"));
  EXPECT_FALSE(adapter.IsProviderRegistration("initialize"));
  EXPECT_EQ("unsubscribe", adapter.DetachMethod("subscribe"));
  EXPECT_TRUE(adapter.DetachMethod("unknown").empty());
  EXPECT_TRUE(adapter.ExcludesInstanceMethod("setNativePointer"));
  EXPECT_FALSE(adapter.ExcludesInstanceMethod("play"));
}

TEST(MacPlayerAdapterTest, RegistryRejectsUnknownIdsAndVersions) {
  ASSERT_NE(nullptr, FindMacPlayerAdapter("xunlei.mac-player", 1));
  EXPECT_EQ(nullptr, FindMacPlayerAdapter("unregistered", 1));
  EXPECT_EQ(nullptr, FindMacPlayerAdapter("xunlei.mac-player", 0));
  EXPECT_EQ(nullptr, FindMacPlayerAdapter("xunlei.mac-player", 2));
}

class BundledMacPlayerAdapterTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(temp_.CreateUniqueTempDir());
    ASSERT_TRUE(base::NormalizeFilePath(temp_.GetPath(), &root_));
    outer_ = root_.AppendASCII("Host.app");
    apps_ = outer_.AppendASCII("Contents")
                .AppendASCII("Resources")
                .AppendASCII("xenon-apps");
    ASSERT_TRUE(base::CreateDirectory(apps_));
  }

  base::FilePath Resources(const std::string& app_name) const {
    return apps_.AppendASCII(app_name)
        .AppendASCII("Contents")
        .AppendASCII("Resources");
  }

  base::FilePath Module(const base::FilePath& resources) const {
    return resources.Append(base::FilePath::FromUTF8Unsafe(
        FindMacPlayerAdapter("xunlei.mac-player", 1)->module_relative_path));
  }

  bool WriteApp(const base::FilePath& resources) {
    const auto module = Module(resources);
    return base::CreateDirectory(module.DirName()) &&
           base::WriteFile(module, "native-module-fixture") &&
           WriteDeclaration(resources, kDeclaration);
  }

  bool WriteDeclaration(const base::FilePath& resources,
                        std::string_view text) {
    return base::WriteFile(resources.AppendASCII("xenon-native-player.json"),
                           text);
  }

  void ExpectRejected(const base::FilePath& resources) const {
    const auto resolved = ResolveBundledMacPlayerAdapter(outer_, resources);
    EXPECT_EQ(nullptr, resolved.adapter);
    EXPECT_TRUE(resolved.module_path.empty());
  }

  base::ScopedTempDir temp_;
  base::FilePath root_;
  base::FilePath outer_;
  base::FilePath apps_;
};

TEST_F(BundledMacPlayerAdapterTest, ResolvesRenamedEmbeddedApplication) {
  const auto resources = Resources("Renamed Player.app");
  ASSERT_TRUE(WriteApp(resources));
  const auto resolved = ResolveBundledMacPlayerAdapter(outer_, resources);
  EXPECT_EQ(FindMacPlayerAdapter("xunlei.mac-player", 1), resolved.adapter);
  EXPECT_EQ(Module(resources).AsUTF8Unsafe(), resolved.module_path);
}

TEST_F(BundledMacPlayerAdapterTest, ResolvesEquivalentOuterBundlePathAliases) {
  const auto resources = Resources("Player.app");
  ASSERT_TRUE(WriteApp(resources));
  const auto alias = root_.AppendASCII("Host Alias.app");
  ASSERT_TRUE(base::CreateSymbolicLink(outer_, alias));
  base::FilePath relative_resources;
  ASSERT_TRUE(outer_.AppendRelativePath(resources, &relative_resources));
  const auto aliased_resources = alias.Append(relative_resources);

  const auto aliased_outer = ResolveBundledMacPlayerAdapter(alias, resources);
  EXPECT_EQ(FindMacPlayerAdapter("xunlei.mac-player", 1),
            aliased_outer.adapter);
  EXPECT_EQ(Module(resources).AsUTF8Unsafe(), aliased_outer.module_path);
  const auto aliased_app =
      ResolveBundledMacPlayerAdapter(outer_, aliased_resources);
  EXPECT_EQ(FindMacPlayerAdapter("xunlei.mac-player", 1), aliased_app.adapter);
  EXPECT_EQ(Module(resources).AsUTF8Unsafe(), aliased_app.module_path);
}

TEST_F(BundledMacPlayerAdapterTest, RequiresDeclarationAndFileModule) {
  const auto resources = Resources("Player.app");
  ASSERT_TRUE(WriteApp(resources));
  ASSERT_TRUE(
      base::DeleteFile(resources.AppendASCII("xenon-native-player.json")));
  ExpectRejected(resources);
  ASSERT_TRUE(WriteDeclaration(resources, kDeclaration));
  ASSERT_TRUE(base::DeleteFile(Module(resources)));
  ExpectRejected(resources);
  ASSERT_TRUE(base::CreateDirectory(Module(resources)));
  ExpectRejected(resources);
}

TEST_F(BundledMacPlayerAdapterTest, RejectsInvalidOrExtendedDeclarations) {
  const auto resources = Resources("Player.app");
  ASSERT_TRUE(WriteApp(resources));
  for (
      const auto* declaration : {
          "{}",
          "[]",
          R"({"schemaVersion":2,"adapter":{"id":"xunlei.mac-player","version":1}})",
          R"({"schemaVersion":"1","adapter":{"id":"xunlei.mac-player","version":1}})",
          R"({"schemaVersion":1,"adapter":{"id":"unknown","version":1}})",
          R"({"schemaVersion":1,"adapter":{"id":"xunlei.mac-player","version":2}})",
          R"({"schemaVersion":1,"adapter":{"id":"xunlei.mac-player","version":"1"}})",
          R"({"schemaVersion":1,"adapter":{"id":"xunlei.mac-player"}})",
          R"({"schemaVersion":1,"adapter":{"id":"xunlei.mac-player","version":1,"path":"other.node"}})",
          R"({"schemaVersion":1,"adapter":{"id":"xunlei.mac-player","version":1},"module":"other.node"})",
      }) {
    SCOPED_TRACE(declaration);
    ASSERT_TRUE(WriteDeclaration(resources, declaration));
    ExpectRejected(resources);
  }
  ASSERT_TRUE(WriteDeclaration(resources, std::string(4097, ' ')));
  ExpectRejected(resources);
}

TEST_F(BundledMacPlayerAdapterTest,
       RejectsExternalAndNonImmediateApplications) {
  for (const auto& resources : {
           root_.AppendASCII("External.app/Contents/Resources"),
           apps_.AppendASCII("Nested/Player.app/Contents/Resources"),
           apps_.AppendASCII("Player/Contents/Resources"),
           outer_.AppendASCII(
               "Contents/Resources/Player.app/Contents/Resources"),
       }) {
    ASSERT_TRUE(WriteApp(resources));
    ExpectRejected(resources);
  }
}

TEST_F(BundledMacPlayerAdapterTest, RejectsRedirectedTrustRoot) {
  const auto external_apps = root_.AppendASCII("external-apps");
  ASSERT_TRUE(
      WriteApp(external_apps.AppendASCII("Player.app/Contents/Resources")));
  ASSERT_TRUE(base::DeleteFile(apps_));
  ASSERT_TRUE(base::CreateSymbolicLink(external_apps, apps_));
  ExpectRejected(Resources("Player.app"));
}

TEST_F(BundledMacPlayerAdapterTest, RejectsApplicationSymlinkEscape) {
  const auto external_app = root_.AppendASCII("External.app");
  ASSERT_TRUE(WriteApp(external_app.AppendASCII("Contents/Resources")));
  ASSERT_TRUE(
      base::CreateSymbolicLink(external_app, apps_.AppendASCII("Player.app")));
  ExpectRejected(Resources("Player.app"));
}

TEST_F(BundledMacPlayerAdapterTest, RejectsResourcesSymlinkEscape) {
  const auto external_resources = root_.AppendASCII("external-resources");
  ASSERT_TRUE(WriteApp(external_resources));
  const auto resources = Resources("Player.app");
  ASSERT_TRUE(base::CreateDirectory(resources.DirName()));
  ASSERT_TRUE(base::CreateSymbolicLink(external_resources, resources));
  ExpectRejected(resources);
}

TEST_F(BundledMacPlayerAdapterTest, RejectsDeclarationAndModuleSymlinkEscapes) {
  const auto resources = Resources("Player.app");
  ASSERT_TRUE(WriteApp(resources));
  const auto declaration = resources.AppendASCII("xenon-native-player.json");
  const auto external_declaration = root_.AppendASCII("declaration.json");
  ASSERT_TRUE(base::WriteFile(external_declaration, kDeclaration));
  ASSERT_TRUE(base::DeleteFile(declaration));
  ASSERT_TRUE(base::CreateSymbolicLink(external_declaration, declaration));
  ExpectRejected(resources);

  ASSERT_TRUE(base::DeleteFile(declaration));
  ASSERT_TRUE(WriteDeclaration(resources, kDeclaration));
  const auto external_module = root_.AppendASCII("external.node");
  ASSERT_TRUE(base::WriteFile(external_module, "native-module-fixture"));
  ASSERT_TRUE(base::DeleteFile(Module(resources)));
  ASSERT_TRUE(base::CreateSymbolicLink(external_module, Module(resources)));
  ExpectRejected(resources);
}

TEST_F(BundledMacPlayerAdapterTest, ResolvesInternalModuleSymlinkCanonically) {
  const auto resources = Resources("Player.app");
  ASSERT_TRUE(WriteApp(resources));
  const auto actual_module = resources.AppendASCII("versioned.node");
  ASSERT_TRUE(base::Move(Module(resources), actual_module));
  ASSERT_TRUE(base::CreateSymbolicLink(actual_module, Module(resources)));
  const auto resolved = ResolveBundledMacPlayerAdapter(outer_, resources);
  EXPECT_EQ(FindMacPlayerAdapter("xunlei.mac-player", 1), resolved.adapter);
  EXPECT_EQ(actual_module.AsUTF8Unsafe(), resolved.module_path);
}

}  // namespace
}  // namespace xenon
