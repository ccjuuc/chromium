// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_UPDATER_XENON_UPDATE_MAC_PRIVILEGED_PROTOCOL_H_
#define XENON_OVERLAY_CHROME_BROWSER_UPDATER_XENON_UPDATE_MAC_PRIVILEGED_PROTOCOL_H_

#import <Foundation/Foundation.h>

// No destination, command, executable or ownership parameters cross this
// interface. The daemon derives its one allowed installation from its own
// root-owned enclosing app. Tokens are random and bound to the initiating UID.
@protocol XenonPrivilegedUpdateProtocol
- (void)prepareServiceForGeneration:(NSString*)generation
                              reply:(void (^)(BOOL refresh_required,
                                              NSString* error))reply;
- (void)prepareUpdateAtPath:(NSString*)path
        expectedBaseVersion:(NSString*)base_version
      expectedTargetVersion:(NSString*)target_version
                      reply:(void (^)(NSString* token, NSString* error))reply;
- (void)getUpdateStateWithToken:(NSString*)token
                          reply:
                              (void (^)(NSString* state, NSString* error))reply;
- (void)commitUpdateWithToken:(NSString*)token
                        reply:(void (^)(BOOL success, NSString* error))reply;
- (void)rollbackUpdateWithToken:(NSString*)token
                          reply:(void (^)(BOOL success, NSString* error))reply;
- (void)finishUpdateWithToken:(NSString*)token
               preserveBackup:(BOOL)preserve_backup
                        reply:(void (^)(BOOL success, NSString* error))reply;
@end

namespace xenon::updater {
inline NSString* const kPrivilegedUpdateService =
    @"com.xl153.updater.privileged";
inline NSString* const kPrivilegedUpdatePlist =
    @"com.xl153.updater.privileged.plist";
inline NSString* const kPrivilegedUpdateHelperName =
    @"xenon_privileged_update_helper";
}  // namespace xenon::updater

#endif  // XENON_OVERLAY_CHROME_BROWSER_UPDATER_XENON_UPDATE_MAC_PRIVILEGED_PROTOCOL_H_
