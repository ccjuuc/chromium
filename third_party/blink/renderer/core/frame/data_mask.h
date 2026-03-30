// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_FRAME_DATA_MASK_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_FRAME_DATA_MASK_H_

#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "third_party/blink/public/mojom/frame/data_mask.mojom-blink.h"
#include "third_party/blink/public/platform/interface_registry.h"
#include "third_party/blink/renderer/core/core_export.h"
#include "third_party/blink/renderer/platform/heap/garbage_collected.h"
#include "third_party/blink/renderer/platform/heap/member.h"
#include "third_party/blink/renderer/platform/mojo/heap_mojo_receiver.h"

namespace blink {

class WebLocalFrameImpl;

// Renderer-side `blink::mojom::DataMask` implementation. The browser holds a
// `mojo::Remote<blink::mojom::DataMask>` obtained via the frame's child
// `InterfaceProvider` (see `RenderFrameHostImpl::GetDataMask()`).
class CORE_EXPORT DataMask final : public GarbageCollected<DataMask>,
                                   public mojom::blink::DataMask {
 public:
  DataMask(WebLocalFrameImpl& frame, InterfaceRegistry* interface_registry);
  DataMask(const DataMask&) = delete;
  DataMask& operator=(const DataMask&) = delete;

  void Trace(Visitor* visitor) const;
  void Dispose();

  // mojom::blink::DataMask:
  void SendData(int32_t /*request_id*/,
                mojom::blink::DataMaskRulesPtr rules) final;
  void SendXpathData(mojom::blink::XPathConfigPtr data) final;

 private:
  void BindToReceiver(mojo::PendingReceiver<mojom::blink::DataMask> receiver);

  WeakMember<WebLocalFrameImpl> frame_;

  // `nullptr` context matches `FindInPage`'s frame-bound registry interface;
  // disconnect still follows GC / `Dispose()`.
  HeapMojoReceiver<mojom::blink::DataMask, DataMask> receiver_{this, nullptr};
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_FRAME_DATA_MASK_H_
