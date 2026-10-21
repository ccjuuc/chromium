// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/updater/xenon_update_installer.h"

#import <AppKit/AppKit.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <memory>

#include "base/apple/bundle_locations.h"
#include "base/apple/foundation_util.h"
#include "base/check.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_file.h"
#include "base/files/scoped_temp_dir.h"
#include "base/logging.h"
#include "base/posix/eintr_wrapper.h"
#include "base/process/launch.h"
#include "base/process/process.h"
#include "base/process/process_handle.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/sys_string_conversions.h"
#include "base/task/bind_post_task.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "base/version.h"
#include "xenon_overlay/chrome/browser/updater/xenon_update_mac_privileged_client.h"
#include "xenon_overlay/chrome/browser/updater/xenon_update_mac_privileged_util.h"
#include "xenon_overlay/chrome/browser/updater/xenon_update_mac_util.h"

namespace xenon::updater {
namespace {

NSString* const kSystemInstallerPath =
    @"/System/Library/CoreServices/Installer.app";

XenonUpdateInstaller::PrivilegedInstallResult StartPrivilegedCoordinator(
    base::FilePath staged,
    base::FilePath target,
    std::string token,
    bool relaunch,
    base::CommandLine::StringVector browser_arguments) {
  @autoreleasepool {
    using Status = XenonUpdateInstaller::PrivilegedInstallStatus;
    const base::FilePath helper =
        target.Append("Contents/Helpers").Append("xenon_update_helper");
    const std::string version =
        XenonUpdateInstaller::ReadBundleShortVersion(staged);
    int descriptors[2];
    if (pipe(descriptors) != 0) {
      return {Status::kFailed, "Could not create privileged coordinator pipe"};
    }
    base::ScopedFD reader(descriptors[0]);
    base::ScopedFD writer(descriptors[1]);
    base::CommandLine command(helper);
    command.AppendArg("--privileged");
    command.AppendArg(base::NumberToString(base::GetCurrentProcId()));
    command.AppendArg("3");
    command.AppendArg(token);
    command.AppendArgPath(target);
    command.AppendArg(version);
    command.AppendArg(relaunch ? "1" : "0");
    for (size_t index = 1; index < browser_arguments.size(); ++index) {
      command.AppendArg(browser_arguments[index]);
    }
    base::LaunchOptions options;
    options.new_process_group = true;
    options.fds_to_remap.emplace_back(writer.get(), 3);
    base::Process process = base::LaunchProcess(command, options);
    writer.reset();
    if (!process.IsValid()) {
      return {Status::kFailed,
              "Could not launch privileged update coordinator"};
    }
    struct pollfd descriptor = {reader.get(), POLLIN, 0};
    const base::TimeTicks deadline = base::TimeTicks::Now() + base::Seconds(30);
    int result;
    do {
      result =
          poll(&descriptor, 1,
               std::max<int64_t>(
                   0, (deadline - base::TimeTicks::Now()).InMilliseconds()));
    } while (result < 0 && errno == EINTR && base::TimeTicks::Now() < deadline);
    char response = 0;
    if (result <= 0 || !(descriptor.revents & POLLIN) ||
        HANDLE_EINTR(read(reader.get(), &response, 1)) != 1 ||
        response != 'R') {
      process.Terminate(/*exit_code=*/1, /*wait=*/true);
      return {Status::kFailed,
              "Privileged update coordinator did not become ready"};
    }
    return {Status::kReadyToQuit, ""};
  }
}

}  // namespace

bool XenonUpdateInstaller::CanInstallDifferentially(
    const base::FilePath& target,
    std::string* error) {
  if (error) {
    error->clear();
  }
  @autoreleasepool {
    NSString* reason = nil;
    NSString* app = base::apple::FilePathToNSString(target);
    bool allowed = !UpdateRequiresElevation(app) &&
                   CanInstallOwnedUpdateBundle(app, &reason);
    if (!allowed) {
      if (@available(macOS 13.0, *)) {
        allowed = CanUsePrivilegedUpdateForTarget(app, &reason);
      }
    }
    if (!allowed && error) {
      *error = base::SysNSStringToUTF8(reason);
    }
    return allowed;
  }
}

bool XenonUpdateInstaller::RequiresElevation(const base::FilePath& target) {
  @autoreleasepool {
    return UpdateRequiresElevation(base::apple::FilePathToNSString(target));
  }
}

void XenonUpdateInstaller::BeginPrivilegedInstallAndRelaunch(
    const base::FilePath& staged,
    const base::FilePath& target,
    const base::FilePath& relaunch,
    base::OnceCallback<void(PrivilegedInstallResult)> completion) {
  DCHECK([NSThread isMainThread]);
  auto reply =
      std::make_shared<base::OnceCallback<void(PrivilegedInstallResult)>>(
          base::BindPostTaskToCurrentDefault(std::move(completion)));
  // Objective-C blocks preserve C++ reference captures as references. Own all
  // paths used after this UI call returns, before handing work to Cocoa.
  const base::FilePath staged_copy = staged;
  const base::FilePath target_copy = target;
  const bool should_relaunch = !relaunch.empty();
  const auto browser_arguments = base::CommandLine::ForCurrentProcess()->argv();
  BeginPrivilegedUpdatePreparation(
      base::apple::FilePathToNSString(staged),
      base::apple::FilePathToNSString(target),
      ^(NSString* token, bool approval, NSString* error) {
        if (!token.length) {
          std::move(*reply).Run(
              {approval ? PrivilegedInstallStatus::kRequiresApproval
                        : PrivilegedInstallStatus::kFailed,
               base::SysNSStringToUTF8(
                   error ?: @"Privileged preparation failed")});
          return;
        }
        base::ThreadPool::PostTaskAndReplyWithResult(
            FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_BLOCKING},
            base::BindOnce(&StartPrivilegedCoordinator, staged_copy,
                           target_copy, base::SysNSStringToUTF8(token),
                           should_relaunch, browser_arguments),
            base::BindOnce(
                [](decltype(reply) reply, base::FilePath target,
                   std::string token, PrivilegedInstallResult result) {
                  if (result.status == PrivilegedInstallStatus::kFailed) {
                    PerformPrivilegedUpdateOperation(
                        base::apple::FilePathToNSString(target),
                        base::SysUTF8ToNSString(token),
                        PrivilegedUpdateOperation::kFinish, false,
                        ^(NSString*, NSString* cleanup_error) {
                          if (cleanup_error) {
                            NSLog(@"Privileged update cleanup: %@",
                                  cleanup_error);
                          }
                        });
                  }
                  std::move(*reply).Run(std::move(result));
                },
                reply, target_copy, base::SysNSStringToUTF8(token)));
      });
}

XenonUpdateInstaller::InstallerPackagePreparation
XenonUpdateInstaller::PrepareInstallerPackage(
    const base::FilePath& source,
    const std::string& expected_sha256,
    const base::FilePath& cache_root_override) {
  InstallerPackagePreparation result;
  @autoreleasepool {
    NSBundle* installer = [NSBundle bundleWithPath:kSystemInstallerPath];
    if (![installer.bundleIdentifier isEqualToString:@"com.apple.installer"]) {
      result.error = "The system Installer application is unavailable";
      return result;
    }
    if (!cache_root_override.empty() && !cache_root_override.IsAbsolute()) {
      result.error = "Installer package cache path must be absolute";
      return result;
    }
    NSString* cache =
        cache_root_override.empty()
            ? NSSearchPathForDirectoriesInDomains(NSCachesDirectory,
                                                  NSUserDomainMask, YES)
                  .firstObject
            : base::apple::FilePathToNSString(cache_root_override);
    NSError* directory_error = nil;
    if (!cache || ![[NSFileManager defaultManager]
                            createDirectoryAtPath:cache
                      withIntermediateDirectories:YES
                                       attributes:@{
                                         NSFilePosixPermissions : @0700
                                       }
                                            error:&directory_error]) {
      result.error = "Could not prepare the user installer package cache";
      return result;
    }
    NSString* failure = nil;
    NSString* prepared = PrepareInstallerPackageInCache(
        base::apple::FilePathToNSString(source),
        base::SysUTF8ToNSString(expected_sha256), cache, &failure);
    if (!prepared) {
      result.error = base::SysNSStringToUTF8(failure);
      return result;
    }
    result.path = base::apple::NSStringToFilePath(prepared);
    return result;
  }
}

void XenonUpdateInstaller::OpenInstallerPackage(
    const base::FilePath& package,
    base::OnceCallback<void(bool, std::string)> completion) {
  DCHECK([NSThread isMainThread]);
  auto reply = std::make_shared<base::OnceCallback<void(bool, std::string)>>(
      base::BindPostTaskToCurrentDefault(std::move(completion)));
  if (!package.IsAbsolute() ||
      !package.MatchesExtension(FILE_PATH_LITERAL(".pkg"))) {
    std::move(*reply).Run(false, "Installer package path is invalid");
    return;
  }
  @autoreleasepool {
    NSWorkspaceOpenConfiguration* configuration =
        [NSWorkspaceOpenConfiguration configuration];
    configuration.promptsUserIfNeeded = YES;
    [[NSWorkspace sharedWorkspace]
                    openURLs:@[
                      [NSURL fileURLWithPath:base::apple::FilePathToNSString(
                                                 package)]
                    ]
        withApplicationAtURL:[NSURL fileURLWithPath:kSystemInstallerPath]
               configuration:configuration
           completionHandler:^(NSRunningApplication* application,
                               NSError* error) {
             const bool opened = !error && application &&
                                 application.processIdentifier > 0 &&
                                 !application.terminated &&
                                 [application.bundleIdentifier
                                     isEqualToString:@"com.apple.installer"] &&
                                 [application.bundleURL.path
                                     isEqualToString:kSystemInstallerPath];
             std::string message;
             if (!opened) {
               NSString* detail =
                   error ? [NSString
                               stringWithFormat:@"System Installer could "
                                                @"not open the package: %@",
                                                DescribeUpdateNSError(error)]
                         : @"System Installer did not accept the package";
               message = base::SysNSStringToUTF8(detail);
             }
             // NSWorkspace completes on an arbitrary queue. BindPostTask
             // returns the handoff result to the caller's UI sequence without
             // waiting.
             std::move(*reply).Run(opened, std::move(message));
           }];
  }
}

std::string XenonUpdateInstaller::ReadBundleShortVersion(
    const base::FilePath& bundle_path) {
  if (bundle_path.empty()) {
    return std::string();
  }
  base::FilePath plist_path =
      bundle_path.Append("Contents").Append("Info.plist");
  @autoreleasepool {
    NSDictionary* info = [NSDictionary
        dictionaryWithContentsOfFile:base::apple::FilePathToNSString(
                                         plist_path)];
    NSString* version =
        base::apple::ObjCCast<NSString>(info[@"CFBundleShortVersionString"]);
    if (version.length == 0) {
      version = base::apple::ObjCCast<NSString>(info[@"CFBundleVersion"]);
    }
    return base::SysNSStringToUTF8(version);
  }
}

bool XenonUpdateInstaller::InstallAndRelaunch(
    const base::FilePath& staged_path,
    const base::FilePath& target_install_dir,
    const base::FilePath& relaunch_executable,
    std::string* error) {
  if (error) {
    error->clear();
  }
  @autoreleasepool {
    base::FilePath target_app = target_install_dir;
    if (target_app.empty() ||
        target_app.Extension() != FILE_PATH_LITERAL(".app")) {
      target_app = base::apple::OuterBundlePath();
    }
    NSString* target = base::apple::FilePathToNSString(target_app);
    NSString* attempted_version = @"";
    const NSTimeInterval attempt_started = [NSDate date].timeIntervalSince1970;
    auto fail_install = [&](NSString* message, NSString* status) {
      LOG(ERROR) << "[XenonUpdateInstaller] "
                 << base::SysNSStringToUTF8(message);
      if (error) {
        *error = base::SysNSStringToUTF8(message);
      }
      NSString* write_error = nil;
      if (!WriteUpdateResult(target, attempted_version, status, message, @"",
                             &write_error)) {
        LOG(WARNING) << "[XenonUpdateInstaller] "
                     << base::SysNSStringToUTF8(write_error);
      }
      return false;
    };
    base::FilePath staged_app = staged_path;
    if (staged_app.Extension() != FILE_PATH_LITERAL(".app") &&
        base::DirectoryExists(staged_app)) {
      base::FileEnumerator enumerator(staged_app, false,
                                      base::FileEnumerator::DIRECTORIES,
                                      FILE_PATH_LITERAL("*.app"));
      staged_app = enumerator.Next();
      if (!enumerator.Next().empty()) {
        return fail_install(@"Multiple update application bundles",
                            @"validation_failed");
      }
    }

    attempted_version =
        base::SysUTF8ToNSString(ReadBundleShortVersion(staged_app));
    // Record the attempted version even if ownership changed after download,
    // so a later successful PKG installation supersedes this failure journal.
    NSString* eligibility_error =
        @"The application tree requires an authorized privileged update";
    if (UpdateRequiresElevation(target) ||
        !CanInstallOwnedUpdateBundle(target, &eligibility_error)) {
      return fail_install(eligibility_error, @"replacement_failed");
    }
    NSString* staged = base::apple::FilePathToNSString(staged_app);
    NSString* validation_error = nil;
    if (!ValidateUpdateBundles(staged, target, &validation_error)) {
      return fail_install(validation_error, @"validation_failed");
    }

    // Copy before shutdown into a private directory on the installation volume.
    // NSFileManager preserves executable modes and framework symlinks. The
    // running app is untouched if copying or verification fails (including
    // ENOSPC).
    base::ScopedTempDir preparation;
    if (!preparation.CreateUniqueTempDirUnderPath(target_app.DirName(),
                                                  ".xenon-update-")) {
      return fail_install(@"Could not create the update preparation directory; "
                          @"check free disk space and write permissions",
                          @"validation_failed");
    }
    const base::FilePath prepared_app =
        preparation.GetPath().Append(target_app.BaseName());
    NSString* prepared = base::apple::FilePathToNSString(prepared_app);
    NSError* copy_error = nil;
    if (![[NSFileManager defaultManager] copyItemAtPath:staged
                                                 toPath:prepared
                                                  error:&copy_error] ||
        !ValidateUpdateBundles(prepared, target, &validation_error)) {
      NSString* message =
          copy_error
              ? [NSString stringWithFormat:@"Could not copy update (%@:%ld)",
                                           copy_error.domain,
                                           static_cast<long>(copy_error.code)]
              : validation_error;
      return fail_install(message, @"validation_failed");
    }

    const base::FilePath helper = target_app.Append("Contents")
                                      .Append("Helpers")
                                      .Append("xenon_update_helper");
    if (access(helper.value().c_str(), X_OK) != 0) {
      return fail_install(@"Native update helper is missing or not executable",
                          @"handshake_failed");
    }
    int pipe_fds[2];
    if (pipe(pipe_fds) != 0) {
      return fail_install(
          @"Could not create native update helper readiness pipe",
          @"handshake_failed");
    }
    base::ScopedFD read_fd(pipe_fds[0]);
    base::ScopedFD write_fd(pipe_fds[1]);
    constexpr int kReadyFd = 3;
    base::CommandLine command(helper);
    command.AppendArg(base::NumberToString(base::GetCurrentProcId()));
    command.AppendArg(base::NumberToString(kReadyFd));
    command.AppendArgPath(prepared_app);
    command.AppendArgPath(target_app);
    command.AppendArg(relaunch_executable.empty() ? "0" : "1");
    const auto& browser_arguments =
        base::CommandLine::ForCurrentProcess()->argv();
    for (size_t index = 1; index < browser_arguments.size(); ++index) {
      command.AppendArg(browser_arguments[index]);
    }
    base::LaunchOptions options;
    options.new_process_group = true;
    options.fds_to_remap.emplace_back(write_fd.get(), kReadyFd);
    base::Process process = base::LaunchProcess(command, options);
    write_fd.reset();
    if (!process.IsValid()) {
      return fail_install(@"Could not start native update helper",
                          @"handshake_failed");
    }

    // A successful launch alone is not readiness. The helper acknowledges only
    // after taking its lock, verifying the bundles and subscribing to
    // NOTE_EXIT.
    struct pollfd descriptor = {read_fd.get(), POLLIN, 0};
    const base::TimeTicks deadline = base::TimeTicks::Now() + base::Seconds(30);
    int poll_result;
    do {
      poll_result =
          poll(&descriptor, 1,
               std::max<int64_t>(
                   0, (deadline - base::TimeTicks::Now()).InMilliseconds()));
    } while (poll_result < 0 && errno == EINTR &&
             base::TimeTicks::Now() < deadline);
    char response = 0;
    const bool ready = poll_result > 0 && (descriptor.revents & POLLIN) &&
                       HANDLE_EINTR(read(read_fd.get(), &response, 1)) == 1 &&
                       response == 'R';
    if (!ready) {
      LOG(ERROR)
          << "[XenonUpdateInstaller] Native update helper did not become ready";
      // Stop and reap it before deleting its input directory. In particular a
      // failed handshake must never leave a helper that can later swap the app.
      process.Terminate(/*exit_code=*/1, /*wait=*/true);
      NSDictionary* result = ReadUpdateResult(target, nullptr);
      if (result && [result[@"timestamp"] doubleValue] >= attempt_started) {
        if (error) {
          *error = base::SysNSStringToUTF8(result[@"message"]);
        }
        return false;
      }
      return fail_install(@"Native update helper did not become ready",
                          @"handshake_failed");
    }

    // The helper now owns cleanup. The caller can request normal browser exit;
    // no application files are changed until the original process has exited.
    preparation.Take();
    return true;
  }
}

std::string XenonUpdateInstaller::ReadLastInstallError(
    const base::FilePath& bundle_path,
    const std::string& current_version) {
  @autoreleasepool {
    NSString* target = base::apple::FilePathToNSString(bundle_path);
    NSDictionary* result = ReadPrivilegedUpdateResult(target, nullptr);
    const bool privileged_result = result != nil;
    if (!result) {
      result = ReadUpdateResult(target, nullptr);
    }
    if (!result || [result[@"status"] isEqualToString:@"success"]) {
      return {};
    }
    const std::string target_version =
        base::SysNSStringToUTF8(result[@"target_version"]);
    const base::Version recorded_version(target_version);
    const base::Version running_version(current_version);
    const bool pending =
        [result[@"status"] isEqualToString:@"launching"] ||
        [result[@"status"] isEqualToString:@"installed_pending_launch"] ||
        [result[@"status"] isEqualToString:@"awaiting_user_approval"] ||
        [result[@"status"] isEqualToString:@"installed_not_started"];
    if (pending && recorded_version.IsValid() && running_version.IsValid() &&
        recorded_version == running_version) {
      // This runs from the installed browser's startup path. Only now can a
      // previously blocked or deferred launch be recorded as successful.
      if (privileged_result) {
        WritePrivilegedUpdateResult(target, result[@"target_version"],
                                    @"success", @"", nullptr);
      } else {
        WriteUpdateResult(target, result[@"target_version"], @"success", @"",
                          result[@"retained_path"], nullptr);
      }
      return {};
    }
    if (pending) {
      if (recorded_version.IsValid() && running_version.IsValid() &&
          recorded_version < running_version) {
        return {};
      }
      return "更新至 " + target_version + " 尚未完成启动：" +
             base::SysNSStringToUTF8(result[@"message"]);
    }
    if (recorded_version.IsValid() && running_version.IsValid() &&
        recorded_version <= running_version &&
        ![result[@"status"]
            isEqualToString:@"relaunch_failed_rollback_failed"]) {
      return {};
    }
    return "上次更新至 " + target_version + " 未完成：" +
           base::SysNSStringToUTF8(result[@"message"]);
  }
}

}  // namespace xenon::updater
