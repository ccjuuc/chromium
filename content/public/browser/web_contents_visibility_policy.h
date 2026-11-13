// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_PUBLIC_BROWSER_WEB_CONTENTS_VISIBILITY_POLICY_H_
#define CONTENT_PUBLIC_BROWSER_WEB_CONTENTS_VISIBILITY_POLICY_H_

#include "xenon_overlay/buildflags/buildflags.h"

#if BUILDFLAG(ENABLE_XENON_SERVICE)
#include "content/common/content_export.h"

namespace content {
class WebContents;
class RenderWidgetHost;

// Opt a single embedded widget into allocating navigation surfaces while its
// native view is hidden but its renderer is still drawing. Ordinary widgets
// retain Chromium's deferred-allocation policy. UI sequence only.
CONTENT_EXPORT void SetRenderWidgetHiddenSurfaceAllocationAllowed(
    RenderWidgetHost* widget,
    bool allowed);

// UI sequence only. Disabling occlusion maps native OCCLUDED updates to VISIBLE
// for this WebContents. Explicit HIDDEN updates, background throttling, other
// WebContents and the native window/compositor's occlusion policy are
// unchanged.
CONTENT_EXPORT void SetWebContentsOcclusionEnabled(WebContents* contents,
                                                   bool enabled);
}  // namespace content
#endif

#endif  // CONTENT_PUBLIC_BROWSER_WEB_CONTENTS_VISIBILITY_POLICY_H_
