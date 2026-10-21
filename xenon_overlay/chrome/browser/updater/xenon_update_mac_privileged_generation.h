// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_UPDATER_XENON_UPDATE_MAC_PRIVILEGED_GENERATION_H_
#define XENON_OVERLAY_CHROME_BROWSER_UPDATER_XENON_UPDATE_MAC_PRIVILEGED_GENERATION_H_

#import <Foundation/Foundation.h>

namespace xenon::updater {

// A comparison fingerprint, never authorization. Callers must first
// authenticate the app/helper using the production signing policy. May block. A
// daemon calls this with running_code=true once at startup and retains the
// result; clients use false to fingerprint the installed helper. Failure
// returns nil.
NSString* PrivilegedServiceGeneration(NSString* target,
                                      bool running_code,
                                      NSString** error);

}  // namespace xenon::updater

#endif  // XENON_OVERLAY_CHROME_BROWSER_UPDATER_XENON_UPDATE_MAC_PRIVILEGED_GENERATION_H_
