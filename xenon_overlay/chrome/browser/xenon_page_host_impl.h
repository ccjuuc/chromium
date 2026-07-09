// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_XENON_PAGE_HOST_IMPL_H_
#define XENON_OVERLAY_CHROME_BROWSER_XENON_PAGE_HOST_IMPL_H_

#include "base/memory/raw_ptr.h"
#include "base/values.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "xenon_overlay/public/mojom/xenon_page_api.mojom.h"

namespace content {
class RenderFrameHost;
}  // namespace content

namespace xenon {

// Browser-side implementation of `xenon::mojom::XenonPageHost` for
// `window.xenon` (one instance per Mojo connection from the renderer).
class XenonPageHostImpl : public mojom::XenonPageHost {
 public:
  static void Create(content::RenderFrameHost* render_frame_host,
                     mojo::PendingReceiver<mojom::XenonPageHost> receiver);

  XenonPageHostImpl(const XenonPageHostImpl&) = delete;
  XenonPageHostImpl& operator=(const XenonPageHostImpl&) = delete;

  ~XenonPageHostImpl() override;

 private:
  explicit XenonPageHostImpl(content::RenderFrameHost* render_frame_host);

  // xenon::mojom::XenonPageHost:
  void Ping(PingCallback callback) override;
  void GetApiVersion(GetApiVersionCallback callback) override;
  void EchoObject(base::Value input, EchoObjectCallback callback) override;
  void WrapObjectWithBrowserMeta(base::Value input,
                                 WrapObjectWithBrowserMetaCallback callback) override;
  void RegisterTool(const std::string& name,
                    const std::string& description,
                    const std::string& input_schema,
                    mojo::PendingRemote<mojom::XenonToolExecutor> executor,
                    RegisterToolCallback callback) override;

  raw_ptr<content::RenderFrameHost> render_frame_host_;
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_XENON_PAGE_HOST_IMPL_H_
