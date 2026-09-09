// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_MODULES_MEDIA_CONTROLS_ELEMENTS_MEDIA_CONTROL_SIDE_ENCLOSURE_ELEMENT_H_
#define THIRD_PARTY_BLINK_RENDERER_MODULES_MEDIA_CONTROLS_ELEMENTS_MEDIA_CONTROL_SIDE_ENCLOSURE_ELEMENT_H_

#include "third_party/blink/renderer/modules/media_controls/elements/media_control_div_element.h"

namespace blink {

class MediaControlsImpl;

enum class MediaControlSideEnclosurePosition { kLeft, kRight };

class MediaControlSideEnclosureElement final : public MediaControlDivElement {
 public:
  MediaControlSideEnclosureElement(MediaControlsImpl&,
                                   MediaControlSideEnclosurePosition);

  void MakeOpaque();
  void MakeTransparent();
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_MODULES_MEDIA_CONTROLS_ELEMENTS_MEDIA_CONTROL_SIDE_ENCLOSURE_ELEMENT_H_
