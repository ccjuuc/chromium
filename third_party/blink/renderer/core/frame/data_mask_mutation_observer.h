// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_FRAME_DATA_MASK_MUTATION_OBSERVER_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_FRAME_DATA_MASK_MUTATION_OBSERVER_H_

#include "third_party/blink/renderer/core/dom/mutation_observer.h"
#include "third_party/blink/renderer/platform/heap/member.h"

namespace blink {

class Element;
class LocalFrame;

// Watches the document element (`<html>`) for DOM mutations and re-applies
// DataMask rules so parser- and script-inserted text does not stay unmasked
// between layouts/paints (same idea as aiswork's ElementWebRootObserver).
class DataMaskSubtreeObserver final : public MutationObserver::Delegate {
 public:
  DataMaskSubtreeObserver(LocalFrame& frame, Element& document_element);

  void Disconnect();

  Element* ObservedRoot() const { return root_.Get(); }

  ExecutionContext* GetExecutionContext() const override;
  void Deliver(const MutationRecordVector& records, MutationObserver&) override;
  void Trace(Visitor* visitor) const override;

 private:
  WeakMember<LocalFrame> frame_;
  Member<Element> root_;
  Member<MutationObserver> observer_;
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_FRAME_DATA_MASK_MUTATION_OBSERVER_H_
