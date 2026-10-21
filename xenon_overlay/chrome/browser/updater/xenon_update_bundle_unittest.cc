// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/updater/xenon_update_bundle.h"

#include <string>
#include <string_view>
#include <utility>

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/json/json_writer.h"
#include "base/strings/string_number_conversions.h"
#include "base/values.h"
#include "build/build_config.h"
#include "crypto/hash.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "xenon_overlay/chrome/browser/updater/xenon_update_patcher.h"

namespace xenon::updater {
namespace {

base::DictValue Directory(std::string target, int mode = 0755) {
  return base::DictValue()
      .Set("action", "directory")
      .Set("target", std::move(target))
      .Set("mode", mode);
}

base::DictValue Link(std::string target, std::string link_target) {
  return base::DictValue()
      .Set("action", "symlink")
      .Set("target", std::move(target))
      .Set("link_target", std::move(link_target));
}

base::DictValue FileAction(std::string action,
                           std::string target,
                           std::string_view contents,
                           int mode = 0644) {
  return base::DictValue()
      .Set("action", std::move(action))
      .Set("target", std::move(target))
      .Set("size", static_cast<double>(contents.size()))
      .Set("sha256", base::HexEncode(crypto::hash::Sha256(contents)))
      .Set("mode", mode);
}

class XenonUpdateBundleTest : public testing::Test {
 public:
  void SetUp() override {
    ASSERT_TRUE(temp_.CreateUniqueTempDir());
    current_ = temp_.GetPath().AppendASCII("Current.app");
    patch_ = temp_.GetPath().AppendASCII("patch");
    output_ = temp_.GetPath().AppendASCII("Updated.app");
    ASSERT_TRUE(base::CreateDirectory(current_));
    ASSERT_TRUE(base::CreateDirectory(patch_));
  }

 protected:
  void Write(const base::FilePath& root,
             const std::string& path,
             std::string_view contents) {
    const base::FilePath file =
        root.Append(base::FilePath::FromUTF8Unsafe(path));
    ASSERT_TRUE(base::CreateDirectory(file.DirName()));
    ASSERT_TRUE(base::WriteFile(file, contents));
  }

  std::string Read(const std::string& relative) {
    std::string contents;
    EXPECT_TRUE(base::ReadFileToString(
        output_.Append(base::FilePath::FromUTF8Unsafe(relative)), &contents));
    return contents;
  }

  base::DictValue Manifest(base::ListValue actions) {
    return base::DictValue()
        .Set("schema_version", 2)
        .Set("platform", "mac")
        .Set("base_version", "1.0.0.1")
        .Set("target_version", "1.0.0.2")
        .Set("root_mode", 0755)
        .Set("actions", std::move(actions));
  }

  bool Reconstruct(base::DictValue manifest) {
    auto json = base::WriteJson(manifest);
    EXPECT_TRUE(json);
    if (!json || !base::WriteFile(patch_.AppendASCII("manifest.json"), *json)) {
      return false;
    }
    return ReconstructMacUpdateBundle(current_, patch_, output_, "1.0.0.1",
                                      "1.0.0.2", &error_);
  }

  base::ListValue OneFile(std::string_view contents = "new data") {
    Write(patch_, "files/data", contents);
    base::ListValue actions;
    actions.Append(Directory("Contents"));
    actions.Append(FileAction("add", "Contents/data", contents)
                       .Set("source", "files/data"));
    return actions;
  }

  void ExpectRejected(base::DictValue manifest) {
    EXPECT_FALSE(Reconstruct(std::move(manifest)));
    EXPECT_FALSE(error_.empty());
    EXPECT_FALSE(base::PathExists(output_));
  }

  base::ScopedTempDir temp_;
  base::FilePath current_;
  base::FilePath patch_;
  base::FilePath output_;
  std::string error_;
};

TEST_F(XenonUpdateBundleTest,
       ReconstructsExactTreeUsingExplicitOldVersionPath) {
  Write(current_, "Contents/Versions/1.0.0.1/program", "unchanged program");
  Write(current_, "Contents/removed", "must not survive");
  Write(patch_, "files/Info.plist", "new plist");
  base::ListValue actions;
  // Deliberately unsorted: reconstruction must not depend on manifest order.
  actions.Append(FileAction("copy", "Contents/Versions/1.0.0.2/program",
                            "unchanged program", 0755)
                     .Set("base", "Contents/Versions/1.0.0.1/program"));
  actions.Append(Directory("Contents/Versions/1.0.0.2"));
  actions.Append(Directory("Contents"));
  actions.Append(Directory("Contents/Versions"));
  actions.Append(Directory("Contents/Empty"));
  actions.Append(FileAction("add", "Contents/Info.plist", "new plist")
                     .Set("source", "files/Info.plist"));
  ASSERT_TRUE(Reconstruct(Manifest(std::move(actions)))) << error_;
  EXPECT_EQ(Read("Contents/Versions/1.0.0.2/program"), "unchanged program");
  EXPECT_EQ(Read("Contents/Info.plist"), "new plist");
  EXPECT_TRUE(base::DirectoryExists(output_.AppendASCII("Contents/Empty")));
  EXPECT_FALSE(base::PathExists(output_.AppendASCII("Contents/removed")));
  EXPECT_TRUE(base::PathExists(current_.AppendASCII("Contents/removed")));
}

TEST_F(XenonUpdateBundleTest, AppliesPatchAndVerifiesFinalFile) {
  const std::string original = "old program contents 1.0.0.1";
  const std::string updated = "new program contents 1.0.0.2 with extra data";
  Write(current_, "program", original);
  const base::FilePath target = temp_.GetPath().AppendASCII("target");
  ASSERT_TRUE(base::WriteFile(target, updated));
  ASSERT_TRUE(
      XenonUpdatePatcher::GeneratePatch(current_.AppendASCII("program"), target,
                                        patch_.AppendASCII("program.zucc")));
  base::ListValue actions;
  actions.Append(FileAction("patch", "program", updated, 0755)
                     .Set("base", "program")
                     .Set("patch", "program.zucc"));
  ASSERT_TRUE(Reconstruct(Manifest(std::move(actions)))) << error_;
  EXPECT_EQ(Read("program"), updated);
}

TEST_F(XenonUpdateBundleTest, AcceptsVerifiedEmptyFiles) {
  ASSERT_TRUE(Reconstruct(Manifest(OneFile("")))) << error_;
  EXPECT_TRUE(Read("Contents/data").empty());
}

TEST_F(XenonUpdateBundleTest, RejectsWrongHashAndCleansOutput) {
  auto actions = OneFile();
  actions[1].GetDict().Set("sha256", std::string(64, '0'));
  ExpectRejected(Manifest(std::move(actions)));
  EXPECT_NE(error_.find("SHA-256"), std::string::npos);
}

TEST_F(XenonUpdateBundleTest, RejectsWrongSizeAndCleansOutput) {
  auto actions = OneFile();
  actions[1].GetDict().Set("size", 999);
  ExpectRejected(Manifest(std::move(actions)));
}

TEST_F(XenonUpdateBundleTest, RejectsMissingAndNonRegularSources) {
  base::ListValue actions;
  actions.Append(FileAction("copy", "data", "old data").Set("base", "missing"));
  ExpectRejected(Manifest(std::move(actions)));
  ASSERT_TRUE(base::CreateDirectory(current_.AppendASCII("not_a_file")));
  base::ListValue directory_source;
  directory_source.Append(
      FileAction("copy", "data", "old data").Set("base", "not_a_file"));
  ExpectRejected(Manifest(std::move(directory_source)));
}

TEST_F(XenonUpdateBundleTest, RequiresMatchingVersionsAndNewSchema) {
  auto manifest = Manifest(OneFile());
  manifest.Set("base_version", "1.0.0.0");
  ExpectRejected(std::move(manifest));
  manifest = Manifest(OneFile());
  manifest.Set("target_version", "1.0.0.3");
  ExpectRejected(std::move(manifest));
  manifest = Manifest(OneFile());
  manifest.Remove("schema_version");
  ExpectRejected(std::move(manifest));
  manifest = Manifest(OneFile());
  manifest.Set("platform", "win");
  ExpectRejected(std::move(manifest));
}

TEST_F(XenonUpdateBundleTest, RejectsTraversalAndAbsolutePaths) {
  for (const char* target :
       {"../escape", "/absolute", "C:/absolute", "Contents/../escape",
        "Contents//data", "Contents\\data", "Contents/./data"}) {
    SCOPED_TRACE(target);
    auto actions = OneFile();
    actions[1].GetDict().Set("target", target);
    ExpectRejected(Manifest(std::move(actions)));
  }
  auto actions = OneFile();
  actions[1].GetDict().Set("source", "../Current.app/data");
  ExpectRejected(Manifest(std::move(actions)));
}

TEST_F(XenonUpdateBundleTest, RejectsMalformedEntriesAndDuplicateTargets) {
  auto actions = OneFile();
  actions.Append(17);
  ExpectRejected(Manifest(std::move(actions)));
  actions = OneFile();
  actions[1].GetDict().Remove("sha256");
  ExpectRejected(Manifest(std::move(actions)));
  actions = OneFile();
  actions[1].GetDict().Set("action", "execute");
  ExpectRejected(Manifest(std::move(actions)));
  actions = OneFile();
  actions[1].GetDict().Set("mode", 04755);
  ExpectRejected(Manifest(std::move(actions)));
  actions = OneFile();
  actions.Append(actions[1].Clone());
  ExpectRejected(Manifest(std::move(actions)));
  actions = OneFile();
  actions.Append(Directory("contents"));
  ExpectRejected(Manifest(std::move(actions)));
}

TEST_F(XenonUpdateBundleTest, RejectsSymlinkParentsAndEscapingLinkChains) {
  auto actions = OneFile();
  actions.Append(Link("alias", "Contents"));
  actions.Append(FileAction("add", "alias/injected", "new data")
                     .Set("source", "files/data"));
  ExpectRejected(Manifest(std::move(actions)));

  actions = OneFile();
  actions.Append(Link("Contents/up", ".."));
  actions.Append(Link("Contents/escape", "up/.."));
  ExpectRejected(Manifest(std::move(actions)));

  actions = OneFile();
  actions.Append(Link("a", "b"));
  actions.Append(Link("b", "a"));
  ExpectRejected(Manifest(std::move(actions)));

  actions = OneFile();
  actions.Append(Link("missing", "not_in_manifest"));
  ExpectRejected(Manifest(std::move(actions)));
}

TEST_F(XenonUpdateBundleTest, DoesNotOverwriteExistingOutput) {
  Write(output_, "sentinel", "leave me alone");
  EXPECT_FALSE(Reconstruct(Manifest(OneFile())));
  EXPECT_EQ(Read("sentinel"), "leave me alone");
}

TEST_F(XenonUpdateBundleTest, RejectsOutputInsideSourceTree) {
  output_ = current_.AppendASCII("Updated.app");
  ExpectRejected(Manifest(OneFile()));
}

#if BUILDFLAG(IS_POSIX)
TEST_F(XenonUpdateBundleTest, PreservesExecutableModesAndFrameworkLinks) {
  Write(patch_, "files/program", "program");
  base::ListValue actions;
  actions.Append(Directory("Framework"));
  actions.Append(Directory("Framework/Versions"));
  actions.Append(Directory("Framework/Versions/1.0.0.2", 0555));
  actions.Append(
      FileAction("add", "Framework/Versions/1.0.0.2/program", "program", 0751)
          .Set("source", "files/program"));
  actions.Append(Link("Framework/Versions/Current", "1.0.0.2"));
  actions.Append(Link("Framework/program", "Versions/Current/program"));
  ASSERT_TRUE(Reconstruct(Manifest(std::move(actions)))) << error_;
  EXPECT_EQ(Read("Framework/program"), "program");
  int mode = 0;
  ASSERT_TRUE(base::GetPosixFilePermissions(
      output_.AppendASCII("Framework/Versions/1.0.0.2/program"), &mode));
  EXPECT_EQ(mode, 0751);
  ASSERT_TRUE(base::GetPosixFilePermissions(
      output_.AppendASCII("Framework/Versions/1.0.0.2"), &mode));
  EXPECT_EQ(mode, 0555);
  base::FilePath link;
  ASSERT_TRUE(
      base::ReadSymbolicLink(output_.AppendASCII("Framework/program"), &link));
  EXPECT_EQ(link.value(), "Versions/Current/program");
  // Restore directory permissions so the test fixture can clean up.
  ASSERT_TRUE(base::SetPosixFilePermissions(
      output_.AppendASCII("Framework/Versions/1.0.0.2"), 0755));
}

TEST_F(XenonUpdateBundleTest, RejectsSourceSymlinkAndSymlinkParent) {
  Write(current_, "real/data", "old data");
  ASSERT_TRUE(base::CreateSymbolicLink(base::FilePath("real"),
                                       current_.AppendASCII("alias")));
  base::ListValue actions;
  actions.Append(
      FileAction("copy", "data", "old data").Set("base", "alias/data"));
  ExpectRejected(Manifest(std::move(actions)));
  ASSERT_TRUE(base::CreateSymbolicLink(base::FilePath("real/data"),
                                       current_.AppendASCII("alias_file")));
  actions.Append(
      FileAction("copy", "data", "old data").Set("base", "alias_file"));
  ExpectRejected(Manifest(std::move(actions)));
}
#endif

}  // namespace
}  // namespace xenon::updater
