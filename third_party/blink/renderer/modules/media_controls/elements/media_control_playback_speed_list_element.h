// Copyright 2021 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_MODULES_MEDIA_CONTROLS_ELEMENTS_MEDIA_CONTROL_PLAYBACK_SPEED_LIST_ELEMENT_H_
#define THIRD_PARTY_BLINK_RENDERER_MODULES_MEDIA_CONTROLS_ELEMENTS_MEDIA_CONTROL_PLAYBACK_SPEED_LIST_ELEMENT_H_

#include "third_party/blink/renderer/modules/media_controls/elements/media_control_popup_menu_element.h"

namespace blink {

class Event;
class MediaControlsImpl;

class MediaControlPlaybackSpeedListElement final
    : public MediaControlPopupMenuElement {
 public:
  explicit MediaControlPlaybackSpeedListElement(MediaControlsImpl&);

  // Node interface.
  bool WillRespondToMouseClickEvents() override;

  void SetIsWanted(bool) final;

#if defined(FOR_XL)
  // Re-apply the open class if a collapse animation was interrupted by hover.
  void EnsureXlOpenAnimation();
#endif

  void Trace(Visitor*) const override;

 private:
  class RequestAnimationFrameCallback;
#if defined(FOR_XL)
  class OpenAnimationFrameCallback;
#endif

  Element* PopupAnchor() const override;

  void SetPosition() override;

  void DefaultEventHandler(Event&) override;

  void RefreshPlaybackSpeedListMenu();

#if defined(FOR_XL)
  bool IsXlSidePopup() const;
  void PositionXlBesideButton();
  void CancelXlCollapseAnimation();
  void FinishXlCollapseAnimation();
  Element* XlPlaybackSpeedListItemFromEventTarget(Node* target) const;
  void UpdateXlSideCheckedItem(Element* selected_item);

  // Bumps to cancel a pending collapse PostDelayedTask.
  int xl_collapse_generation_ = 0;
#endif

  void ApplyPlaybackRate(double playback_rate);

  // Creates the playback speed element in the list.
  Element* CreatePlaybackSpeedListItem(const int display_name,
                                       const double playback_rate,
                                       const String* custom_label = nullptr,
                                       bool xl_side_item = false);

  // Creates the header element of the playback speed list.
  Element* CreatePlaybackSpeedHeaderItem();

  // Centers vertically the checked item in the playback speed list.
  void CenterCheckedItem();

  Member<Element> checked_item_;
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_MODULES_MEDIA_CONTROLS_ELEMENTS_MEDIA_CONTROL_PLAYBACK_SPEED_LIST_ELEMENT_H_
