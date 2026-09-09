// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/modules/media_controls/elements/media_control_side_enclosure_element.h"

#include "third_party/blink/renderer/core/html_names.h"
#include "third_party/blink/renderer/modules/media_controls/media_controls_impl.h"

namespace blink {

MediaControlSideEnclosureElement::MediaControlSideEnclosureElement(
    MediaControlsImpl& media_controls,
    MediaControlSideEnclosurePosition position)
    : MediaControlDivElement(media_controls) {
  SetShadowPseudoId(
      AtomicString(position == MediaControlSideEnclosurePosition::kLeft
                       ? "-internal-media-controls-left-side-enclosure"
                       : "-internal-media-controls-right-side-enclosure"));
}

void MediaControlSideEnclosureElement::MakeOpaque() {
  removeAttribute(html_names::kClassAttr);
}

void MediaControlSideEnclosureElement::MakeTransparent() {
  setAttribute(html_names::kClassAttr, AtomicString("transparent"));
}

}  // namespace blink
