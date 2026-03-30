// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/xenon_frame_interface_binder.h"

#include "content/public/browser/render_frame_host.h"
#include "mojo/public/cpp/bindings/binder_map.h"
#include "third_party/blink/public/mojom/frame/data_mask.mojom.h"
#include "xenon_overlay/chrome/browser/xenon_data_mask_to_main_host_impl.h"
#include "xenon_overlay/chrome/browser/xenon_page_host_impl.h"
#include "xenon_overlay/public/mojom/xenon_page_api.mojom.h"

namespace xenon {

namespace {

void BindXenonPageHost(
    content::RenderFrameHost* render_frame_host,
    mojo::PendingReceiver<xenon::mojom::XenonPageHost> receiver) {
  XenonPageHostImpl::Create(render_frame_host, std::move(receiver));
}

void BindDataMaskToMain(
    content::RenderFrameHost* render_frame_host,
    mojo::PendingReceiver<blink::mojom::DataMaskToMain> receiver) {
  XenonDataMaskToMainHostImpl::Create(render_frame_host, std::move(receiver));
}

}  // namespace

void PopulateXenonFrameBinders(
    mojo::BinderMapWithContext<content::RenderFrameHost*>* map) {
  map->Add<xenon::mojom::XenonPageHost>(&BindXenonPageHost);
  map->Add<blink::mojom::DataMaskToMain>(&BindDataMaskToMain);
}

}  // namespace xenon
