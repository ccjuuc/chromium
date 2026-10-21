// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import <AppKit/AppKit.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/event.h>
#include <sys/file.h>
#include <unistd.h>

#include "xenon_overlay/chrome/browser/updater/xenon_update_mac_privileged_client.h"
#include "xenon_overlay/chrome/browser/updater/xenon_update_mac_privileged_util.h"
#include "xenon_overlay/chrome/browser/updater/xenon_update_mac_util.h"

namespace {

NSString* DescribeLaunchError(NSError* error) {
  NSMutableArray<NSString*>* parts = [NSMutableArray array];
  // Store stable domain/code information without copying userInfo, which can
  // contain unrelated URLs or other private data.
  for (NSUInteger depth = 0; error && depth < 4; ++depth) {
    [parts addObject:[NSString stringWithFormat:@"%@ (%ld)", error.domain,
                                                static_cast<long>(error.code)]];
    id underlying = error.userInfo[NSUnderlyingErrorKey];
    error = [underlying isKindOfClass:[NSError class]] ? underlying : nil;
  }
  return [parts componentsJoinedByString:@"; caused by "];
}

bool RelaunchApplication(NSString* target,
                         NSArray<NSString*>* arguments,
                         NSString** failure = nullptr) {
  __block bool finished = false;
  __block bool succeeded = false;
  __block NSString* launch_error = nil;
  NSWorkspaceOpenConfiguration* configuration =
      [NSWorkspaceOpenConfiguration configuration];
  configuration.createsNewApplicationInstance = YES;
  configuration.arguments = arguments;
  [[NSWorkspace sharedWorkspace]
      openApplicationAtURL:[NSURL fileURLWithPath:target]
             configuration:configuration
         completionHandler:^(NSRunningApplication* app, NSError* error) {
           // Completion may run on an arbitrary queue; mutate run-loop state
           // only on the main queue used below.
           dispatch_async(dispatch_get_main_queue(), ^{
             NSString* validation_error = nil;
             succeeded =
                 error == nil && xenon::updater::ValidateRelaunchResult(
                                     target, app ? app.processIdentifier : -1,
                                     app ? app.terminated : true, app.bundleURL,
                                     app.executableURL, &validation_error);
             finished = true;
             if (!succeeded) {
               launch_error =
                   error ? DescribeLaunchError(error) : validation_error;
               NSLog(@"Xenon update relaunch failed: %@", launch_error);
             }
           });
         }];
  while (!finished) {
    [[NSRunLoop currentRunLoop]
        runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
  }
  if (!succeeded && failure) {
    *failure =
        launch_error ?: @"LaunchServices did not return a live application";
  }
  return succeeded;
}

bool ParsePositiveInt(NSString* input, int* value) {
  NSScanner* scanner = [NSScanner scannerWithString:input];
  scanner.charactersToBeSkipped = nil;
  long long parsed = 0;
  if (![scanner scanLongLong:&parsed] || !scanner.isAtEnd || parsed <= 0 ||
      parsed > INT_MAX) {
    return false;
  }
  *value = static_cast<int>(parsed);
  return true;
}

NSString* PrivilegedRequest(NSString* target,
                            NSString* token,
                            xenon::updater::PrivilegedUpdateOperation operation,
                            bool preserve_backup,
                            NSString** error) {
  __block bool finished = false;
  __block NSString* result = nil;
  __block NSString* failure = nil;
  xenon::updater::PerformPrivilegedUpdateOperation(
      target, token, operation, preserve_backup,
      ^(NSString* value, NSString* message) {
        result = value;
        failure = message;
        finished = true;
      });
  // The client enforces an operation-specific deadline and always replies on
  // this main queue. Pumping the run loop also permits NSXPC progress.
  while (!finished) {
    [[NSRunLoop currentRunLoop]
        runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
  }
  if (error) {
    *error = failure;
  }
  return result;
}

int RunPrivileged(NSArray<NSString*>* arguments) {
  using Operation = xenon::updater::PrivilegedUpdateOperation;
  int parent_pid = 0;
  int ready_fd = 0;
  // --privileged browserPID readyFD token target targetVersion relaunch [args]
  if (geteuid() == 0 || arguments.count < 8 ||
      !ParsePositiveInt(arguments[2], &parent_pid) || parent_pid != getppid() ||
      !ParsePositiveInt(arguments[3], &ready_fd) ||
      ![[NSUUID alloc] initWithUUIDString:arguments[4]] ||
      (![arguments[7] isEqualToString:@"0"] && ![arguments[7]
                                                   isEqualToString:@"1"])) {
    return 2;
  }
  NSString* token = arguments[4];
  NSString* target = arguments[5];
  NSString* version = arguments[6];
  const bool relaunch = [arguments[7] isEqualToString:@"1"];
  NSArray<NSString*>* browser_arguments =
      [arguments subarrayWithRange:NSMakeRange(8, arguments.count - 8)];
  char executable[PATH_MAX];
  uint32_t executable_size = sizeof(executable);
  NSString* error = nil;
  NSString* own_path =
      _NSGetExecutablePath(executable, &executable_size) == 0
          ? [NSString stringWithUTF8String:executable].stringByStandardizingPath
          : nil;
  NSString* daemon_path = [own_path.stringByDeletingLastPathComponent
      stringByAppendingPathComponent:@"xenon_privileged_update_helper"];
  NSString* expected_target =
      [own_path.lastPathComponent isEqual:@"xenon_update_helper"]
          ? xenon::updater::PrivilegedTargetFromExecutable(daemon_path, &error)
          : nil;
  if (![target isEqualToString:expected_target] || version.length == 0 ||
      !xenon::updater::CanUsePrivilegedUpdateForTarget(target, &error)) {
    NSLog(@"Privileged coordinator rejected: %@", error ?: @"Target mismatch");
    return 5;
  }
  auto record = [&](NSString* status, NSString* message) {
    NSString* failure = nil;
    if (!xenon::updater::WritePrivilegedUpdateResult(
            target, version, status, message ?: @"", &failure)) {
      NSLog(@"Could not persist privileged update result: %@", failure);
    }
  };
  auto finish = [&](bool preserve) {
    NSString* failure = nil;
    const bool success = PrivilegedRequest(target, token, Operation::kFinish,
                                           preserve, &failure) != nil;
    if (!success) {
      NSLog(@"Privileged update finalization failed: %@", failure);
    }
    return success;
  };
  auto finish_if_uncommitted = [&](NSString* state) {
    if ([state isEqualToString:@"prepared"] ||
        [state isEqualToString:@"ready"] ||
        [state isEqualToString:@"rolled_back"]) {
      finish(false);
    }
  };
  const int queue = kqueue();
  struct kevent change;
  EV_SET(&change, parent_pid, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0,
         nullptr);
  if (queue < 0 || kevent(queue, &change, 1, nullptr, 0, nullptr) != 0) {
    if (queue >= 0) {
      close(queue);
    }
    finish_if_uncommitted(
        PrivilegedRequest(target, token, Operation::kState, false, nullptr));
    record(@"watch_failed", @"Could not watch the browser process");
    return 4;
  }
  NSString* state =
      PrivilegedRequest(target, token, Operation::kState, false, &error);
  if (![state isEqualToString:@"prepared"]) {
    close(queue);
    finish_if_uncommitted(state);
    record(@"validation_failed",
           error ?: @"Privileged transaction is not prepared");
    return 5;
  }
  signal(SIGPIPE, SIG_IGN);
  const char ready = 'R';
  ssize_t written;
  do {
    written = write(ready_fd, &ready, 1);
  } while (written < 0 && errno == EINTR);
  close(ready_fd);
  if (written != 1) {
    close(queue);
    finish_if_uncommitted(
        PrivilegedRequest(target, token, Operation::kState, false, nullptr));
    record(@"handshake_failed", @"Browser readiness handshake failed");
    return 6;
  }
  struct kevent event;
  int count;
  do {
    count = kevent(queue, nullptr, 0, &event, 1, nullptr);
  } while (count < 0 && errno == EINTR);
  close(queue);
  if (count != 1 || (event.flags & EV_ERROR) || !(event.fflags & NOTE_EXIT)) {
    finish_if_uncommitted(
        PrivilegedRequest(target, token, Operation::kState, false, nullptr));
    record(@"wait_failed", @"Could not confirm the original browser exited");
    return 7;
  }
  // The daemon has its own NOTE_EXIT subscription. Allow its queue to observe
  // that event before requesting commit; never loosen its state requirement.
  NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:15];
  do {
    state = PrivilegedRequest(target, token, Operation::kState, false, &error);
    if (![state isEqualToString:@"prepared"]) {
      break;
    }
    [[NSRunLoop currentRunLoop]
        runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
  } while ([deadline timeIntervalSinceNow] > 0);
  if (![state isEqualToString:@"ready"]) {
    record(@"replacement_failed",
           error ?: @"Privileged service did not observe browser exit");
    finish_if_uncommitted(state);
    if (relaunch) {
      RelaunchApplication(target, browser_arguments);
    }
    return 8;
  }
  if (!PrivilegedRequest(target, token, Operation::kCommit, false, &error)) {
    // A transport timeout is ambiguous. Query the transaction instead of
    // assuming that a mutating RPC never ran or issuing the swap a second time.
    NSString* state_error = nil;
    state = PrivilegedRequest(target, token, Operation::kState, false,
                              &state_error);
    if (![state isEqualToString:@"committed"]) {
      record(@"replacement_failed", error ?: state_error ?: @"Privileged replacement failed");
      finish_if_uncommitted(state);
      if (relaunch && ([state isEqualToString:@"ready"] ||
                       [state isEqualToString:@"rolled_back"])) {
        RelaunchApplication(target, browser_arguments);
      }
      return 8;
    }
  }
  if (!relaunch) {
    record(@"installed_pending_launch",
           @"Replacement installed; previous version retained");
    finish(true);
    return 0;
  }
  record(@"launching", @"");
  NSString* launch_error = nil;
  if (RelaunchApplication(target, browser_arguments, &launch_error)) {
    const bool cleaned = finish(false);
    record(@"success",
           cleaned ? @""
                   : @"Installed; previous version cleanup was not confirmed");
    return 0;
  }
  NSString* quarantine_error = nil;
  NSString* pending = xenon::updater::PendingUpdateStatusAfterLaunchFailure(
      target, &quarantine_error);
  if (pending) {
    record(pending,
           [NSString stringWithFormat:@"The new version has not started. "
                                      @"Review any macOS security prompt "
                                      @"and open the application again. %@ %@",
                                      launch_error, quarantine_error ?: @""]);
    finish(true);
    return 12;
  }
  if (!PrivilegedRequest(target, token, Operation::kRollback, false, &error)) {
    record(@"relaunch_failed_rollback_failed",
           [NSString stringWithFormat:@"%@; rollback: %@", launch_error,
                                      error ?: @"not confirmed"]);
    return 9;
  }
  finish(false);
  record(@"relaunch_failed_rolled_back", launch_error);
  RelaunchApplication(target, browser_arguments);
  return 10;
}

int Run() {
  // Positional arguments are passed as argv, never interpreted by a shell.
  // The staged bundle is inside a private, same-volume preparation directory.
  int parent_pid = 0;
  int ready_fd = 0;
  NSArray<NSString*>* arguments = NSProcessInfo.processInfo.arguments;
  if (arguments.count > 1 && [arguments[1] isEqualToString:@"--privileged"]) {
    return RunPrivileged(arguments);
  }
  if (arguments.count < 6 || !ParsePositiveInt(arguments[1], &parent_pid) ||
      !ParsePositiveInt(arguments[2], &ready_fd) ||
      (![arguments[5] isEqualToString:@"0"] && ![arguments[5]
                                                   isEqualToString:@"1"])) {
    return 2;
  }
  NSString* staged = arguments[3];
  NSString* target = arguments[4];
  const bool relaunch = [arguments[5] isEqualToString:@"1"];
  NSArray<NSString*>* browser_arguments =
      [arguments subarrayWithRange:NSMakeRange(6, arguments.count - 6)];

  // Keep the lock file in place: unlinking it would allow another helper to
  // lock a different inode while this helper is still committing the update.
  NSString* lock_path = [target stringByAppendingString:@".xenon-update.lock"];
  const int lock_fd = open(lock_path.fileSystemRepresentation,
                           O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (lock_fd < 0 || flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
    NSLog(
        @"Another Xenon update is running or the installation is not writable");
    return 3;
  }
  NSDictionary* staged_info = [NSDictionary
      dictionaryWithContentsOfFile:
          [staged stringByAppendingPathComponent:@"Contents/Info.plist"]];
  id version = staged_info[@"CFBundleShortVersionString"];
  if (![version isKindOfClass:[NSString class]]) {
    version = staged_info[@"CFBundleVersion"];
  }
  NSString* target_version =
      [version isKindOfClass:[NSString class]] ? version : @"";
  auto record_result = [&](NSString* status, NSString* message,
                           NSString* retained_path) {
    NSString* journal_error = nil;
    if (!xenon::updater::WriteUpdateResult(target, target_version, status,
                                           message ?: @"", retained_path,
                                           &journal_error)) {
      NSLog(@"Could not persist Xenon update result: %@", journal_error);
    }
  };

  // Register NOTE_EXIT before acknowledging readiness. Unlike polling a PID,
  // kqueue follows the original process even if its PID is later reused.
  const int queue = kqueue();
  struct kevent change;
  EV_SET(&change, parent_pid, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0,
         nullptr);
  if (queue < 0 || kevent(queue, &change, 1, nullptr, 0, nullptr) != 0) {
    NSLog(@"Could not watch the browser process: %s", strerror(errno));
    record_result(@"watch_failed", @"Could not watch the browser process",
                  staged);
    return 4;
  }
  NSString* error = @"Application tree requires a privileged update";
  if (xenon::updater::UpdateRequiresElevation(target) ||
      !xenon::updater::ValidateUpdateBundles(staged, target, &error)) {
    NSLog(@"Xenon update rejected: %@", error);
    record_result(@"validation_failed", error, staged);
    return 5;
  }
  signal(SIGPIPE, SIG_IGN);
  const char ready = 'R';
  ssize_t written;
  do {
    written = write(ready_fd, &ready, 1);
  } while (written < 0 && errno == EINTR);
  close(ready_fd);
  if (written != 1) {
    record_result(@"handshake_failed", @"Browser readiness handshake failed",
                  staged);
    return 6;
  }

  struct kevent event;
  int count;
  do {
    count = kevent(queue, nullptr, 0, &event, 1, nullptr);
  } while (count < 0 && errno == EINTR);
  close(queue);
  if (count != 1 || (event.flags & EV_ERROR) || !(event.fflags & NOTE_EXIT)) {
    NSLog(@"Failed while waiting for the browser to exit");
    record_result(@"wait_failed",
                  @"Failed while waiting for the browser to exit", staged);
    return 7;
  }

  // Revalidate immediately before committing: preparation may precede browser
  // shutdown by a long time. Failure leaves the installed application intact.
  NSString* original_parent = staged.stringByDeletingLastPathComponent;
  NSString* target_parent = target.stringByDeletingLastPathComponent;
  NSString* temporary_name =
      [NSString stringWithFormat:@".xenon-swap-%@.app", NSUUID.UUID.UUIDString];
  NSString* swap_path =
      [target_parent stringByAppendingPathComponent:temporary_name];
  if (xenon::updater::UpdateRequiresElevation(target) ||
      !xenon::updater::CanInstallOwnedUpdateBundle(target, &error) ||
      !xenon::updater::ValidateUpdateBundles(staged, target, &error) ||
      !xenon::updater::PromoteUpdateBundleToSibling(staged, target, swap_path,
                                                    &error)) {
    NSLog(@"Xenon update failed; installed application retained: %@", error);
    record_result(@"replacement_failed", error, staged);
    if (relaunch) {
      RelaunchApplication(target, browser_arguments);
    }
    return 8;
  }
  // Remove only the original, now-empty private preparation directory. Never
  // derive this cleanup path from the promoted sibling beside the installed
  // app.
  if (![original_parent isEqualToString:target_parent]) {
    rmdir(original_parent.fileSystemRepresentation);
  }
  // The installed bundle can be root-owned and non-writable by this user.
  // Keeping both paths in one parent avoids changing its '..' directory entry.
  // Recheck the signature after promotion, before touching the installed app.
  if (xenon::updater::UpdateRequiresElevation(target) ||
      !xenon::updater::ValidateUpdateBundles(swap_path, target, &error) ||
      !xenon::updater::CanInstallOwnedUpdateBundle(target, &error) ||
      !xenon::updater::AtomicSwapAppBundles(swap_path, target, &error)) {
    NSLog(@"Xenon update failed; installed application retained, prepared "
          @"application retained at %@: %@",
          swap_path, error);
    record_result(@"replacement_failed", error, swap_path);
    if (relaunch) {
      RelaunchApplication(target, browser_arguments);
    }
    return 8;
  }

  // A newly launched browser may read this journal before NSWorkspace delivers
  // its completion. Supersede an older failure without claiming success early.
  if (!relaunch) {
    record_result(@"installed_pending_launch", @"", swap_path);
    NSLog(@"Replacement installed; previous application retained until the "
          @"new version starts: %@",
          swap_path);
    return 0;
  }
  record_result(@"launching", @"", swap_path);
  NSString* launch_error = nil;
  if (!RelaunchApplication(target, browser_arguments, &launch_error)) {
    NSString* quarantine_error = nil;
    NSString* pending_status =
        xenon::updater::PendingUpdateStatusAfterLaunchFailure(
            target, &quarantine_error);
    if (pending_status) {
      NSString* message = @"The new version has not started. Check any macOS "
                          @"security prompt, allow the app if you trust it, "
                          @"then open the application again.";
      message =
          [message stringByAppendingFormat:@" Launch result: %@", launch_error];
      if (quarantine_error) {
        message = [message stringByAppendingFormat:@" %@", quarantine_error];
      }
      record_result(pending_status, message, swap_path);
      NSLog(@"Xenon update is awaiting a successful launch; new application "
            @"retained at %@, previous application retained at %@",
            target, swap_path);
      return 12;
    }
    if (!xenon::updater::AtomicSwapAppBundles(swap_path, target, &error)) {
      NSLog(@"Xenon update rollback failed; previous application retained at "
            @"%@: %@",
            swap_path, error);
      record_result(
          @"relaunch_failed_rollback_failed",
          [NSString stringWithFormat:@"%@; rollback: %@", launch_error, error],
          swap_path);
      return 9;
    }
    NSLog(@"Xenon update rolled back; replacement retained at %@", swap_path);
    record_result(@"relaunch_failed_rolled_back", launch_error, swap_path);
    RelaunchApplication(target, browser_arguments);
    return 10;
  }

  // After the atomic swap, swap_path contains the previous application. It is
  // deleted only once replacement (and the requested launch) has succeeded.
  NSError* cleanup_error = nil;
  if (![[NSFileManager defaultManager] removeItemAtPath:swap_path
                                                  error:&cleanup_error]) {
    NSLog(@"Replacement installed; previous application retained at %@: %@",
          swap_path, cleanup_error);
  }
  record_result(@"success",
                cleanup_error
                    ? @"Replacement installed; previous application retained"
                    : @"",
                cleanup_error ? swap_path : nil);
  close(lock_fd);
  return 0;
}

}  // namespace

int main() {
  @autoreleasepool {
    return Run();
  }
}
