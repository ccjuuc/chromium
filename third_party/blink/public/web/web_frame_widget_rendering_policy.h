// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_PUBLIC_WEB_WEB_FRAME_WIDGET_RENDERING_POLICY_H_
#define THIRD_PARTY_BLINK_PUBLIC_WEB_WEB_FRAME_WIDGET_RENDERING_POLICY_H_

#include "xenon_overlay/buildflags/buildflags.h"

#if BUILDFLAG(ENABLE_XENON_SERVICE)
#include "base/functional/callback_helpers.h"
#include "third_party/blink/public/platform/web_common.h"

namespace blink {
class WebFrameWidget;

// Renderer sequence only. Allows an authorized embedder's active widget to
// produce frames even when the native window does not present them. Actual
// widget/page visibility and freezing are unchanged. Scopes compose with each
// other and with existing clients such as document picture-in-picture.
BLINK_EXPORT base::ScopedClosureRunner SuppressUndrawnFrameThrottling(
    WebFrameWidget* widget);
}  // namespace blink
#endif

#endif  // THIRD_PARTY_BLINK_PUBLIC_WEB_WEB_FRAME_WIDGET_RENDERING_POLICY_H_
