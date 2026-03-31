// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/xenon_data_mask_to_main_host_impl.h"

#include <utility>

#include "base/logging.h"
#include "content/public/browser/render_frame_host.h"
#include "url/gurl.h"

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
  content::RenderFrameHost& rfh = render_frame_host();
  if (!rfh.IsRenderFrameLive()) {
    return;
  }

  // 浏览器侧收口：可在此接入 DLP/审计/持久化。默认仅打可观测日志；勿在 INFO 打印整段
  // `data`，避免泄露页面敏感内容。
  const GURL& url = rfh.GetLastCommittedURL();
  VLOG(1) << "Xenon DataMaskToMain"
          << " request_id=" << request_id << " url=" << url.spec()
          << " bytes=" << data.size();

  // TODO(xenon): Route `data` to policy / XenonMainService / metrics when the
  // pipeline is defined; keep synchronous work minimal (large payloads).
}

}  // namespace xenon
