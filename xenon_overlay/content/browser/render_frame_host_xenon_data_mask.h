// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CONTENT_BROWSER_RENDER_FRAME_HOST_XENON_DATA_MASK_H_
#define XENON_OVERLAY_CONTENT_BROWSER_RENDER_FRAME_HOST_XENON_DATA_MASK_H_

class GURL;

namespace content {
class RenderFrameHostImpl;
}

namespace xenon {

// Applies test/global DataMask rules by sending mojo to the frame's renderer.
void RenderFrameHostDataMaskApplyPolicy(content::RenderFrameHostImpl* host,
                                        const GURL& url);

// Drops the frame's DataMask pipe UserData (call from TearDownMojoConnection).
void RenderFrameHostDataMaskTearDown(content::RenderFrameHostImpl* host);

}  // namespace xenon

#endif  // XENON_OVERLAY_CONTENT_BROWSER_RENDER_FRAME_HOST_XENON_DATA_MASK_H_
