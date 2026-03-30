// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/xenon_data_mask_to_main_host_impl.h"

#include <utility>

#include "base/logging.h"
#include "content/public/browser/render_frame_host.h"

namespace xenon {

void XenonDataMaskToMainHostImpl::Create(
    content::RenderFrameHost* render_frame_host,
    mojo::PendingReceiver<blink::mojom::DataMaskToMain> receiver) {
  // DocumentService automatically manages its own lifetime, deleting itself
  // when the RenderFrameHost (document) is destroyed or the mojo connection
  // is closed.
  new XenonDataMaskToMainHostImpl(render_frame_host, std::move(receiver));
}

XenonDataMaskToMainHostImpl::XenonDataMaskToMainHostImpl(
    content::RenderFrameHost* render_frame_host,
    mojo::PendingReceiver<blink::mojom::DataMaskToMain> receiver)
    : content::DocumentService<blink::mojom::DataMaskToMain>(
          *render_frame_host, std::move(receiver)) {}

XenonDataMaskToMainHostImpl::~XenonDataMaskToMainHostImpl() = default;

void XenonDataMaskToMainHostImpl::SendDataToMain(int32_t request_id,
                                                 const std::string& data) {
  VLOG(1) << "Xenon DataMaskToMain request_id=" << request_id
          << " bytes=" << data.size()
          << " rfh_live="
          << render_frame_host().IsRenderFrameLive();
}

}  // namespace xenon
