// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/content/browser/render_frame_host_xenon_data_mask.h"

#include "base/supports_user_data.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "content/browser/renderer_host/render_frame_host_impl.h"
#include "third_party/blink/public/mojom/frame/data_mask.mojom.h"
#include "url/gurl.h"

namespace xenon {

namespace {

const void* const kRenderFrameHostDataMaskUserDataKey =
    &kRenderFrameHostDataMaskUserDataKey;

class RenderFrameHostDataMaskUserData : public base::SupportsUserData::Data {
 public:
  mojo::Remote<blink::mojom::DataMask> remote;
};

mojo::Remote<blink::mojom::DataMask>& GetOrBindDataMaskRemote(
    content::RenderFrameHostImpl* rfh) {
  auto* existing = static_cast<RenderFrameHostDataMaskUserData*>(
      rfh->GetUserData(kRenderFrameHostDataMaskUserDataKey));
  if (!existing) {
    auto created = std::make_unique<RenderFrameHostDataMaskUserData>();
    existing = created.get();
    rfh->SetUserData(kRenderFrameHostDataMaskUserDataKey, std::move(created));
  }
  if (!existing->remote.is_bound() && rfh->GetRemoteInterfaces()) {
    rfh->GetRemoteInterfaces()->GetInterface(
        existing->remote.BindNewPipeAndPassReceiver());
  }
  return existing->remote;
}

}  // namespace

void RenderFrameHostDataMaskApplyPolicy(content::RenderFrameHostImpl* host,
                                        const GURL& url) {
  (void)url;

  blink::mojom::DataMaskRulesPtr rules = blink::mojom::DataMaskRules::New();
  auto item = blink::mojom::MaskItem::New();
  item->tag_id = 1;
  item->policy_name = "GlobalTestMasking";
  item->regs.push_back("百度");
  item->mask_type = blink::mojom::MaskType::kReplace;
  item->replace_text = "***XENON***";
  item->is_content_mask = true;
  rules->mask_items.push_back(std::move(item));

  if (!rules->mask_items.empty()) {
    GetOrBindDataMaskRemote(host)->SendData(2001, rules.Clone());
  }
}

void RenderFrameHostDataMaskTearDown(content::RenderFrameHostImpl* host) {
  host->RemoveUserData(kRenderFrameHostDataMaskUserDataKey);
}

}  // namespace xenon
