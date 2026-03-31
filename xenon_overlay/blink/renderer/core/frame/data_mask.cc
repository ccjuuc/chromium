// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/blink/renderer/core/frame/data_mask.h"

#include "third_party/blink/public/platform/task_type.h"
#include "xenon_overlay/blink/renderer/core/frame/data_mask_applier.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/frame/web_local_frame_impl.h"
#include "third_party/blink/renderer/platform/wtf/functional.h"

namespace blink {

DataMask::DataMask(WebLocalFrameImpl& frame, InterfaceRegistry* interface_registry)
    : frame_(frame) {
  if (!interface_registry) {
    return;
  }
  interface_registry->AddInterface<mojom::blink::DataMask>(
      BindRepeating(&DataMask::BindToReceiver, WrapWeakPersistent(this)));
}

void DataMask::Trace(Visitor* visitor) const {
  visitor->Trace(frame_);
  visitor->Trace(receiver_);
}

void DataMask::Dispose() {
  receiver_.reset();
}

void DataMask::BindToReceiver(
    mojo::PendingReceiver<mojom::blink::DataMask> receiver) {
  if (!frame_) {
    return;
  }
  receiver_.Bind(std::move(receiver),
                 frame_->GetTaskRunner(TaskType::kInternalDefault));
}

void DataMask::SendData(int32_t /*request_id*/,
                        mojom::blink::DataMaskRulesPtr rules) {
  LocalFrame* local_frame = frame_ ? frame_->GetFrame() : nullptr;
  if (!local_frame) {
    return;
  }
  local_frame->SetDataMaskRules(std::move(rules));
  ApplyDataMaskForLocalFrame(*local_frame);
}

void DataMask::SendXpathData(mojom::blink::XPathConfigPtr data) {
  LocalFrame* local_frame = frame_ ? frame_->GetFrame() : nullptr;
  if (!local_frame) {
    return;
  }
  local_frame->SetDataMaskXPathConfig(std::move(data));
}

}  // namespace blink
