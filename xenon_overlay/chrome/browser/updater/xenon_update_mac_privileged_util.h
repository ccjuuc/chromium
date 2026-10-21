// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_UPDATER_XENON_UPDATE_MAC_PRIVILEGED_UTIL_H_
#define XENON_OVERLAY_CHROME_BROWSER_UPDATER_XENON_UPDATE_MAC_PRIVILEGED_UTIL_H_

#import <Foundation/Foundation.h>
#include <sys/types.h>

namespace xenon::updater {

// No Chromium or AppKit dependency. All filesystem/signature operations may
// block. An ad-hoc/unsigned identity is never accepted by these functions.
NSDictionary* ReadTrustedCodeIdentity(NSString* path, NSString** error);
NSString* PrivilegedPeerRequirement(NSString* team,
                                    NSArray<NSString*>* identifiers,
                                    NSString** error);
NSString* PrivilegedTargetFromExecutable(NSString* executable,
                                         NSString** error);
bool CanUsePrivilegedUpdateForTarget(NSString* target, NSString** error);

// Returned inactive: configure interruption/invalidation handlers, then
// activate.
NSXPCConnection* CreatePrivilegedUpdateConnection(NSString* daemon_path,
                                                  NSString** error);

}  // namespace xenon::updater

// A single installation transaction, confined to one serial queue by its owner.
// Paths and ownership are selected locally by the daemon, never by an RPC.
@interface XenonPrivilegedUpdateJob : NSObject
- (instancetype)initWithTarget:(NSString*)target error:(NSString**)error;
#if defined(XENON_PRIVILEGED_UPDATE_TESTING)
// Only compiled into the standalone, unprivileged test executable. Production
// construction always requires root ownership and Apple-issued signatures.
- (instancetype)initForTestingWithTarget:(NSString*)target
                                   error:(NSString**)error;
- (void)expireLeaseForTesting;
#endif
// Expiry only discards uncommitted copies. A committed/rolled-back transaction
// loses its token/lock while retaining the private recovery bundle.
- (BOOL)reclaimIfExpired;
- (BOOL)reclaimUncommittedForCallerUID:(uid_t)caller_uid;
- (NSString*)preparePath:(NSString*)source
             baseVersion:(NSString*)base_version
           targetVersion:(NSString*)target_version
               callerPID:(pid_t)caller_pid
               callerUID:(uid_t)caller_uid
                   error:(NSString**)error;
- (NSString*)stateForToken:(NSString*)token
                 callerUID:(uid_t)caller_uid
                     error:(NSString**)error;
- (BOOL)commitToken:(NSString*)token
          callerUID:(uid_t)caller_uid
              error:(NSString**)error;
- (BOOL)rollbackToken:(NSString*)token
            callerUID:(uid_t)caller_uid
                error:(NSString**)error;
- (BOOL)finishToken:(NSString*)token
          callerUID:(uid_t)caller_uid
     preserveBackup:(BOOL)preserve_backup
              error:(NSString**)error;
@end

#endif  // XENON_OVERLAY_CHROME_BROWSER_UPDATER_XENON_UPDATE_MAC_PRIVILEGED_UTIL_H_
