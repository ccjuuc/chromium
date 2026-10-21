// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_UPDATER_XENON_UPDATE_MAC_UTIL_H_
#define XENON_OVERLAY_CHROME_BROWSER_UPDATER_XENON_UPDATE_MAC_UTIL_H_

#import <Foundation/Foundation.h>
#include <sys/types.h>

namespace xenon::updater {

// These functions intentionally have no Chromium dependencies: the replacement
// helper must keep working after the running browser framework is replaced.
bool ValidateUpdateBundles(NSString* staged_app,
                           NSString* target_app,
                           NSString** error);

// Read only the bundle/main-executable quarantine attributes. This neither
// changes attributes nor predicts whether macOS will permit an actual launch.
bool HasUpdateQuarantine(NSString* app, bool* quarantined, NSString** error);

// After a failed launch, quarantine (or inability to inspect it) keeps the new
// app installed for user action. nil means quarantine is absent and the caller
// can roll back an ordinary launch failure. Does not launch or move anything.
NSString* PendingUpdateStatusAfterLaunchFailure(NSString* app,
                                                NSString** error);

// A versioned, owner-only result beside this particular target bundle. Writes
// replace the complete JSON atomically. Reads never consume the result and
// return nil without an error when no result exists. These are diagnostics,
// never authorization to launch or change files.
bool WriteUpdateResult(NSString* target_app,
                       NSString* target_version,
                       NSString* status,
                       NSString* message,
                       NSString* retained_path,
                       NSString** error);
NSDictionary* ReadUpdateResult(NSString* target_app, NSString** error);

// No signing check: this is only the ownership/write-access qualification for
// in-place differential installation before downloading an update.
bool CanInstallOwnedUpdateBundle(NSString* target, NSString** error);

// Copies a flat PKG from an open, non-symlink regular source into a
// newly-created private persistent subdirectory of cache_root. The copied bytes
// must match the expected SHA-256. Quarantine metadata is preserved. Returns
// nil on failure.
NSString* PrepareInstallerPackageInCache(NSString* source,
                                         NSString* expected_sha256,
                                         NSString* cache_root,
                                         NSString** error);

// Validates an NSWorkspace completion result without launching or signalling
// anything. A result for an existing app with the same bundle ID is not enough:
// both the actual bundle and executable must belong to the requested target.
bool ValidateRelaunchResult(NSString* target_app,
                            pid_t process_id,
                            bool terminated,
                            NSURL* bundle_url,
                            NSURL* executable_url,
                            NSString** error);

// Move a caller-owned prepared bundle to an unused hidden .app sibling of the
// installed bundle. This never replaces an existing destination or changes
// permissions. The caller must revalidate the promoted bundle before swapping.
bool PromoteUpdateBundleToSibling(NSString* prepared_app,
                                  NSString* target_app,
                                  NSString* sibling_app,
                                  NSString** error);

// Both bundles must be on the same volume. On failure both paths are unchanged;
// on success the old application is at staged_app, available for rollback.
bool AtomicSwapAppBundles(NSString* staged_app,
                          NSString* target_app,
                          NSString** error);

}  // namespace xenon::updater

#endif  // XENON_OVERLAY_CHROME_BROWSER_UPDATER_XENON_UPDATE_MAC_UTIL_H_
