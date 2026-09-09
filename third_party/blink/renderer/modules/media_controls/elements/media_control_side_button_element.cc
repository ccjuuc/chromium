// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/modules/media_controls/elements/media_control_side_button_element.h"

#include "base/notreached.h"
#include "third_party/blink/public/platform/platform.h"
#include "third_party/blink/public/platform/user_metrics_action.h"
#include "third_party/blink/public/strings/grit/blink_strings.h"
#include "third_party/blink/renderer/core/dom/events/event.h"
#include "third_party/blink/renderer/core/frame/picture_in_picture_controller.h"
#include "third_party/blink/renderer/core/html/media/html_media_element.h"
#include "third_party/blink/renderer/core/html/media/html_video_element.h"
#include "third_party/blink/renderer/core/html_names.h"
#include "third_party/blink/renderer/core/input_type_names.h"
#include "third_party/blink/renderer/modules/media_controls/elements/media_control_elements_helper.h"
#include "third_party/blink/renderer/modules/media_controls/media_controls_impl.h"
#include "third_party/blink/renderer/platform/text/platform_locale.h"
#include "ui/gfx/geometry/size.h"
#include "ui/strings/grit/ax_strings.h"

#if defined(FOR_XL)
#include <cmath>
#endif

namespace blink {

namespace {

constexpr int kXlSideButtonWidth = 48;
constexpr int kXlSideButtonHeight = 69;  // 48px tile + 8px gap + 13px label
constexpr int kXrSideButtonWidth = 48;
constexpr int kXrSideButtonHeight = 69;  // Match xl1 row height for alignment.

const char* UserMetricsActionForType(MediaControlSideButtonType type) {
  switch (type) {
    case MediaControlSideButtonType::kXl1:
      return "Media.Controls.Xl1";
    case MediaControlSideButtonType::kXl2:
      return "Media.Controls.Xl2";
    case MediaControlSideButtonType::kXl3:
      return "Media.Controls.Xl3";
    case MediaControlSideButtonType::kXr1:
      return "Media.Controls.Xr1";
  }
  NOTREACHED();
}

#if defined(FOR_XL)
String XlSideButtonPlaybackSpeedLabel(double playback_rate) {
  if (playback_rate == 1.0) {
    return String();
  }
  if (playback_rate == std::floor(playback_rate)) {
    return String::NumberToStringEcmaScript(playback_rate) + ".0X";
  }
  return String::NumberToStringEcmaScript(playback_rate) + "X";
}

AtomicString ButtonIdForType(MediaControlSideButtonType type) {
  switch (type) {
    case MediaControlSideButtonType::kXl1:
      return AtomicString("xl1");
    case MediaControlSideButtonType::kXl2:
      return AtomicString("xl2");
    case MediaControlSideButtonType::kXl3:
      return AtomicString("xl3");
    case MediaControlSideButtonType::kXr1:
      return AtomicString("xr1");
  }
  NOTREACHED();
}

// Notifies the page for xl1/xl2/xl3/xr1. Returns false if a listener called
// preventDefault() on the dispatched event.
bool DispatchXlControlActionToPage(HTMLMediaElement& media,
                                   MediaControlSideButtonType type) {
  DEFINE_STATIC_LOCAL(AtomicString, k_xl_control_action_event_type,
                      ("xlcontrolaction"));
  DEFINE_STATIC_LOCAL(AtomicString, k_data_xl_control_attribute,
                      ("data-xl-control"));

  const AtomicString button_id = ButtonIdForType(type);
  media.setAttribute(k_data_xl_control_attribute, button_id);

  Event* event = Event::CreateCancelableBubble(k_xl_control_action_event_type);
  media.DispatchEvent(*event);
  return !event->defaultPrevented();
}

// Built-in actions run unless the page explicitly cancels xlcontrolaction.
bool ShouldRunXlDefaultAction(bool dispatch_not_prevented) {
  return dispatch_not_prevented;
}

void TogglePictureInPicture(HTMLMediaElement& media) {
  auto* video_element = DynamicTo<HTMLVideoElement>(media);
  if (!video_element || !media.SupportsPictureInPicture()) {
    return;
  }

  PictureInPictureController& controller =
      PictureInPictureController::From(media.GetDocument());
  if (PictureInPictureController::IsElementInPictureInPicture(video_element)) {
    controller.ExitPictureInPicture(video_element, nullptr);
  } else {
    controller.EnterPictureInPicture(video_element, /*promise=*/nullptr);
  }
}

#endif

}  // namespace

MediaControlSideButtonElement::MediaControlSideButtonElement(
    MediaControlsImpl& media_controls,
    MediaControlSideButtonType button_type)
    : MediaControlInputElement(media_controls), button_type_(button_type) {
  setType(input_type_names::kButton);
  SetShadowPseudoId(GetShadowPseudoIdForType());
  SetIsWanted(true);
  UpdateDisplayType();
}

AtomicString MediaControlSideButtonElement::GetShadowPseudoIdForType() const {
  switch (button_type_) {
    case MediaControlSideButtonType::kXl1:
      return AtomicString("-internal-media-controls-xl1-button");
    case MediaControlSideButtonType::kXl2:
      return AtomicString("-internal-media-controls-xl2-button");
    case MediaControlSideButtonType::kXl3:
      return AtomicString("-internal-media-controls-xl3-button");
    case MediaControlSideButtonType::kXr1:
      return AtomicString("-internal-media-controls-xr1-button");
  }
  NOTREACHED();
}

AtomicString MediaControlSideButtonElement::GetAriaLabelForType() const {
  switch (button_type_) {
    case MediaControlSideButtonType::kXl1:
      return AtomicString(
          GetLocale().QueryString(IDS_MEDIA_OVERFLOW_MENU_DOWNLOAD));
    case MediaControlSideButtonType::kXl2:
      return AtomicString(
          GetLocale().QueryString(IDS_XL_MEDIA_CONTROLS_SMOOTH_PLAYBACK));
    case MediaControlSideButtonType::kXl3:
      return AtomicString(GetLocale().QueryString(
          IDS_AX_MEDIA_SHOW_PLAYBACK_SPEED_MENU_BUTTON));
    case MediaControlSideButtonType::kXr1: {
      auto* video_element = DynamicTo<HTMLVideoElement>(MediaElement());
      const bool is_in_picture_in_picture =
          video_element &&
          PictureInPictureController::IsElementInPictureInPicture(
              video_element);
      return AtomicString(GetLocale().QueryString(
          is_in_picture_in_picture
              ? IDS_AX_MEDIA_EXIT_PICTURE_IN_PICTURE_BUTTON
              : IDS_AX_MEDIA_ENTER_PICTURE_IN_PICTURE_BUTTON));
    }
  }
  NOTREACHED();
}

String MediaControlSideButtonElement::GetVisibleLabelForType() const {
  switch (button_type_) {
    case MediaControlSideButtonType::kXl1:
      return GetLocale().QueryString(IDS_MEDIA_OVERFLOW_MENU_DOWNLOAD);
    case MediaControlSideButtonType::kXl2:
      return GetLocale().QueryString(IDS_XL_MEDIA_CONTROLS_SMOOTH_PLAYBACK);
    case MediaControlSideButtonType::kXl3: {
#if defined(FOR_XL)
      if (const String speed_label =
              XlSideButtonPlaybackSpeedLabel(MediaElement().playbackRate());
          !speed_label.empty()) {
        return speed_label;
      }
#endif
      return GetLocale().QueryString(
          IDS_XL_MEDIA_CONTROLS_PLAYBACK_SPEED_SHORT);
    }
    case MediaControlSideButtonType::kXr1:
      return String();
  }
  NOTREACHED();
}

void MediaControlSideButtonElement::UpdateDisplayType() {
  setAttribute(html_names::kAriaLabelAttr, GetAriaLabelForType());
  SetValue(GetVisibleLabelForType());
  if (button_type_ == MediaControlSideButtonType::kXr1) {
    Element* tooltip = nextElementSibling();
    if (tooltip &&
        tooltip->ShadowPseudoId() ==
            AtomicString("-internal-media-controls-xr1-tooltip")) {
      auto* video_element = DynamicTo<HTMLVideoElement>(MediaElement());
      const bool is_in_picture_in_picture =
          video_element &&
          PictureInPictureController::IsElementInPictureInPicture(
              video_element);
      tooltip->setTextContent(GetLocale().QueryString(
          is_in_picture_in_picture
              ? IDS_XL_MEDIA_CONTROLS_EXIT_PICTURE_IN_PICTURE_TOOLTIP
              : IDS_XL_MEDIA_CONTROLS_ENTER_PICTURE_IN_PICTURE_TOOLTIP));
    }
  }
  MediaControlInputElement::UpdateDisplayType();
}

bool MediaControlSideButtonElement::WillRespondToMouseClickEvents() {
  return true;
}

gfx::Size MediaControlSideButtonElement::GetSizeOrDefault() const {
  if (button_type_ == MediaControlSideButtonType::kXr1) {
    return MediaControlElementsHelper::GetSizeOrDefault(
        *this, gfx::Size(kXrSideButtonWidth, kXrSideButtonHeight));
  }
  return MediaControlElementsHelper::GetSizeOrDefault(
      *this, gfx::Size(kXlSideButtonWidth, kXlSideButtonHeight));
}

const char* MediaControlSideButtonElement::GetNameForHistograms() const {
  switch (button_type_) {
    case MediaControlSideButtonType::kXl1:
      return "Xl1Button";
    case MediaControlSideButtonType::kXl2:
      return "Xl2Button";
    case MediaControlSideButtonType::kXl3:
      return "Xl3Button";
    case MediaControlSideButtonType::kXr1:
      return "Xr1Button";
  }
  NOTREACHED();
}

void MediaControlSideButtonElement::DefaultEventHandler(Event& event) {
#if defined(FOR_XL)
  if (!IsDisabled() && button_type_ == MediaControlSideButtonType::kXl3) {
    if (event.type() == event_type_names::kMouseover) {
      GetMediaControls().ShowPlaybackSpeedListNear(this);
    } else if (event.type() == event_type_names::kMouseout) {
      GetMediaControls().ScheduleHidePlaybackSpeedList();
    }
  }

  if (button_type_ == MediaControlSideButtonType::kXr1) {
    Element* tooltip = nextElementSibling();
    if (tooltip &&
        tooltip->ShadowPseudoId() ==
            AtomicString("-internal-media-controls-xr1-tooltip")) {
      if (!IsDisabled() &&
          (event.type() == event_type_names::kMouseover ||
           event.type() == event_type_names::kMousemove ||
           event.type() == event_type_names::kPointerover ||
           event.type() == event_type_names::kPointermove)) {
        tooltip->setAttribute(html_names::kClassAttr, AtomicString("visible"));
      } else if (event.type() == event_type_names::kMouseout ||
                 event.type() == event_type_names::kPointerout) {
        tooltip->removeAttribute(html_names::kClassAttr);
      }
    }
  }
#endif

  if (!IsDisabled() && (event.type() == event_type_names::kClick ||
                        event.type() == event_type_names::kGesturetap)) {
    event.SetDefaultHandled();
    Platform::Current()->RecordAction(
        UserMetricsAction(UserMetricsActionForType(button_type_)));
#if defined(FOR_XL)
    // xl1/xl2/xl3/xr1: always fire xlcontrolaction on <video>
    // (data-xl-control).
    const bool dispatch_not_prevented =
        DispatchXlControlActionToPage(MediaElement(), button_type_);
    if (button_type_ == MediaControlSideButtonType::kXl1 &&
        ShouldRunXlDefaultAction(dispatch_not_prevented)) {
      GetMediaControls().DownloadMediaIfAvailable();
    } else if (button_type_ == MediaControlSideButtonType::kXl3 &&
               dispatch_not_prevented &&
               GetMediaControls().ShouldShowPlaybackSpeedButton()) {
      GetMediaControls().ShowPlaybackSpeedListNear(this);
    } else if (button_type_ == MediaControlSideButtonType::kXr1 &&
               ShouldRunXlDefaultAction(dispatch_not_prevented)) {
      TogglePictureInPicture(MediaElement());
    }
    // Mouse click leaves :focus on the <input>; hover ring uses :focus in panel
    // buttons but XL side buttons only style :hover — blur to clear stuck ring.
    blur();
#endif
    MaybeRecordInteracted();
  }
  MediaControlInputElement::DefaultEventHandler(event);
}

bool MediaControlSideButtonElement::KeepEventInNode(const Event& event) const {
  return MediaControlElementsHelper::IsUserInteractionEvent(event);
}

}  // namespace blink
