// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/blink/renderer/core/frame/data_mask_mutation_observer.h"

#include "third_party/blink/renderer/bindings/core/v8/v8_mutation_observer_init.h"
#include "third_party/blink/renderer/core/dom/element.h"
#include "third_party/blink/renderer/core/dom/mutation_record.h"
#include "third_party/blink/renderer/platform/bindings/exception_state.h"
#include "xenon_overlay/blink/renderer/core/frame/data_mask_applier.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"

namespace blink {

DataMaskSubtreeObserver::DataMaskSubtreeObserver(LocalFrame& frame,
                                                 Element& document_element)
    : frame_(&frame),
      root_(document_element),
      observer_(MutationObserver::Create(this)) {
  MutationObserverInit* init = MutationObserverInit::Create();
  init->setChildList(true);
  init->setSubtree(true);
  init->setCharacterData(true);
  observer_->observe(&document_element, init, ASSERT_NO_EXCEPTION);
}

void DataMaskSubtreeObserver::Disconnect() {
  if (observer_) {
    observer_->disconnect();
  }
}

ExecutionContext* DataMaskSubtreeObserver::GetExecutionContext() const {
  return root_ ? root_->GetExecutionContext() : nullptr;
}

void DataMaskSubtreeObserver::Deliver(const MutationRecordVector& records,
                                      MutationObserver&) {
  if (records.empty()) {
    return;
  }
  if (LocalFrame* frame = frame_.Get()) {
    ApplyDataMaskForLocalFrame(*frame);
  }
}

void DataMaskSubtreeObserver::Trace(Visitor* visitor) const {
  visitor->Trace(frame_);
  visitor->Trace(root_);
  visitor->Trace(observer_);
  MutationObserver::Delegate::Trace(visitor);
}

}  // namespace blink
