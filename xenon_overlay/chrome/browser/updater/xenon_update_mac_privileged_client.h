// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_UPDATER_XENON_UPDATE_MAC_PRIVILEGED_CLIENT_H_
#define XENON_OVERLAY_CHROME_BROWSER_UPDATER_XENON_UPDATE_MAC_PRIVILEGED_CLIENT_H_

#import <Foundation/Foundation.h>

namespace xenon::updater {

enum class PrivilegedUpdateOperation { kState, kCommit, kRollback, kFinish };
using PrivilegedUpdateReply = void (^)(NSString* value, NSString* error);

namespace internal {
// Pure asynchronous control flow, separated from authenticated XPC and SM APIs
// so ordering, duplicate replies and the one-refresh limit can be tested.
// All supplied callbacks run on the main queue.
using GenerationReply = void (^)(bool refresh_required, NSString* error);
using GenerationCheck = void (^)(GenerationReply reply);
using ServiceRefreshReply = void (^)(bool approval, NSString* error);
using ServiceRefresh = void (^)(ServiceRefreshReply reply);
void RunServiceGenerationWorkflow(GenerationCheck check,
                                  ServiceRefresh refresh,
                                  ServiceRefreshReply completion);
}  // namespace internal

// Explicit user installation only. Performs signature checks before requesting
// service registration. Replies on the main queue; approval never means ready.
void BeginPrivilegedUpdatePreparation(NSString* staged,
                                      NSString* target,
                                      void (^reply)(NSString* token,
                                                    bool requires_approval,
                                                    NSString* error));

// Each request authenticates the daemon and has a bounded completion. These
// functions never register a service, launch a GUI, or change quarantine.
void PerformPrivilegedUpdateOperation(NSString* target,
                                      NSString* token,
                                      PrivilegedUpdateOperation operation,
                                      bool preserve_backup,
                                      PrivilegedUpdateReply reply);

NSString* DescribeUpdateNSError(NSError* error);

// Read-only, potentially blocking. Checks every entry without following
// symlinks; unreadable/special/foreign-owned entries fail closed.
bool UpdateRequiresElevation(NSString* target);

// Diagnostic records live in a private per-user cache, even when /Applications
// is not user-writable. They never authorize a transaction. A cache override is
// used only by standalone tests.
bool WritePrivilegedUpdateResult(NSString* target,
                                 NSString* target_version,
                                 NSString* status,
                                 NSString* message,
                                 NSString** error,
                                 NSString* cache_override = nil);
NSDictionary* ReadPrivilegedUpdateResult(NSString* target,
                                         NSString** error,
                                         NSString* cache_override = nil);

}  // namespace xenon::updater

#endif  // XENON_OVERLAY_CHROME_BROWSER_UPDATER_XENON_UPDATE_MAC_PRIVILEGED_CLIENT_H_
