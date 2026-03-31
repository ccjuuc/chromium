// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Out-of-tree implementations for LocalFrame DataMask APIs (xenon overlay).

#include "third_party/blink/renderer/core/frame/local_frame.h"

#include "third_party/blink/public/platform/task_type.h"
#include "xenon_overlay/blink/renderer/core/frame/data_mask_applier.h"
#include "xenon_overlay/blink/renderer/core/frame/data_mask_mutation_observer.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/dom/element.h"
#include "third_party/blink/renderer/platform/wtf/functional.h"

namespace blink {

void LocalFrame::SetDataMaskRules(mojom::blink::DataMaskRulesPtr rules) {
  data_mask_rules_ = std::move(rules);
  if (!data_mask_rules_ || data_mask_rules_->mask_items.empty()) {
    ResetDataMaskPresentationState(*this);
  }
}

void LocalFrame::EnsureDataMaskSubtreeObserver() {
  if (!GetDataMaskRules() || GetDataMaskRules()->mask_items.empty()) {
    ClearDataMaskMutationObserver();
    return;
  }
  Document* doc = GetDocument();
  Element* html = doc ? doc->documentElement() : nullptr;
  if (!html) {
    return;
  }
  if (data_mask_mutation_observer_ &&
      data_mask_mutation_observer_->ObservedRoot() == html) {
    return;
  }
  ClearDataMaskMutationObserver();
  data_mask_mutation_observer_ =
      MakeGarbageCollected<DataMaskSubtreeObserver>(*this, *html);
}

void LocalFrame::ClearDataMaskMutationObserver() {
  if (data_mask_mutation_observer_) {
    data_mask_mutation_observer_->Disconnect();
    data_mask_mutation_observer_.Clear();
  }
}

void LocalFrame::ScheduleDataMaskApplyPumpIfNeeded() {
  if (!GetDataMaskRules() || GetDataMaskRules()->mask_items.empty()) {
    return;
  }
  if (!IsLoading()) {
    return;
  }
  if (data_mask_load_pump_scheduled_) {
    return;
  }
  data_mask_load_pump_scheduled_ = true;
  GetTaskRunner(TaskType::kInternalLoading)
      ->PostTask(FROM_HERE, blink::BindOnce(&LocalFrame::RunDataMaskApplyPump,
                                             WrapWeakPersistent(this)));
}

void LocalFrame::RunDataMaskApplyPump() {
  data_mask_load_pump_scheduled_ = false;
  ApplyDataMaskForLocalFrame(*this);
}

const mojom::blink::DataMaskRules* LocalFrame::GetDataMaskRules() const {
  return data_mask_rules_.get();
}

void LocalFrame::SetDataMaskXPathConfig(mojom::blink::XPathConfigPtr config) {
  data_mask_xpath_config_ = std::move(config);
}

const mojom::blink::XPathConfig* LocalFrame::GetDataMaskXPathConfig() const {
  return data_mask_xpath_config_.get();
}

}  // namespace blink
