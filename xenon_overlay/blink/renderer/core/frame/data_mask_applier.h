// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_BLINK_RENDERER_CORE_FRAME_DATA_MASK_APPLIER_H_
#define XENON_OVERLAY_BLINK_RENDERER_CORE_FRAME_DATA_MASK_APPLIER_H_

#include "third_party/blink/renderer/core/core_export.h"

namespace blink {

class LocalFrame;

// Walks text nodes in the current document and applies the frame's
// `DataMaskRules` (content-mask items: regex, dictionary, replace/blur).
CORE_EXPORT void ApplyDataMaskForLocalFrame(LocalFrame& frame);

// Clears load pump / document-visibility suppression (call when rules are
// cleared).
CORE_EXPORT void ResetDataMaskPresentationState(LocalFrame& frame);

}  // namespace blink

#endif  // XENON_OVERLAY_BLINK_RENDERER_CORE_FRAME_DATA_MASK_APPLIER_H_
