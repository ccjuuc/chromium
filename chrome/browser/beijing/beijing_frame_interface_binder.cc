// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/beijing/beijing_frame_interface_binder.h"

#include "chrome/browser/beijing/beijing_page_host_impl.h"
#include "content/public/browser/render_frame_host.h"
#include "mojo/public/cpp/bindings/binder_map.h"
#include "chrome/common/beijing/beijing_page_api.mojom.h"

namespace beijing {

namespace {

void BindBeijingPageHost(
    content::RenderFrameHost* render_frame_host,
    mojo::PendingReceiver<beijing::mojom::BeijingPageHost> receiver) {
  BeijingPageHostImpl::Create(render_frame_host, std::move(receiver));
}

}  // namespace

void PopulateBeijingFrameBinders(
    mojo::BinderMapWithContext<content::RenderFrameHost*>* map) {
  map->Add<beijing::mojom::BeijingPageHost>(&BindBeijingPageHost);
}

}  // namespace beijing
