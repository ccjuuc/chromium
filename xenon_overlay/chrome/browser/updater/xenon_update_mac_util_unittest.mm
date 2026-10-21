// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/updater/xenon_update_mac_util.h"

#include <sys/xattr.h>

#include "base/apple/foundation_util.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "xenon_overlay/chrome/browser/updater/xenon_update_installer.h"

namespace xenon::updater {
namespace {

class XenonUpdateMacRelaunchTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(temp_.CreateUniqueTempDir());
    target_ = temp_.GetPath().Append("Target.app");
    executable_ = target_.Append("Contents/MacOS/browser");
    ASSERT_TRUE(base::CreateDirectory(executable_.DirName()));
    ASSERT_TRUE(base::WriteFile(executable_, "test executable"));
    ASSERT_TRUE(base::SetPosixFilePermissions(executable_, 0755));
    NSDictionary* properties = @{
      @"CFBundleIdentifier" : @"com.xenon.relaunch-test",
      @"CFBundleExecutable" : @"browser",
    };
    ASSERT_TRUE([properties
        writeToFile:base::apple::FilePathToNSString(
                        target_.Append("Contents/Info.plist"))
         atomically:YES]);
  }

  NSString* target_path() { return base::apple::FilePathToNSString(target_); }
  NSURL* target_url() { return [NSURL fileURLWithPath:target_path()]; }
  NSURL* executable_url() {
    return [NSURL fileURLWithPath:base::apple::FilePathToNSString(executable_)];
  }

  base::ScopedTempDir temp_;
  base::FilePath target_;
  base::FilePath executable_;
};

TEST_F(XenonUpdateMacRelaunchTest, RejectsMissingOrTerminatedProcess) {
  for (pid_t pid : {-1, 0}) {
    NSString* error = nil;
    EXPECT_FALSE(ValidateRelaunchResult(target_path(), pid, false, target_url(),
                                        executable_url(), &error));
    EXPECT_NE(error, nil);
    EXPECT_TRUE([error containsString:@"no live application"]);
  }
  NSString* error = nil;
  EXPECT_FALSE(ValidateRelaunchResult(target_path(), 123, true, target_url(),
                                      executable_url(), &error));
  EXPECT_NE(error, nil);
}

TEST_F(XenonUpdateMacRelaunchTest, AcceptsMatchingLiveApplication) {
  NSString* error = nil;
  EXPECT_TRUE(ValidateRelaunchResult(target_path(), 123, false, target_url(),
                                     executable_url(), &error));
  EXPECT_EQ(error, nil);
}

TEST_F(XenonUpdateMacRelaunchTest, RejectsAnotherCopyWithSameBundleIdentifier) {
  const base::FilePath other = temp_.GetPath().Append("Other.app");
  ASSERT_TRUE(base::CopyDirectory(target_, other, true));
  NSURL* other_url =
      [NSURL fileURLWithPath:base::apple::FilePathToNSString(other)];
  NSURL* other_executable_url =
      [NSURL fileURLWithPath:base::apple::FilePathToNSString(
                                 other.Append("Contents/MacOS/browser"))];
  NSString* error = nil;
  EXPECT_FALSE(ValidateRelaunchResult(target_path(), 123, false, other_url,
                                      other_executable_url, &error));
  EXPECT_TRUE([error containsString:@"unexpected application"]);
  EXPECT_FALSE(ValidateRelaunchResult(target_path(), 123, false, target_url(),
                                      other_executable_url, &error));
  EXPECT_FALSE(ValidateRelaunchResult(target_path(), 123, false, other_url,
                                      executable_url(), &error));
}

TEST_F(XenonUpdateMacRelaunchTest, RejectsMissingOrNonFileUrls) {
  NSString* error = nil;
  EXPECT_FALSE(ValidateRelaunchResult(target_path(), 123, false, nil,
                                      executable_url(), &error));
  EXPECT_FALSE(ValidateRelaunchResult(target_path(), 123, false, target_url(),
                                      nil, &error));
  EXPECT_FALSE(ValidateRelaunchResult(
      target_path(), 123, false, [NSURL URLWithString:@"https://example.com"],
      executable_url(), &error));
}

TEST_F(XenonUpdateMacRelaunchTest, AcceptsCanonicalSymlinkPaths) {
  const base::FilePath alias = temp_.GetPath().Append("Alias.app");
  ASSERT_TRUE(base::CreateSymbolicLink(target_, alias));
  NSURL* alias_url =
      [NSURL fileURLWithPath:base::apple::FilePathToNSString(alias)];
  NSURL* alias_executable_url =
      [NSURL fileURLWithPath:base::apple::FilePathToNSString(
                                 alias.Append("Contents/./MacOS/browser"))];
  NSString* error = nil;
  EXPECT_TRUE(ValidateRelaunchResult(target_path(), 123, false, alias_url,
                                     alias_executable_url, &error));
  EXPECT_EQ(error, nil);
}

TEST_F(XenonUpdateMacRelaunchTest, FailedLaunchWithoutQuarantineCanRollBack) {
  NSString* error = nil;
  bool quarantined = true;
  EXPECT_TRUE(HasUpdateQuarantine(target_path(), &quarantined, &error));
  EXPECT_FALSE(quarantined);
  EXPECT_EQ(nil, error);
  EXPECT_EQ(nil, PendingUpdateStatusAfterLaunchFailure(target_path(), &error));
}

TEST_F(XenonUpdateMacRelaunchTest,
       QuarantinedFailedLaunchLeavesBothVersionsForUserApproval) {
  const base::FilePath backup = temp_.GetPath().Append(".xenon-swap-old.app");
  ASSERT_TRUE(base::CopyDirectory(target_, backup, true));
  NSData* quarantine =
      [@"0081;12345678;Native Test;" dataUsingEncoding:NSUTF8StringEncoding];
  ASSERT_EQ(0, setxattr(target_.value().c_str(), "com.apple.quarantine",
                        quarantine.bytes, quarantine.length, 0, 0));
  NSString* error = nil;
  bool quarantined = false;
  EXPECT_TRUE(HasUpdateQuarantine(target_path(), &quarantined, &error));
  EXPECT_TRUE(quarantined);
  NSString* status =
      PendingUpdateStatusAfterLaunchFailure(target_path(), &error);
  EXPECT_TRUE([status isEqual:@"awaiting_user_approval"]);
  ASSERT_TRUE(WriteUpdateResult(
      target_path(), @"2.0", status, @"The new version has not started",
      base::apple::FilePathToNSString(backup), &error));
  NSDictionary* result = ReadUpdateResult(target_path(), &error);
  EXPECT_TRUE([result[@"status"] isEqual:@"awaiting_user_approval"]);
  EXPECT_TRUE([result[@"retained_path"]
      isEqual:base::apple::FilePathToNSString(backup)]);
  EXPECT_TRUE(base::PathExists(executable_));
  EXPECT_TRUE(base::PathExists(backup.Append("Contents/MacOS/browser")));
  EXPECT_EQ(static_cast<ssize_t>(quarantine.length),
            getxattr(target_.value().c_str(), "com.apple.quarantine", nullptr,
                     0, 0, XATTR_NOFOLLOW));
}

TEST_F(XenonUpdateMacRelaunchTest, MainExecutableQuarantineAlsoDefersRollback) {
  NSData* quarantine =
      [@"0081;12345678;Native Test;" dataUsingEncoding:NSUTF8StringEncoding];
  ASSERT_EQ(0, setxattr(executable_.value().c_str(), "com.apple.quarantine",
                        quarantine.bytes, quarantine.length, 0, 0));
  NSString* error = nil;
  EXPECT_TRUE([PendingUpdateStatusAfterLaunchFailure(target_path(), &error)
      isEqual:@"awaiting_user_approval"]);
}

TEST_F(XenonUpdateMacRelaunchTest,
       UnavailableQuarantineDefersDestructiveRollback) {
  NSString* error = nil;
  EXPECT_TRUE([PendingUpdateStatusAfterLaunchFailure(
      [target_path() stringByAppendingPathComponent:@"Missing.app"], &error)
      isEqual:@"installed_not_started"]);
  EXPECT_NE(nil, error);
}

TEST_F(XenonUpdateMacRelaunchTest,
       EligibilityFailureRecordsVersionForLaterPackageSuccess) {
  const base::FilePath staged = temp_.GetPath().Append("New.app");
  ASSERT_TRUE(base::CopyDirectory(target_, staged, true));
  NSString* plist_path =
      base::apple::FilePathToNSString(staged.Append("Contents/Info.plist"));
  NSMutableDictionary* info =
      [[NSDictionary dictionaryWithContentsOfFile:plist_path] mutableCopy];
  info[@"CFBundleShortVersionString"] = @"2.0";
  ASSERT_TRUE([info writeToFile:plist_path atomically:YES]);
  // A symlink is an ineligible installation target, but leaves a writable
  // fixture directory in which to inspect its failure journal without root.
  const base::FilePath alias = temp_.GetPath().Append("Alias.app");
  ASSERT_TRUE(base::CreateSymbolicLink(target_, alias));
  std::string error;
  EXPECT_FALSE(XenonUpdateInstaller::InstallAndRelaunch(
      staged, alias, base::FilePath(), &error));
  EXPECT_FALSE(error.empty());
  NSDictionary* result = ReadUpdateResult(target_path(), nullptr);
  ASSERT_NE(nil, result);
  EXPECT_TRUE([result[@"status"] isEqual:@"replacement_failed"]);
  EXPECT_TRUE([result[@"target_version"] isEqual:@"2.0"]);
  EXPECT_FALSE(
      XenonUpdateInstaller::ReadLastInstallError(target_, "1.0").empty());
  EXPECT_TRUE(
      XenonUpdateInstaller::ReadLastInstallError(target_, "2.0").empty());
}

TEST(XenonUpdateMacUtilTest, AtomicSwapCanRollBackWithoutDeletingEitherBundle) {
  base::ScopedTempDir temp;
  ASSERT_TRUE(temp.CreateUniqueTempDir());
  const base::FilePath target = temp.GetPath().Append("App$(false).app");
  const base::FilePath staged = temp.GetPath().Append("New.app");
  ASSERT_TRUE(base::CreateDirectory(target));
  ASSERT_TRUE(base::CreateDirectory(staged));
  ASSERT_TRUE(base::WriteFile(target.Append("old"), "old"));
  ASSERT_TRUE(base::WriteFile(staged.Append("new"), "new"));
  NSString* error = nil;
  ASSERT_TRUE(AtomicSwapAppBundles(base::apple::FilePathToNSString(staged),
                                   base::apple::FilePathToNSString(target),
                                   &error));
  EXPECT_TRUE(base::PathExists(target.Append("new")));
  EXPECT_TRUE(base::PathExists(staged.Append("old")));
  ASSERT_TRUE(AtomicSwapAppBundles(base::apple::FilePathToNSString(staged),
                                   base::apple::FilePathToNSString(target),
                                   &error));
  EXPECT_TRUE(base::PathExists(target.Append("old")));
  EXPECT_TRUE(base::PathExists(staged.Append("new")));
}

TEST(XenonUpdateMacUtilTest, PromotedSiblingSwapsAndRollsBackReadOnlyBundle) {
  base::ScopedTempDir temp;
  ASSERT_TRUE(temp.CreateUniqueTempDir());
  const base::FilePath target = temp.GetPath().Append("App.app");
  const base::FilePath prepared =
      temp.GetPath().Append("private").Append("New.app");
  const base::FilePath sibling = temp.GetPath().Append(".xenon-swap-test.app");
  ASSERT_TRUE(base::CreateDirectory(target));
  ASSERT_TRUE(base::CreateDirectory(prepared));
  ASSERT_TRUE(base::WriteFile(target.Append("old"), "old"));
  ASSERT_TRUE(base::WriteFile(prepared.Append("new"), "new"));
  ASSERT_TRUE(base::SetPosixFilePermissions(target, 0555));
  NSString* error = nil;
  const bool promoted = PromoteUpdateBundleToSibling(
      base::apple::FilePathToNSString(prepared),
      base::apple::FilePathToNSString(target),
      base::apple::FilePathToNSString(sibling), &error);
  EXPECT_TRUE(promoted);
  if (promoted) {
    EXPECT_FALSE(base::PathExists(prepared));
    const bool swapped =
        AtomicSwapAppBundles(base::apple::FilePathToNSString(sibling),
                             base::apple::FilePathToNSString(target), &error);
    EXPECT_TRUE(swapped);
    if (swapped) {
      int mode = 0;
      EXPECT_TRUE(base::GetPosixFilePermissions(sibling, &mode));
      EXPECT_EQ(0555, mode);
      EXPECT_TRUE(base::PathExists(target.Append("new")));
      EXPECT_TRUE(base::PathExists(sibling.Append("old")));
      EXPECT_TRUE(AtomicSwapAppBundles(base::apple::FilePathToNSString(sibling),
                                       base::apple::FilePathToNSString(target),
                                       &error));
      EXPECT_TRUE(base::PathExists(target.Append("old")));
      EXPECT_TRUE(base::PathExists(sibling.Append("new")));
    }
  }
  // Restore only this test's directory modes, after exercising real access
  // checks for both the commit and rollback.
  EXPECT_TRUE(base::SetPosixFilePermissions(target, 0755));
  if (base::PathExists(sibling)) {
    EXPECT_TRUE(base::SetPosixFilePermissions(sibling, 0755));
  }
}

TEST(XenonUpdateMacUtilTest, PromotionCollisionLeavesAllBundlesUnchanged) {
  base::ScopedTempDir temp;
  ASSERT_TRUE(temp.CreateUniqueTempDir());
  const base::FilePath target = temp.GetPath().Append("App.app");
  const base::FilePath prepared = temp.GetPath().Append("private/New.app");
  const base::FilePath sibling = temp.GetPath().Append(".xenon-swap-test.app");
  for (const auto& app : {target, prepared, sibling}) {
    ASSERT_TRUE(base::CreateDirectory(app));
    ASSERT_TRUE(base::WriteFile(app.Append("marker"), app.value()));
  }
  NSString* error = nil;
  EXPECT_FALSE(PromoteUpdateBundleToSibling(
      base::apple::FilePathToNSString(prepared),
      base::apple::FilePathToNSString(target),
      base::apple::FilePathToNSString(sibling), &error));
  EXPECT_NE(nil, error);
  for (const auto& app : {target, prepared, sibling}) {
    std::string contents;
    ASSERT_TRUE(base::ReadFileToString(app.Append("marker"), &contents));
    EXPECT_EQ(app.value(), contents);
  }
}

TEST(XenonUpdateMacUtilTest, PromotionRejectsSymlinkAndNonSiblingDestination) {
  base::ScopedTempDir temp;
  ASSERT_TRUE(temp.CreateUniqueTempDir());
  const base::FilePath target = temp.GetPath().Append("App.app");
  const base::FilePath prepared = temp.GetPath().Append("private/New.app");
  const base::FilePath link = temp.GetPath().Append("Link.app");
  const base::FilePath sibling = temp.GetPath().Append(".xenon-swap-test.app");
  ASSERT_TRUE(base::CreateDirectory(target));
  ASSERT_TRUE(base::CreateDirectory(prepared));
  ASSERT_TRUE(base::CreateSymbolicLink(prepared, link));
  NSString* error = nil;
  EXPECT_FALSE(PromoteUpdateBundleToSibling(
      base::apple::FilePathToNSString(link),
      base::apple::FilePathToNSString(target),
      base::apple::FilePathToNSString(sibling), &error));
  EXPECT_FALSE(PromoteUpdateBundleToSibling(
      base::apple::FilePathToNSString(prepared),
      base::apple::FilePathToNSString(target),
      base::apple::FilePathToNSString(
          prepared.DirName().Append(".xenon-swap-test.app")),
      &error));
  EXPECT_TRUE(base::DirectoryExists(target));
  EXPECT_TRUE(base::DirectoryExists(prepared));
  EXPECT_TRUE(base::IsLink(link));
  EXPECT_FALSE(base::PathExists(sibling));
}

TEST(XenonUpdateMacUtilTest, MissingReplacementDoesNotMoveInstalledBundle) {
  base::ScopedTempDir temp;
  ASSERT_TRUE(temp.CreateUniqueTempDir());
  const base::FilePath target = temp.GetPath().Append("App.app");
  ASSERT_TRUE(base::CreateDirectory(target));
  ASSERT_TRUE(base::WriteFile(target.Append("old"), "old"));
  NSString* error = nil;
  EXPECT_FALSE(AtomicSwapAppBundles(
      base::apple::FilePathToNSString(temp.GetPath().Append("Missing.app")),
      base::apple::FilePathToNSString(target), &error));
  EXPECT_TRUE(base::PathExists(target.Append("old")));
  EXPECT_NE(error, nil);
}

TEST(XenonUpdateMacUtilTest, RefusesSymlinkAndIdenticalPaths) {
  base::ScopedTempDir temp;
  ASSERT_TRUE(temp.CreateUniqueTempDir());
  const base::FilePath target = temp.GetPath().Append("App.app");
  const base::FilePath link = temp.GetPath().Append("Link.app");
  ASSERT_TRUE(base::CreateDirectory(target));
  ASSERT_TRUE(base::CreateSymbolicLink(target, link));
  NSString* error = nil;
  EXPECT_FALSE(AtomicSwapAppBundles(base::apple::FilePathToNSString(target),
                                    base::apple::FilePathToNSString(target),
                                    &error));
  EXPECT_FALSE(AtomicSwapAppBundles(base::apple::FilePathToNSString(link),
                                    base::apple::FilePathToNSString(target),
                                    &error));
  EXPECT_TRUE(base::DirectoryExists(target));
  EXPECT_TRUE(base::IsLink(link));
}

TEST(XenonUpdateMacUtilTest, UpdateResultPersistsAndSuccessReplacesFailure) {
  base::ScopedTempDir temp;
  ASSERT_TRUE(temp.CreateUniqueTempDir());
  const base::FilePath target = temp.GetPath().Append("App.app");
  ASSERT_TRUE(base::CreateDirectory(target));
  NSString* app = base::apple::FilePathToNSString(target);
  NSString* error = nil;
  EXPECT_EQ(nil, ReadUpdateResult(app, &error));
  EXPECT_EQ(nil, error);
  ASSERT_TRUE(WriteUpdateResult(app, @"2.0", @"relaunch_failed_rolled_back",
                                @"NSOSStatusErrorDomain (-10827)",
                                @"backup.app", &error));
  for (int index = 0; index < 2; ++index) {
    NSDictionary* result = ReadUpdateResult(app, &error);
    ASSERT_NE(nil, result);
    EXPECT_TRUE([result[@"target_version"] isEqual:@"2.0"]);
    EXPECT_TRUE([result[@"status"] isEqual:@"relaunch_failed_rolled_back"]);
  }
  const base::FilePath journal(target.value() + ".xenon-update-result.json");
  int permissions = 0;
  ASSERT_TRUE(base::GetPosixFilePermissions(journal, &permissions));
  EXPECT_EQ(0600, permissions);
  ASSERT_TRUE(WriteUpdateResult(app, @"2.0", @"success", @"", nil, &error));
  EXPECT_TRUE([ReadUpdateResult(app, &error)[@"status"] isEqual:@"success"]);
}

TEST(XenonUpdateMacUtilTest,
     UpdateResultRejectsLinksWithoutTouchingDestination) {
  base::ScopedTempDir temp;
  ASSERT_TRUE(temp.CreateUniqueTempDir());
  const base::FilePath target = temp.GetPath().Append("App.app");
  const base::FilePath victim = temp.GetPath().Append("private-data");
  const base::FilePath journal(target.value() + ".xenon-update-result.json");
  ASSERT_TRUE(base::CreateDirectory(target));
  ASSERT_TRUE(base::WriteFile(victim, "must remain unchanged"));
  ASSERT_TRUE(base::CreateSymbolicLink(victim, journal));
  NSString* app = base::apple::FilePathToNSString(target);
  NSString* error = nil;
  EXPECT_EQ(nil, ReadUpdateResult(app, &error));
  EXPECT_NE(nil, error);
  EXPECT_FALSE(WriteUpdateResult(app, @"2.0", @"success", @"", nil, &error));
  EXPECT_TRUE(base::IsLink(journal));
  std::string contents;
  ASSERT_TRUE(base::ReadFileToString(victim, &contents));
  EXPECT_EQ("must remain unchanged", contents);
}

TEST(XenonUpdateMacUtilTest,
     UpdateResultRejectsInvalidSchemaTargetAndPermissions) {
  base::ScopedTempDir temp;
  ASSERT_TRUE(temp.CreateUniqueTempDir());
  const base::FilePath target = temp.GetPath().Append("App.app");
  const base::FilePath journal(target.value() + ".xenon-update-result.json");
  ASSERT_TRUE(base::CreateDirectory(target));
  NSString* app = base::apple::FilePathToNSString(target);
  NSString* error = nil;
  ASSERT_TRUE(WriteUpdateResult(app, @"2.0", @"replacement_failed", @"failure",
                                nil, &error));
  NSMutableDictionary* result = [ReadUpdateResult(app, &error) mutableCopy];
  ASSERT_NE(nil, result);
  for (NSString* key in @[ @"schema_version", @"target", @"status" ]) {
    id original = result[key];
    result[key] = @"invalid";
    NSData* data = [NSJSONSerialization dataWithJSONObject:result
                                                   options:0
                                                     error:nil];
    ASSERT_TRUE(base::WriteFile(
        journal,
        std::string_view(static_cast<const char*>(data.bytes), data.length)));
    EXPECT_EQ(nil, ReadUpdateResult(app, &error));
    EXPECT_NE(nil, error);
    result[key] = original;
  }
  ASSERT_TRUE(WriteUpdateResult(app, @"2.0", @"success", @"", nil, &error));
  ASSERT_TRUE(base::SetPosixFilePermissions(journal, 0644));
  EXPECT_EQ(nil, ReadUpdateResult(app, &error));
  EXPECT_NE(nil, error);
}

}  // namespace
}  // namespace xenon::updater
