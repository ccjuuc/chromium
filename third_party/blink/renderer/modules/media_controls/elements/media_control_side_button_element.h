// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_MODULES_MEDIA_CONTROLS_ELEMENTS_MEDIA_CONTROL_SIDE_BUTTON_ELEMENT_H_
#define THIRD_PARTY_BLINK_RENDERER_MODULES_MEDIA_CONTROLS_ELEMENTS_MEDIA_CONTROL_SIDE_BUTTON_ELEMENT_H_

#include "third_party/blink/renderer/modules/media_controls/elements/media_control_input_element.h"
#include "ui/gfx/geometry/size.h"

namespace blink {

class Event;
class MediaControlsImpl;

enum class MediaControlSideButtonType { kXl1, kXl2, kXl3, kXr1 };

class MediaControlSideButtonElement final : public MediaControlInputElement {
 public:
  MediaControlSideButtonElement(MediaControlsImpl&, MediaControlSideButtonType);

  void UpdateDisplayType() override;

  bool WillRespondToMouseClickEvents() final;

  gfx::Size GetSizeOrDefault() const override;

 protected:
  const char* GetNameForHistograms() const final;

 private:
  void DefaultEventHandler(Event&) final;
  bool KeepEventInNode(const Event&) const final;

  AtomicString GetShadowPseudoIdForType() const;
  AtomicString GetAriaLabelForType() const;
  String GetVisibleLabelForType() const;

  MediaControlSideButtonType button_type_;
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_MODULES_MEDIA_CONTROLS_ELEMENTS_MEDIA_CONTROL_SIDE_BUTTON_ELEMENT_H_
