// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_XENON_FRAME_INTERFACE_BINDER_H_
#define XENON_OVERLAY_CHROME_BROWSER_XENON_FRAME_INTERFACE_BINDER_H_

namespace content {
class RenderFrameHost;
}  // namespace content

namespace mojo {
template <typename>
class BinderMapWithContext;
}  // namespace mojo

namespace xenon {

// Registers document-scoped Xenon frame interfaces (`xenon::mojom::XenonPageHost`,
// `blink::mojom::DataMaskToMain`). `blink::mojom::DataMask` is implemented in
// Blink; browser-side sends rules via `xenon::RenderFrameHostDataMaskApplyPolicy`.
// (see //chrome/browser/chrome_browser_interface_binders.cc).
void PopulateXenonFrameBinders(
    mojo::BinderMapWithContext<content::RenderFrameHost*>* map);

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_XENON_FRAME_INTERFACE_BINDER_H_
