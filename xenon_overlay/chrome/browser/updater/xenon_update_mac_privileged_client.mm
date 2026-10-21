// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/updater/xenon_update_mac_privileged_client.h"

#include <CommonCrypto/CommonDigest.h>
#import <ServiceManagement/ServiceManagement.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "xenon_overlay/chrome/browser/updater/xenon_update_mac_privileged_generation.h"
#include "xenon_overlay/chrome/browser/updater/xenon_update_mac_privileged_protocol.h"
#include "xenon_overlay/chrome/browser/updater/xenon_update_mac_privileged_util.h"

namespace xenon::updater {
namespace {

NSString* DaemonPath(NSString* target) {
  return [[target stringByAppendingPathComponent:@"Contents/Helpers"]
      stringByAppendingPathComponent:kPrivilegedUpdateHelperName];
}

// Connection lifetime and all replies are serialized on the main queue. Late
// replies after timeout/interruption are ignored, never retried as a mutation.
void Request(NSString* target,
             NSTimeInterval timeout,
             void (^send)(id<XenonPrivilegedUpdateProtocol>,
                          PrivilegedUpdateReply),
             PrivilegedUpdateReply reply) {
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    NSString* error = nil;
    NSXPCConnection* connection =
        CreatePrivilegedUpdateConnection(DaemonPath(target), &error);
    dispatch_async(dispatch_get_main_queue(), ^{
      if (!connection) {
        reply(nil,
              error ?: @"Could not authenticate privileged update service");
        return;
      }
      __block bool completed = false;
      PrivilegedUpdateReply finish = ^(NSString* value, NSString* failure) {
        dispatch_async(dispatch_get_main_queue(), ^{
          if (completed) {
            return;
          }
          completed = true;
          connection.interruptionHandler = nil;
          connection.invalidationHandler = nil;
          [connection invalidate];
          reply(value, failure);
        });
      };
      connection.interruptionHandler = ^{
        finish(nil, @"Privileged update service connection was interrupted");
      };
      connection.invalidationHandler = ^{
        finish(nil, @"Privileged update service connection was invalidated");
      };
      [connection activate];
      id<XenonPrivilegedUpdateProtocol> proxy =
          [connection remoteObjectProxyWithErrorHandler:^(NSError* failure) {
            finish(nil, DescribeUpdateNSError(failure));
          }];
      send(proxy, finish);
      dispatch_after(
          dispatch_time(DISPATCH_TIME_NOW,
                        static_cast<int64_t>(timeout * NSEC_PER_SEC)),
          dispatch_get_main_queue(), ^{
            finish(nil, @"Privileged update request timed out; the "
                        @"installed application has not been confirmed");
          });
    });
  });
}

NSString* BundleVersion(NSString* app) {
  NSDictionary* info = [NSDictionary
      dictionaryWithContentsOfFile:
          [app stringByAppendingPathComponent:@"Contents/Info.plist"]];
  id version = info[@"CFBundleShortVersionString"] ?: info[@"CFBundleVersion"];
  return [version isKindOfClass:[NSString class]] ? version : nil;
}

void RunGenerationRound(internal::GenerationCheck check,
                        internal::ServiceRefresh refresh,
                        internal::ServiceRefreshReply completion,
                        bool refresh_attempted) {
  __block bool checked = false;
  check(^(bool refresh_required, NSString* error) {
    if (checked) {
      return;
    }
    checked = true;
    if (error.length || !refresh_required) {
      completion(false, error);
      return;
    }
    if (refresh_attempted) {
      completion(
          false,
          @"The privileged service generation still differs after refresh");
      return;
    }
    __block bool refreshed = false;
    refresh(^(bool approval, NSString* refresh_error) {
      if (refreshed) {
        return;
      }
      refreshed = true;
      if (approval || refresh_error.length) {
        completion(approval, refresh_error);
        return;
      }
      RunGenerationRound(check, refresh, completion, true);
    });
  });
}

void RefreshService(SMAppService* service,
                    internal::ServiceRefreshReply completion)
    API_AVAILABLE(macos(13.0)) {
  __block bool completed = false;
  internal::ServiceRefreshReply finish = ^(bool approval, NSString* error) {
    if (completed) {
      return;
    }
    completed = true;
    completion(approval, error);
  };
  // Do not register a second process before asynchronous unregister has
  // completed. Both service and callbacks are retained through the operation.
  [service unregisterWithCompletionHandler:^(NSError* error) {
    dispatch_async(dispatch_get_main_queue(), ^{
      if (completed) {
        return;
      }
      if (error) {
        finish(false, DescribeUpdateNSError(error));
        return;
      }
      NSError* registration_error = nil;
      [service registerAndReturnError:&registration_error];
      if (service.status == SMAppServiceStatusRequiresApproval) {
        [SMAppService openSystemSettingsLoginItems];
        finish(true, @"Allow the refreshed xl153 update service in System "
                     @"Settings, then retry installation");
      } else if (service.status != SMAppServiceStatusEnabled) {
        finish(false,
               registration_error
                   ? DescribeUpdateNSError(registration_error)
                   : @"The refreshed privileged update service is not enabled");
      } else {
        finish(false, nil);
      }
    });
  }];
  dispatch_after(
      dispatch_time(DISPATCH_TIME_NOW, 60 * NSEC_PER_SEC),
      dispatch_get_main_queue(), ^{
        finish(false, @"Timed out refreshing the privileged update service");
      });
}

bool Fail(NSString** error, NSString* message) {
  if (error) {
    *error = message;
  }
  return false;
}

bool DirectoryOwnedByCurrentUser(int descriptor, unsigned depth) {
  if (depth > 128) {
    return false;
  }
  struct stat metadata;
  if (fstat(descriptor, &metadata) != 0 || !S_ISDIR(metadata.st_mode) ||
      metadata.st_uid != geteuid() || metadata.st_uid == 0) {
    return false;
  }
  const int duplicate = dup(descriptor);
  if (duplicate < 0) {
    return false;
  }
  DIR* directory = fdopendir(duplicate);
  if (!directory) {
    close(duplicate);
    return false;
  }
  bool owned = true;
  while (owned) {
    errno = 0;
    struct dirent* entry = readdir(directory);
    if (!entry) {
      owned = errno == 0;
      break;
    }
    NSString* name = [NSString stringWithUTF8String:entry->d_name];
    if ([name isEqualToString:@"."] || [name isEqualToString:@".."]) {
      continue;
    }
    if (fstatat(descriptor, entry->d_name, &metadata, AT_SYMLINK_NOFOLLOW) !=
            0 ||
        metadata.st_uid != geteuid() || metadata.st_uid == 0) {
      owned = false;
      break;
    }
    if (S_ISDIR(metadata.st_mode)) {
      const int child = openat(descriptor, entry->d_name,
                               O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      owned = child >= 0 && DirectoryOwnedByCurrentUser(child, depth + 1);
      if (child >= 0) {
        close(child);
      }
    } else if (!S_ISREG(metadata.st_mode) && !S_ISLNK(metadata.st_mode)) {
      owned = false;
    }
  }
  closedir(directory);
  return owned;
}

bool ValidRecord(NSDictionary* record, NSString* target) {
  return [record isKindOfClass:[NSDictionary class]] &&
         [record[@"schema_version"] isEqual:@1] &&
         [record[@"target"] isEqual:target] &&
         [record[@"target_version"] isKindOfClass:[NSString class]] &&
         [record[@"status"] isKindOfClass:[NSString class]] &&
         [record[@"message"] isKindOfClass:[NSString class]] &&
         [record[@"timestamp"] isKindOfClass:[NSNumber class]];
}

// Walk private cache components with directory descriptors, refusing symlinks,
// foreign owners and nonprivate existing directories. No permission repair.
int OpenResultDirectory(NSString* target,
                        NSString* cache_override,
                        bool create,
                        NSString** error) {
  if (!target.isAbsolutePath) {
    Fail(error, @"Invalid privileged update result target");
    return -1;
  }
  NSString* cache = cache_override
                        ?: NSSearchPathForDirectoriesInDomains(
                               NSCachesDirectory, NSUserDomainMask, YES)
                               .firstObject;
  if (!cache.isAbsolutePath) {
    Fail(error, @"Invalid privileged update result cache");
    return -1;
  }
  int parent = open(cache.fileSystemRepresentation,
                    O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  struct stat metadata;
  if (parent < 0 || fstat(parent, &metadata) != 0 ||
      metadata.st_uid != geteuid()) {
    if (parent >= 0) {
      close(parent);
    }
    Fail(error, @"Update result cache is not an owned directory");
    return -1;
  }
  NSData* bytes = [target dataUsingEncoding:NSUTF8StringEncoding];
  unsigned char digest[CC_SHA256_DIGEST_LENGTH];
  CC_SHA256(bytes.bytes, static_cast<CC_LONG>(bytes.length), digest);
  NSMutableString* name = [NSMutableString string];
  for (unsigned char byte : digest) {
    [name appendFormat:@"%02x", byte];
  }
  for (NSString* component in @[ @"XenonUpdaterResults", name ]) {
    if (create &&
        mkdirat(parent, component.fileSystemRepresentation, 0700) != 0 &&
        errno != EEXIST) {
      close(parent);
      Fail(error, @"Could not create private update result directory");
      return -1;
    }
    const int next = openat(parent, component.fileSystemRepresentation,
                            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    close(parent);
    parent = next;
    if (parent < 0 || fstat(parent, &metadata) != 0 ||
        metadata.st_uid != geteuid() || (metadata.st_mode & 0777) != 0700) {
      if (parent >= 0) {
        close(parent);
      }
      if (create || errno != ENOENT) {
        Fail(error, @"Update result directory is not private");
      }
      return -1;
    }
  }
  return parent;
}

}  // namespace

namespace internal {
void RunServiceGenerationWorkflow(GenerationCheck check,
                                  ServiceRefresh refresh,
                                  ServiceRefreshReply completion) {
  __block bool completed = false;
  RunGenerationRound(
      check, refresh,
      ^(bool approval, NSString* error) {
        if (completed) {
          return;
        }
        completed = true;
        completion(approval, error);
      },
      false);
}
}  // namespace internal

NSString* DescribeUpdateNSError(NSError* error) {
  NSMutableArray<NSString*>* parts = [NSMutableArray array];
  for (NSUInteger depth = 0; error && depth < 4; ++depth) {
    [parts addObject:[NSString stringWithFormat:@"%@ (%ld)", error.domain,
                                                static_cast<long>(error.code)]];
    id underlying = error.userInfo[NSUnderlyingErrorKey];
    error = [underlying isKindOfClass:[NSError class]] ? underlying : nil;
  }
  return [parts componentsJoinedByString:@"; caused by "];
}

bool UpdateRequiresElevation(NSString* target) {
  if (!target.isAbsolutePath ||
      access(target.stringByDeletingLastPathComponent.fileSystemRepresentation,
             W_OK | X_OK) != 0) {
    return true;
  }
  const int directory = open(target.fileSystemRepresentation,
                             O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (directory < 0) {
    return true;
  }
  const bool owned = DirectoryOwnedByCurrentUser(directory, 0);
  close(directory);
  return !owned;
}

void BeginPrivilegedUpdatePreparation(NSString* staged,
                                      NSString* target,
                                      void (^reply)(NSString*,
                                                    bool,
                                                    NSString*)) {
  // Cryptographic checks and bundle reads must not block the browser UI.
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    NSString* error = nil;
    if (!CanUsePrivilegedUpdateForTarget(target, &error)) {
      dispatch_async(dispatch_get_main_queue(), ^{
        reply(nil, false, error);
      });
      return;
    }
    NSString* base_version = BundleVersion(target);
    NSString* target_version = BundleVersion(staged);
    if (!base_version.length || !target_version.length) {
      dispatch_async(dispatch_get_main_queue(), ^{
        reply(nil, false, @"The staged update has no valid product version");
      });
      return;
    }
    NSString* generation = PrivilegedServiceGeneration(target, false, &error);
    if (!generation.length) {
      dispatch_async(dispatch_get_main_queue(), ^{
        reply(nil, false,
              error ?: @"Could not determine privileged service generation");
      });
      return;
    }
    dispatch_async(dispatch_get_main_queue(), ^{
      if (@available(macOS 13.0, *)) {
        SMAppService* service =
            [SMAppService daemonServiceWithPlistName:kPrivilegedUpdatePlist];
        NSError* registration_error = nil;
        if (service.status != SMAppServiceStatusEnabled &&
            service.status != SMAppServiceStatusRequiresApproval) {
          [service registerAndReturnError:&registration_error];
        }
        if (service.status == SMAppServiceStatusRequiresApproval) {
          [SMAppService openSystemSettingsLoginItems];
          reply(nil, true,
                @"Allow the xl153 update service in System Settings, "
                @"then retry installation");
          return;
        }
        if (service.status != SMAppServiceStatusEnabled) {
          reply(nil, false,
                registration_error
                    ? DescribeUpdateNSError(registration_error)
                    : @"Privileged update service is not enabled");
          return;
        }
        internal::RunServiceGenerationWorkflow(
            ^(internal::GenerationReply checked) {
              Request(
                  target, 30,
                  ^(id<XenonPrivilegedUpdateProtocol> proxy,
                    PrivilegedUpdateReply complete) {
                    [proxy prepareServiceForGeneration:generation
                                                 reply:^(BOOL refresh_required,
                                                         NSString* failure) {
                                                   complete(refresh_required
                                                                ? @"refresh"
                                                                : @"current",
                                                            failure);
                                                 }];
                  },
                  ^(NSString* result, NSString* failure) {
                    checked([result isEqualToString:@"refresh"],
                            failure
                                ?: (result ? nil
                                           : @"No privileged service "
                                             @"generation reply"));
                  });
            },
            ^(internal::ServiceRefreshReply refreshed) {
              RefreshService(service, refreshed);
            },
            ^(bool approval, NSString* generation_error) {
              if (approval || generation_error.length) {
                reply(nil, approval, generation_error);
                return;
              }
              Request(
                  target, 300,
                  ^(id<XenonPrivilegedUpdateProtocol> proxy,
                    PrivilegedUpdateReply complete) {
                    [proxy prepareUpdateAtPath:staged
                           expectedBaseVersion:base_version
                         expectedTargetVersion:target_version
                                         reply:complete];
                  },
                  ^(NSString* token, NSString* failure) {
                    reply(token, false, failure);
                  });
            });
      } else {
        reply(nil, false, @"Privileged updates require macOS 13 or later");
      }
    });
  });
}

void PerformPrivilegedUpdateOperation(NSString* target,
                                      NSString* token,
                                      PrivilegedUpdateOperation operation,
                                      bool preserve_backup,
                                      PrivilegedUpdateReply reply) {
  if (![[NSUUID alloc] initWithUUIDString:token]) {
    dispatch_async(dispatch_get_main_queue(), ^{
      reply(nil, @"Invalid privileged update transaction token");
    });
    return;
  }
  Request(
      target, operation == PrivilegedUpdateOperation::kState ? 15 : 120,
      ^(id<XenonPrivilegedUpdateProtocol> proxy,
        PrivilegedUpdateReply complete) {
        void (^boolean_reply)(BOOL, NSString*) =
            ^(BOOL success, NSString* error) {
              complete(success ? @"ok" : nil, error);
            };
        switch (operation) {
          case PrivilegedUpdateOperation::kState:
            [proxy getUpdateStateWithToken:token reply:complete];
            break;
          case PrivilegedUpdateOperation::kCommit:
            [proxy commitUpdateWithToken:token reply:boolean_reply];
            break;
          case PrivilegedUpdateOperation::kRollback:
            [proxy rollbackUpdateWithToken:token reply:boolean_reply];
            break;
          case PrivilegedUpdateOperation::kFinish:
            [proxy finishUpdateWithToken:token
                          preserveBackup:preserve_backup
                                   reply:boolean_reply];
            break;
        }
      },
      reply);
}

bool WritePrivilegedUpdateResult(NSString* target,
                                 NSString* target_version,
                                 NSString* status,
                                 NSString* message,
                                 NSString** error,
                                 NSString* cache_override) {
  target = target.stringByResolvingSymlinksInPath;
  NSDictionary* record = @{
    @"schema_version" : @1,
    @"target" : target ?: @"",
    @"target_version" : target_version ?: @"",
    @"status" : status ?: @"",
    @"message" : message ?: @"",
    @"retained_path" : @"",
    @"timestamp" : @([NSDate date].timeIntervalSince1970)
  };
  NSData* data = [NSJSONSerialization dataWithJSONObject:record
                                                 options:0
                                                   error:nil];
  if (!ValidRecord(record, target) || !data || data.length > 65536) {
    return Fail(error, @"Invalid privileged update result");
  }
  const int directory =
      OpenResultDirectory(target, cache_override, true, error);
  if (directory < 0) {
    return false;
  }
  NSString* temporary =
      [NSString stringWithFormat:@".%@.tmp", NSUUID.UUID.UUIDString];
  const int output =
      openat(directory, temporary.fileSystemRepresentation,
             O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  bool success = output >= 0;
  if (success) {
    NSFileHandle* writer = [[NSFileHandle alloc] initWithFileDescriptor:output
                                                         closeOnDealloc:NO];
    success = fchmod(output, 0600) == 0 && [writer writeData:data error:nil] &&
              fsync(output) == 0;
    close(output);
  }
  if (success) {
    success = renameat(directory, temporary.fileSystemRepresentation, directory,
                       "result.json") == 0;
  }
  if (!success) {
    unlinkat(directory, temporary.fileSystemRepresentation, 0);
  }
  if (success) {
    fsync(directory);
  }
  close(directory);
  return success || Fail(error, @"Could not persist privileged update result");
}

NSDictionary* ReadPrivilegedUpdateResult(NSString* target,
                                         NSString** error,
                                         NSString* cache_override) {
  target = target.stringByResolvingSymlinksInPath;
  const int directory =
      OpenResultDirectory(target, cache_override, false, error);
  if (directory < 0) {
    return nil;
  }
  const int input = openat(directory, "result.json",
                           O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
  close(directory);
  struct stat metadata;
  if (input < 0) {
    return nil;
  }
  if (fstat(input, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
      metadata.st_uid != geteuid() || metadata.st_nlink != 1 ||
      (metadata.st_mode & 0777) != 0600 || metadata.st_size > 65536 ||
      metadata.st_size < 0) {
    close(input);
    Fail(error, @"Privileged update result is not a bounded private file");
    return nil;
  }
  NSFileHandle* reader = [[NSFileHandle alloc] initWithFileDescriptor:input
                                                       closeOnDealloc:YES];
  NSData* data = [reader readDataUpToLength:65537 error:nil];
  id record = data ? [NSJSONSerialization JSONObjectWithData:data
                                                     options:0
                                                       error:nil]
                   : nil;
  return ValidRecord(record, target) ? record : nil;
}

}  // namespace xenon::updater
