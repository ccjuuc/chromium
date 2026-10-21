// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <dispatch/dispatch.h>
#include <libproc.h>
#include <sys/event.h>
#include <sysexits.h>
#include <unistd.h>

#include <array>

#include "xenon_overlay/chrome/browser/updater/xenon_update_mac_privileged_generation.h"
#include "xenon_overlay/chrome/browser/updater/xenon_update_mac_privileged_protocol.h"
#include "xenon_overlay/chrome/browser/updater/xenon_update_mac_privileged_util.h"

// The daemon has no AppKit dependency and never runs a command or launches an
// application. A user-session helper performs launch/approval UI separately.
@interface XenonPrivilegedUpdateService
    : NSObject <NSXPCListenerDelegate, XenonPrivilegedUpdateProtocol>
- (instancetype)initWithTarget:(NSString*)target
                    generation:(NSString*)generation;
#if defined(XENON_PRIVILEGED_UPDATE_TESTING)
- (void)setJobForTesting:(XenonPrivilegedUpdateJob*)job;
- (void)setRefreshCallerPIDForTesting:(pid_t)pid;
#endif
@end

@implementation XenonPrivilegedUpdateService {
  NSString* _target;
  dispatch_queue_t _queue;
  dispatch_source_t _expiryTimer;
  XenonPrivilegedUpdateJob* _job;
  NSString* _loadedGeneration;
  BOOL _draining;
  pid_t _refreshCallerPID;
  int _refreshWatchFD;
#if defined(XENON_PRIVILEGED_UPDATE_TESTING)
  pid_t _testCallerPID;
#endif
}

- (instancetype)initWithTarget:(NSString*)target
                    generation:(NSString*)generation {
  self = [super init];
  if (self) {
    _target = [target copy];
    _loadedGeneration = [generation copy];
    _refreshWatchFD = -1;
    _queue = dispatch_queue_create("com.xl153.updater.privileged.transactions",
                                   DISPATCH_QUEUE_SERIAL);
    _expiryTimer =
        dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, _queue);
    dispatch_source_set_timer(
        _expiryTimer, dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC),
        30 * NSEC_PER_SEC, NSEC_PER_SEC);
    __weak XenonPrivilegedUpdateService* weak_self = self;
    dispatch_source_set_event_handler(_expiryTimer, ^{
      XenonPrivilegedUpdateService* strong_self = weak_self;
      if (strong_self && [strong_self->_job reclaimIfExpired]) {
        strong_self->_job = nil;
      }
    });
    dispatch_resume(_expiryTimer);
  }
  return self;
}

- (void)prepareServiceForGeneration:(NSString*)generation
                              reply:(void (^)(BOOL, NSString*))reply {
  pid_t pid = NSXPCConnection.currentConnection.processIdentifier;
#if defined(XENON_PRIVILEGED_UPDATE_TESTING)
  if (!pid) {
    pid = _testCallerPID ?: getpid();
  }
#endif
  dispatch_async(_queue, ^{
    if ([self->_job reclaimIfExpired]) {
      self->_job = nil;
    }
    if (self->_job) {
      reply(
          NO,
          @"An update transaction is active; the service cannot be refreshed");
      return;
    }
    if (![generation isKindOfClass:NSString.class] || !generation.length ||
        !self->_loadedGeneration.length || pid <= 1) {
      reply(NO,
            @"Could not determine the privileged update service generation");
      return;
    }
    if (self->_draining) {
      struct kevent event;
      const timespec timeout = {0, 0};
      int count =
          kevent(self->_refreshWatchFD, nullptr, 0, &event, 1, &timeout);
      if (count < 0 || (count == 1 && (event.flags & EV_ERROR))) {
        reply(NO,
              @"Could not determine whether the service refresh caller exited");
        return;
      }
      if (count == 1 && (event.fflags & NOTE_EXIT)) {
        self->_refreshCallerPID = 0;
      }
      if (self->_refreshCallerPID != 0 && self->_refreshCallerPID != pid) {
        reply(NO, @"Another running browser is refreshing the update service");
        return;
      }
    }
    BOOL refresh =
        self->_draining || ![self->_loadedGeneration isEqual:generation];
    if (refresh && self->_refreshCallerPID != pid) {
      if (self->_refreshWatchFD >= 0) {
        close(self->_refreshWatchFD);
      }
      self->_refreshWatchFD = kqueue();
      struct kevent watch;
      EV_SET(&watch, static_cast<uintptr_t>(pid), EVFILT_PROC,
             EV_ADD | EV_ENABLE | EV_ONESHOT, NOTE_EXIT, 0, nullptr);
      if (self->_refreshWatchFD < 0 ||
          kevent(self->_refreshWatchFD, &watch, 1, nullptr, 0, nullptr) != 0) {
        reply(NO, @"Could not watch the authenticated service refresh caller");
        return;
      }
      self->_refreshCallerPID = pid;
    }
    // Do not reopen prepare on a timer: an outstanding asynchronous unregister
    // can arrive late and kill a new transaction. Only a fresh daemon is open.
    if (refresh) {
      self->_draining = YES;
    }
    reply(refresh, nil);
  });
}

- (void)dealloc {
  if (_refreshWatchFD >= 0) {
    close(_refreshWatchFD);
  }
  if (_expiryTimer) {
    dispatch_source_cancel(_expiryTimer);
  }
}

#if defined(XENON_PRIVILEGED_UPDATE_TESTING)
- (void)setJobForTesting:(XenonPrivilegedUpdateJob*)job {
  dispatch_sync(_queue, ^{
    self->_job = job;
  });
}

- (void)setRefreshCallerPIDForTesting:(pid_t)pid {
  _testCallerPID = pid;
}
#endif

- (BOOL)listener:(NSXPCListener*)listener
    shouldAcceptNewConnection:(NSXPCConnection*)connection {
  // The listener's public signature requirement rejects untrusted peers before
  // this delegate is called. These attributes are supplied by XPC, not payload.
  if (connection.effectiveUserIdentifier == 0 ||
      connection.processIdentifier <= 1) {
    return NO;
  }
  connection.exportedInterface = [NSXPCInterface
      interfaceWithProtocol:@protocol(XenonPrivilegedUpdateProtocol)];
  connection.exportedObject = self;
  [connection activate];
  return YES;
}

- (void)prepareUpdateAtPath:(NSString*)path
        expectedBaseVersion:(NSString*)base_version
      expectedTargetVersion:(NSString*)target_version
                      reply:(void (^)(NSString*, NSString*))reply {
  NSXPCConnection* connection = NSXPCConnection.currentConnection;
  const uid_t uid = connection.effectiveUserIdentifier;
  const pid_t pid = connection.processIdentifier;
  dispatch_async(_queue, ^{
    @autoreleasepool {
      if (self->_draining) {
        reply(
            nil,
            @"The privileged update service is being refreshed; retry shortly");
        return;
      }
      if (self->_job && ([self->_job reclaimIfExpired] ||
                         [self->_job reclaimUncommittedForCallerUID:uid])) {
        self->_job = nil;
      }
      if (self->_job) {
        reply(nil, @"Another privileged update is already active");
        return;
      }
      NSString* error = nil;
      XenonPrivilegedUpdateJob* job =
          [[XenonPrivilegedUpdateJob alloc] initWithTarget:self->_target
                                                     error:&error];
      NSString* token = [job preparePath:path
                             baseVersion:base_version
                           targetVersion:target_version
                               callerPID:pid
                               callerUID:uid
                                   error:&error];
      if (token) {
        self->_job = job;
      }
      reply(token, error);
    }
  });
}

- (void)getUpdateStateWithToken:(NSString*)token
                          reply:(void (^)(NSString*, NSString*))reply {
  const uid_t uid = NSXPCConnection.currentConnection.effectiveUserIdentifier;
  dispatch_async(_queue, ^{
    NSString* error = nil;
    NSString* state = [self->_job stateForToken:token
                                      callerUID:uid
                                          error:&error];
    reply(state, error ?: (state ? nil : @"No privileged update is active"));
  });
}

- (void)commitUpdateWithToken:(NSString*)token
                        reply:(void (^)(BOOL, NSString*))reply {
  const uid_t uid = NSXPCConnection.currentConnection.effectiveUserIdentifier;
  dispatch_async(_queue, ^{
    NSString* error = nil;
    BOOL result = [self->_job commitToken:token callerUID:uid error:&error];
    reply(result, error ?: (result ? nil : @"No privileged update is active"));
  });
}

- (void)rollbackUpdateWithToken:(NSString*)token
                          reply:(void (^)(BOOL, NSString*))reply {
  const uid_t uid = NSXPCConnection.currentConnection.effectiveUserIdentifier;
  dispatch_async(_queue, ^{
    NSString* error = nil;
    BOOL result = [self->_job rollbackToken:token callerUID:uid error:&error];
    reply(result, error ?: (result ? nil : @"No privileged update is active"));
  });
}

- (void)finishUpdateWithToken:(NSString*)token
               preserveBackup:(BOOL)preserve_backup
                        reply:(void (^)(BOOL, NSString*))reply {
  const uid_t uid = NSXPCConnection.currentConnection.effectiveUserIdentifier;
  dispatch_async(_queue, ^{
    NSString* error = nil;
    // Invalid tokens must not clear another user's transaction or its lock.
    if (![self->_job stateForToken:token callerUID:uid error:&error]) {
      reply(NO, error ?: @"No privileged update is active");
      return;
    }
    BOOL result = [self->_job finishToken:token
                                callerUID:uid
                           preserveBackup:preserve_backup
                                    error:&error];
    self->_job = nil;
    reply(result, error);
  });
}

@end

#if !defined(XENON_PRIVILEGED_UPDATE_TESTING)
int main(int argc, const char* argv[]) {
  @autoreleasepool {
    if (geteuid() != 0) {
      NSLog(@"The privileged updater must be started by its approved system "
            @"service");
      return EX_NOPERM;
    }
    std::array<char, PROC_PIDPATHINFO_MAXSIZE> executable;
    if (proc_pidpath(getpid(), executable.data(),
                     static_cast<uint32_t>(executable.size())) <= 0) {
      return EX_OSERR;
    }
    NSString* error = nil;
    NSString* target = xenon::updater::PrivilegedTargetFromExecutable(
        [NSString stringWithUTF8String:executable.data()], &error);
    if (!target ||
        !xenon::updater::CanUsePrivilegedUpdateForTarget(target, &error)) {
      NSLog(@"Privileged updater refused its installation: %@", error);
      return EX_CONFIG;
    }
    NSDictionary* identity =
        xenon::updater::ReadTrustedCodeIdentity(target, &error);
    NSString* identifier = identity[@"identifier"];
    NSString* requirement = xenon::updater::PrivilegedPeerRequirement(
        identity[@"team_id"],
        @[
          identifier,
          [identifier stringByAppendingString:@".xenon-update-helper"]
        ],
        &error);
    if (!requirement) {
      NSLog(@"Privileged updater could not configure client authentication: %@",
            error);
      return EX_CONFIG;
    }
    NSString* generation =
        xenon::updater::PrivilegedServiceGeneration(target, true, &error);
    if (!generation.length) {
      NSLog(@"Privileged updater could not identify its loaded generation: %@",
            error);
      return EX_CONFIG;
    }
    __attribute__((objc_precise_lifetime))
    XenonPrivilegedUpdateService* service =
        [[XenonPrivilegedUpdateService alloc] initWithTarget:target
                                                  generation:generation];
    __attribute__((objc_precise_lifetime)) NSXPCListener* listener =
        [[NSXPCListener alloc]
            initWithMachServiceName:xenon::updater::kPrivilegedUpdateService];
    [listener setConnectionCodeSigningRequirement:requirement];
    listener.delegate = service;
    [listener activate];
    NSLog(@"Privileged updater is listening for authenticated update requests");
    [[NSRunLoop currentRunLoop] run];
  }
  return EX_OK;
}
#endif
