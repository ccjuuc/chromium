// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/xenon_page_host_impl.h"

#include <memory>

#include "base/functional/callback.h"
#include "content/public/browser/render_frame_host.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "third_party/abseil-cpp/absl/strings/str_cat.h"

#include "xenon_overlay/chrome/browser/xenon_page_tool_manager.h"

namespace xenon {

namespace {

constexpr char kXenonPageApiVersion[] = "1.1.0";

}  // namespace

// static
void XenonPageHostImpl::Create(
    content::RenderFrameHost* render_frame_host,
    mojo::PendingReceiver<mojom::XenonPageHost> receiver) {
  mojo::MakeSelfOwnedReceiver(
      std::unique_ptr<XenonPageHostImpl>(
          new XenonPageHostImpl(render_frame_host)),
      std::move(receiver));
}

XenonPageHostImpl::XenonPageHostImpl(content::RenderFrameHost* render_frame_host)
    : render_frame_host_(render_frame_host) {}

XenonPageHostImpl::~XenonPageHostImpl() = default;

void XenonPageHostImpl::Ping(PingCallback callback) {
  if (!render_frame_host_ || !render_frame_host_->IsRenderFrameLive()) {
    std::move(callback).Run(false, "Xenon: invalid frame");
    return;
  }
  std::move(callback).Run(
      true, absl::StrCat("Xenon OK from browser (rfh=", render_frame_host_->GetRoutingID(),
                         ")"));
}

void XenonPageHostImpl::GetApiVersion(GetApiVersionCallback callback) {
  std::move(callback).Run(kXenonPageApiVersion);
}

void XenonPageHostImpl::EchoObject(base::Value input,
                                   EchoObjectCallback callback) {
  if (!render_frame_host_ || !render_frame_host_->IsRenderFrameLive()) {
    std::move(callback).Run(base::Value());
    return;
  }
  std::move(callback).Run(std::move(input));
}

void XenonPageHostImpl::WrapObjectWithBrowserMeta(
    base::Value input,
    WrapObjectWithBrowserMetaCallback callback) {
  base::DictValue wrapper;
  wrapper.Set("payload", std::move(input));
  if (render_frame_host_ && render_frame_host_->IsRenderFrameLive()) {
    wrapper.Set("browser_routing_id", render_frame_host_->GetRoutingID());
  } else {
    wrapper.Set("browser_routing_id", -1);
  }
  std::move(callback).Run(base::Value(std::move(wrapper)));
}

void XenonPageHostImpl::RegisterTool(
    const std::string& name,
    const std::string& description,
    const std::string& input_schema,
    mojo::PendingRemote<mojom::XenonToolExecutor> executor,
    RegisterToolCallback callback) {
  if (!render_frame_host_ || !render_frame_host_->IsRenderFrameLive()) {
    std::move(callback).Run(false);
    return;
  }
  XenonPageToolManager::GetOrCreateForCurrentDocument(render_frame_host_)
      ->RegisterTool(name, description, input_schema, std::move(executor));
  std::move(callback).Run(true);
}

}  // namespace xenon
