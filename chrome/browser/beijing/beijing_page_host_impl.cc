// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/beijing/beijing_page_host_impl.h"

#include <memory>

#include "chrome/browser/beijing/beijing_service.h"
#include "content/public/browser/render_frame_host.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "third_party/abseil-cpp/absl/strings/str_cat.h"

namespace beijing {

namespace {
constexpr char kBeijingPageApiVersion[] = "1.0.0";
}  // namespace

// static
void BeijingPageHostImpl::Create(
    content::RenderFrameHost* render_frame_host,
    mojo::PendingReceiver<mojom::BeijingPageHost> receiver) {
  mojo::MakeSelfOwnedReceiver(
      std::unique_ptr<BeijingPageHostImpl>(
          new BeijingPageHostImpl(render_frame_host)),
      std::move(receiver));
}

BeijingPageHostImpl::BeijingPageHostImpl(
    content::RenderFrameHost* render_frame_host)
    : render_frame_host_(render_frame_host) {}

BeijingPageHostImpl::~BeijingPageHostImpl() = default;

void BeijingPageHostImpl::Ping(PingCallback callback) {
  if (!render_frame_host_ || !render_frame_host_->IsRenderFrameLive()) {
    std::move(callback).Run(false, "Beijing: invalid frame");
    return;
  }
  std::move(callback).Run(
      true, absl::StrCat("Beijing OK from browser (rfh=", render_frame_host_->GetRoutingID(), ")"));
}

void BeijingPageHostImpl::GetApiVersion(GetApiVersionCallback callback) {
  std::move(callback).Run(kBeijingPageApiVersion);
}

void BeijingPageHostImpl::EchoObject(base::Value input, EchoObjectCallback callback) {
  if (!render_frame_host_ || !render_frame_host_->IsRenderFrameLive()) {
    std::move(callback).Run(base::Value());
    return;
  }
  std::move(callback).Run(std::move(input));
}

void BeijingPageHostImpl::WrapObjectWithBrowserMeta(
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

void BeijingPageHostImpl::Subscribe(
    const std::string& topic,
    mojo::PendingAssociatedRemote<mojom::BeijingPageClient> client) {
  if (!render_frame_host_ || !render_frame_host_->IsRenderFrameLive()) {
    return;
  }
  BeijingService::GetInstance()->Subscribe(
      render_frame_host_->GetBrowserContext(), topic, std::move(client));
}

void BeijingPageHostImpl::Publish(const std::string& topic,
                                  base::Value payload) {
  if (!render_frame_host_ || !render_frame_host_->IsRenderFrameLive()) {
    return;
  }
  BeijingService::GetInstance()->Publish(
      render_frame_host_->GetBrowserContext(), topic, payload);
}

}  // namespace beijing
