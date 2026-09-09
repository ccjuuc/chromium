// Copyright 2021 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/modules/media_controls/elements/media_control_playback_speed_list_element.h"

#include <array>
#include <cmath>

#include "base/metrics/histogram_functions.h"
#include "third_party/blink/public/strings/grit/blink_strings.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_scroll_into_view_options.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_union_boolean_scrollintoviewoptions.h"
#include "third_party/blink/renderer/core/css/css_property_names.h"
#include "third_party/blink/renderer/core/css/css_style_declaration.h"
#include "third_party/blink/renderer/core/css_value_keywords.h"
#include "third_party/blink/renderer/core/html/html_element.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/dom/dom_token_list.h"
#include "third_party/blink/renderer/core/dom/events/event.h"
#include "third_party/blink/renderer/core/dom/events/event_dispatch_forbidden_scope.h"
#include "third_party/blink/renderer/core/dom/focus_params.h"
#include "third_party/blink/renderer/core/dom/frame_request_callback_collection.h"
#include "third_party/blink/renderer/core/dom/text.h"
#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/renderer/core/geometry/dom_rect.h"
#include "third_party/blink/renderer/core/html/forms/html_input_element.h"
#include "third_party/blink/renderer/core/html/forms/html_label_element.h"
#include "third_party/blink/renderer/core/html/html_span_element.h"
#include "third_party/blink/renderer/core/html/media/html_media_element.h"
#include "third_party/blink/renderer/core/html_names.h"
#include "third_party/blink/renderer/core/input_type_names.h"
#include "third_party/blink/renderer/core/keywords.h"
#include "third_party/blink/renderer/modules/media_controls/elements/media_control_div_element.h"
#include "third_party/blink/renderer/modules/media_controls/media_controls_impl.h"
#include "third_party/blink/renderer/platform/heap/garbage_collected.h"
#include "third_party/blink/renderer/platform/text/platform_locale.h"
#include "third_party/blink/renderer/platform/wtf/functional.h"
#include "ui/strings/grit/ax_strings.h"

namespace blink {

namespace {

// This enum is used to record histograms. Do not reorder.
enum class MediaControlsPlaybackSpeed {
  k0_25X = 0,
  k0_5X,
  k0_75X,
  k1X,
  k1_25X,
  k1_5X,
  k1_75X,
  k2X,
  kMaxValue = k2X,
};

void RecordPlaybackSpeedUMA(MediaControlsPlaybackSpeed playback_speed) {
  base::UmaHistogramEnumeration("Media.Controls.PlaybackSpeed", playback_speed);
}

struct PlaybackSpeed {
  const int display_name;
  const double playback_rate;
};

constexpr auto kPlaybackSpeeds = std::to_array<PlaybackSpeed>({
    {IDS_MEDIA_OVERFLOW_MENU_PLAYBACK_SPEED_0_25X_TITLE, 0.25},
    {IDS_MEDIA_OVERFLOW_MENU_PLAYBACK_SPEED_0_5X_TITLE, 0.5},
    {IDS_MEDIA_OVERFLOW_MENU_PLAYBACK_SPEED_0_75X_TITLE, 0.75},
    {IDS_MEDIA_OVERFLOW_MENU_PLAYBACK_SPEED_NORMAL_TITLE, 1.0},
    {IDS_MEDIA_OVERFLOW_MENU_PLAYBACK_SPEED_1_25X_TITLE, 1.25},
    {IDS_MEDIA_OVERFLOW_MENU_PLAYBACK_SPEED_1_5X_TITLE, 1.5},
    {IDS_MEDIA_OVERFLOW_MENU_PLAYBACK_SPEED_1_75X_TITLE, 1.75},
    {IDS_MEDIA_OVERFLOW_MENU_PLAYBACK_SPEED_2X_TITLE, 2.0},
});

#if defined(FOR_XL)
constexpr auto kXlSidePlaybackSpeeds = std::to_array<PlaybackSpeed>({
    {IDS_MEDIA_OVERFLOW_MENU_PLAYBACK_SPEED_0_5X_TITLE, 0.5},
    {IDS_MEDIA_OVERFLOW_MENU_PLAYBACK_SPEED_NORMAL_TITLE, 1.0},
    {IDS_MEDIA_OVERFLOW_MENU_PLAYBACK_SPEED_1_25X_TITLE, 1.25},
    {IDS_MEDIA_OVERFLOW_MENU_PLAYBACK_SPEED_1_5X_TITLE, 1.5},
    {IDS_MEDIA_OVERFLOW_MENU_PLAYBACK_SPEED_2X_TITLE, 2.0},
    {IDS_MEDIA_OVERFLOW_MENU_PLAYBACK_SPEED_NORMAL_TITLE, 3.0},
    {IDS_MEDIA_OVERFLOW_MENU_PLAYBACK_SPEED_NORMAL_TITLE, 4.0},
});

String XlSidePlaybackSpeedLabel(double playback_rate) {
  if (playback_rate == 1.0) {
    return "1.0x";
  }
  if (playback_rate == std::floor(playback_rate)) {
    return String::NumberToStringEcmaScript(playback_rate) + ".0x";
  }
  return String::NumberToStringEcmaScript(playback_rate) + "x";
}

const AtomicString& XlPlaybackSpeedListItemPseudo() {
  DEFINE_STATIC_LOCAL(AtomicString, k_xl_item_pseudo,
                      ("-internal-media-controls-xl-playback-speed-list-item"));
  return k_xl_item_pseudo;
}

const AtomicString& XlOpenClassName() {
  DEFINE_STATIC_LOCAL(AtomicString, k_open, ("open"));
  return k_open;
}
#endif

const QualifiedName& PlaybackRateAttrName() {
  // Save the playback rate in an attribute.
  DEFINE_STATIC_LOCAL(QualifiedName, playback_rate_attr,
                      (AtomicString("data-playback-rate")));
  return playback_rate_attr;
}

}  // anonymous namespace

#if defined(FOR_XL)
class MediaControlPlaybackSpeedListElement::OpenAnimationFrameCallback final
    : public FrameCallback {
 public:
  explicit OpenAnimationFrameCallback(
      MediaControlPlaybackSpeedListElement* list)
      : list_(list) {}

  OpenAnimationFrameCallback(const OpenAnimationFrameCallback&) = delete;
  OpenAnimationFrameCallback& operator=(const OpenAnimationFrameCallback&) =
      delete;

  void Invoke(double) override {
    if (list_) {
      list_->EnsureXlOpenAnimation();
    }
  }

  void Trace(Visitor* visitor) const override {
    visitor->Trace(list_);
    FrameCallback::Trace(visitor);
  }

 private:
  Member<MediaControlPlaybackSpeedListElement> list_;
};
#endif

class MediaControlPlaybackSpeedListElement::RequestAnimationFrameCallback final
    : public FrameCallback {
 public:
  explicit RequestAnimationFrameCallback(
      MediaControlPlaybackSpeedListElement* list)
      : list_(list) {}

  RequestAnimationFrameCallback(const RequestAnimationFrameCallback&) = delete;
  RequestAnimationFrameCallback& operator=(
      const RequestAnimationFrameCallback&) = delete;

  void Invoke(double) override {
#if defined(FOR_XL)
    if (list_->IsXlSidePopup()) {
      // Defer adding `.open` one frame so the collapsed width is painted first
      // and the 300ms ease-out width/opacity transition can run (稿 4672:21965).
      if (!list_->classList().contains(XlOpenClassName())) {
        list_->GetDocument().RequestAnimationFrame(
            MakeGarbageCollected<OpenAnimationFrameCallback>(list_),
            FrameCallbackType::kInternal);
      }
    }
#endif
    list_->CenterCheckedItem();
  }

  void Trace(Visitor* visitor) const override {
    visitor->Trace(list_);
    FrameCallback::Trace(visitor);
  }

 private:
  Member<MediaControlPlaybackSpeedListElement> list_;
};

MediaControlPlaybackSpeedListElement::MediaControlPlaybackSpeedListElement(
    MediaControlsImpl& media_controls)
    : MediaControlPopupMenuElement(media_controls) {
  setAttribute(html_names::kRoleAttr, AtomicString("menu"));
  setAttribute(html_names::kAriaLabelAttr,
               AtomicString(GetLocale().QueryString(
                   IDS_MEDIA_OVERFLOW_MENU_PLAYBACK_SPEED_SUBMENU_TITLE)));
  SetShadowPseudoId(
      AtomicString("-internal-media-controls-playback-speed-list"));
}

bool MediaControlPlaybackSpeedListElement::WillRespondToMouseClickEvents() {
  return true;
}

Element* MediaControlPlaybackSpeedListElement::PopupAnchor() const {
  if (Element* anchor = GetMediaControls().PlaybackSpeedPopupAnchor()) {
    return anchor;
  }
  return MediaControlPopupMenuElement::PopupAnchor();
}

#if defined(FOR_XL)
bool MediaControlPlaybackSpeedListElement::IsXlSidePopup() const {
  return GetMediaControls().PlaybackSpeedPopupAnchor();
}
#endif  // defined(FOR_XL)

void MediaControlPlaybackSpeedListElement::SetPosition() {
#if defined(FOR_XL)
  if (IsXlSidePopup()) {
    PositionXlBesideButton();
    return;
  }
#endif
  MediaControlPopupMenuElement::SetPosition();
}

#if defined(FOR_XL)
void MediaControlPlaybackSpeedListElement::PositionXlBesideButton() {
  // CSS `video::-internal-media-controls-xl3-anchor` does not match this
  // custom pseudo; without position:relative the list's containing block is
  // the left column, so top:-10px lines up with 下载 instead of 倍速.
  if (auto* parent = DynamicTo<HTMLElement>(parentNode())) {
    parent->SetInlineStyleProperty(CSSPropertyID::kPosition,
                                   CSSValueID::kRelative, true);
    parent->SetInlineStyleProperty(CSSPropertyID::kOverflow,
                                   CSSValueID::kVisible, true);
  }

  LocalDOMWindow* dom_window = GetDocument().domWindow();
  if (!dom_window) {
    return;
  }
  static const char kImportant[] = "important";
  style()->setProperty(dom_window, "position", "absolute", kImportant,
                       ASSERT_NO_EXCEPTION);
  // 10px hotzone above the 48px capsule so it lines up with the 48px tile.
  style()->setProperty(dom_window, "top", "-10px", kImportant,
                       ASSERT_NO_EXCEPTION);
  // 6px gap minus 10px hotzone, relative to the 48px button box.
  style()->setProperty(dom_window, "left", "calc(100% - 4px)", kImportant,
                       ASSERT_NO_EXCEPTION);
  style()->setProperty(dom_window, "right", "auto", kImportant,
                       ASSERT_NO_EXCEPTION);
  style()->setProperty(dom_window, "bottom", "auto", kImportant,
                       ASSERT_NO_EXCEPTION);
  style()->setProperty(dom_window, "margin", "0px", kImportant,
                       ASSERT_NO_EXCEPTION);
}
#endif

void MediaControlPlaybackSpeedListElement::SetIsWanted(bool wanted) {
#if defined(FOR_XL)
  if (wanted && IsXlSidePopup()) {
    CancelXlCollapseAnimation();
    RefreshPlaybackSpeedListMenu();
    classList().Remove(XlOpenClassName());
    // Same as xr1 tooltip: stay in the overlay tree, no top-layer popover.
    removeAttribute(html_names::kPopoverAttr);
    MediaControlDivElement::SetIsWanted(true);
    PositionXlBesideButton();
    return;
  }

  // Collapse with width/opacity transition; hide after 300ms.
  if (!wanted && IsWanted() && IsXlSidePopup() &&
      classList().contains(XlOpenClassName())) {
    classList().Remove(XlOpenClassName());
    const int generation = ++xl_collapse_generation_;
    GetDocument()
        .GetTaskRunner(TaskType::kMediaElementEvent)
        ->PostDelayedTask(
            FROM_HERE,
            BindOnce(
                [](MediaControlPlaybackSpeedListElement* self, int generation) {
                  if (!self || self->xl_collapse_generation_ != generation) {
                    return;
                  }
                  self->FinishXlCollapseAnimation();
                },
                WrapWeakPersistent(this), generation),
            base::Milliseconds(300));
    return;
  }

  CancelXlCollapseAnimation();
  if (wanted) {
    setAttribute(html_names::kPopoverAttr, keywords::kAuto);
  }
#endif

  if (wanted) {
    RefreshPlaybackSpeedListMenu();
  }

  if (!wanted) {
    GetMediaControls().ClearPlaybackSpeedPopupAnchor();
    if (!GetMediaControls().OverflowMenuIsWanted()) {
      GetMediaControls().CloseOverflowMenu();
    }
  }

  MediaControlPopupMenuElement::SetIsWanted(wanted);
}

#if defined(FOR_XL)
void MediaControlPlaybackSpeedListElement::EnsureXlOpenAnimation() {
  CancelXlCollapseAnimation();
  PositionXlBesideButton();
  if (!classList().contains(XlOpenClassName())) {
    classList().Add(XlOpenClassName());
  }
}

void MediaControlPlaybackSpeedListElement::CancelXlCollapseAnimation() {
  ++xl_collapse_generation_;
}

void MediaControlPlaybackSpeedListElement::FinishXlCollapseAnimation() {
  classList().Remove(XlOpenClassName());
  GetMediaControls().ClearPlaybackSpeedPopupAnchor();
  if (!GetMediaControls().OverflowMenuIsWanted()) {
    GetMediaControls().CloseOverflowMenu();
  }
  MediaControlDivElement::SetIsWanted(false);
}
#endif

void MediaControlPlaybackSpeedListElement::ApplyPlaybackRate(
    double playback_rate) {
  MediaElement().setDefaultPlaybackRate(playback_rate);
  MediaElement().setPlaybackRate(playback_rate);

  if (playback_rate == 0.25) {
    RecordPlaybackSpeedUMA(MediaControlsPlaybackSpeed::k0_25X);
  } else if (playback_rate == 0.5) {
    RecordPlaybackSpeedUMA(MediaControlsPlaybackSpeed::k0_5X);
  } else if (playback_rate == 0.75) {
    RecordPlaybackSpeedUMA(MediaControlsPlaybackSpeed::k0_75X);
  } else if (playback_rate == 1.0) {
    RecordPlaybackSpeedUMA(MediaControlsPlaybackSpeed::k1X);
  } else if (playback_rate == 1.25) {
    RecordPlaybackSpeedUMA(MediaControlsPlaybackSpeed::k1_25X);
  } else if (playback_rate == 1.5) {
    RecordPlaybackSpeedUMA(MediaControlsPlaybackSpeed::k1_5X);
  } else if (playback_rate == 1.75) {
    RecordPlaybackSpeedUMA(MediaControlsPlaybackSpeed::k1_75X);
  } else if (playback_rate == 2.0) {
    RecordPlaybackSpeedUMA(MediaControlsPlaybackSpeed::k2X);
  }
  // XL-only rates (e.g. 3x, 4x, 5x, 8x) have no UMA bucket; playback still
  // applies.
#if defined(FOR_XL)
  GetMediaControls().UpdateXl3PlaybackSpeedLabel();
#endif
}

#if defined(FOR_XL)
Element*
MediaControlPlaybackSpeedListElement::XlPlaybackSpeedListItemFromEventTarget(
    Node* target) const {
  for (Node* node = target; node; node = node->parentNode()) {
    auto* element = DynamicTo<Element>(node);
    if (!element ||
        element->ShadowPseudoId() != XlPlaybackSpeedListItemPseudo()) {
      continue;
    }
    if (element->hasAttribute(PlaybackRateAttrName())) {
      return element;
    }
  }
  return nullptr;
}

void MediaControlPlaybackSpeedListElement::UpdateXlSideCheckedItem(
    Element* selected_item) {
  for (Node* child = firstChild(); child; child = child->nextSibling()) {
    auto* item = DynamicTo<Element>(child);
    if (!item) {
      continue;
    }
    if (item == selected_item) {
      item->setAttribute(html_names::kAriaCheckedAttr, keywords::kTrue);
      checked_item_ = item;
    } else {
      item->removeAttribute(html_names::kAriaCheckedAttr);
    }
  }
}
#endif

void MediaControlPlaybackSpeedListElement::DefaultEventHandler(Event& event) {
#if defined(FOR_XL)
  if (IsXlSidePopup()) {
    if (event.type() == event_type_names::kMouseover) {
      GetMediaControls().CancelScheduledPlaybackSpeedListHide();
    } else if (event.type() == event_type_names::kMouseout) {
      GetMediaControls().ScheduleHidePlaybackSpeedList();
    }
  }
#endif

  if (event.type() == event_type_names::kClick) {
#if defined(FOR_XL)
    if (IsXlSidePopup()) {
      if (Element* item = XlPlaybackSpeedListItemFromEventTarget(
              event.target()->ToNode())) {
        const double playback_rate =
            item->GetFloatingPointAttribute(PlaybackRateAttrName());
        UpdateXlSideCheckedItem(item);
        ApplyPlaybackRate(playback_rate);
        SetIsWanted(false);
        event.SetDefaultHandled();
        return;
      }
    }
#endif
    // Back button in the overflow submenu.
    GetMediaControls().ToggleOverflowMenu();
    event.SetDefaultHandled();
  } else if (event.type() == event_type_names::kChange) {
    Node* target = event.RawTarget()->ToNode();
    if (!target || !target->IsElementNode()) {
      return;
    }

    const double playback_rate =
        To<Element>(target)->GetFloatingPointAttribute(PlaybackRateAttrName());
    ApplyPlaybackRate(playback_rate);
    SetIsWanted(false);
    event.SetDefaultHandled();
  }
#if defined(FOR_XL)
  // RefreshPlaybackSpeedListMenu() replaces the old focused item. Its
  // focusout posts HideIfNotFocused(); for a UA-shadow popover,
  // Document::FocusedElement() resolves to the host <video>, so that task
  // incorrectly closes the newly opened hover menu. Mouseout and selection
  // already close the XL side popup.
  if (IsXlSidePopup() && event.type() == event_type_names::kFocusout) {
    return;
  }
#endif
  MediaControlPopupMenuElement::DefaultEventHandler(event);
}

Element* MediaControlPlaybackSpeedListElement::CreatePlaybackSpeedListItem(
    const int display_name,
    const double playback_rate,
    const String* custom_label,
    bool xl_side_item) {
  auto* playback_speed_item =
      MakeGarbageCollected<HTMLLabelElement>(GetDocument());
  playback_speed_item->SetShadowPseudoId(
      xl_side_item
          ? AtomicString("-internal-media-controls-xl-playback-speed-list-item")
          : AtomicString("-internal-media-controls-playback-speed-list-item"));
  const String playback_speed_label =
      custom_label ? *custom_label : GetLocale().QueryString(display_name);
  auto* playback_speed_label_span =
      MakeGarbageCollected<HTMLSpanElement>(GetDocument());
  playback_speed_label_span->setInnerText(playback_speed_label);
  playback_speed_label_span->setAttribute(html_names::kAriaHiddenAttr,
                                          keywords::kTrue);
  playback_speed_item->setAttribute(html_names::kAriaLabelAttr,
                                    AtomicString(playback_speed_label));
  playback_speed_item->setTabIndex(0);

#if defined(FOR_XL)
  if (xl_side_item) {
    playback_speed_item->SetFloatingPointAttribute(PlaybackRateAttrName(),
                                                   playback_rate);
    if (playback_rate == MediaElement().playbackRate()) {
      playback_speed_item->setAttribute(html_names::kAriaCheckedAttr,
                                        keywords::kTrue);
      checked_item_ = playback_speed_item;
    }
    playback_speed_item->ParserAppendChild(playback_speed_label_span);
    return playback_speed_item;
  }
#endif

  auto* playback_speed_item_input =
      MakeGarbageCollected<HTMLInputElement>(GetDocument());
  playback_speed_item_input->SetShadowPseudoId(
      AtomicString("-internal-media-controls-playback-speed-list-item-input"));
  playback_speed_item_input->setAttribute(html_names::kAriaHiddenAttr,
                                          keywords::kTrue);
  playback_speed_item_input->setType(input_type_names::kCheckbox);
  playback_speed_item_input->SetFloatingPointAttribute(PlaybackRateAttrName(),
                                                       playback_rate);
  if (playback_rate == MediaElement().playbackRate()) {
    playback_speed_item_input->SetChecked(true);
    playback_speed_item->setAttribute(html_names::kAriaCheckedAttr,
                                      keywords::kTrue);
    checked_item_ = playback_speed_item;
  }
  playback_speed_item_input->setTabIndex(-1);
  playback_speed_item->ParserAppendChild(playback_speed_label_span);
  playback_speed_item->ParserAppendChild(playback_speed_item_input);

  return playback_speed_item;
}

Element* MediaControlPlaybackSpeedListElement::CreatePlaybackSpeedHeaderItem() {
  auto* header_item = MakeGarbageCollected<HTMLLabelElement>(GetDocument());
  header_item->SetShadowPseudoId(
      AtomicString("-internal-media-controls-playback-speed-list-header"));
  header_item->ParserAppendChild(
      Text::Create(GetDocument(),
                   GetLocale().QueryString(
                       IDS_MEDIA_OVERFLOW_MENU_PLAYBACK_SPEED_SUBMENU_TITLE)));
  header_item->setAttribute(html_names::kRoleAttr, AtomicString("button"));
  header_item->setAttribute(html_names::kAriaLabelAttr,
                            AtomicString(GetLocale().QueryString(
                                IDS_AX_MEDIA_BACK_TO_OPTIONS_BUTTON)));
  header_item->setTabIndex(0);
  return header_item;
}

void MediaControlPlaybackSpeedListElement::RefreshPlaybackSpeedListMenu() {
  EventDispatchForbiddenScope::AllowUserAgentEvents allow_events;
  RemoveChildren();

#if defined(FOR_XL)
  const bool xl_side_popup = IsXlSidePopup();
  SetShadowPseudoId(
      xl_side_popup
          ? AtomicString("-internal-media-controls-xl-playback-speed-list")
          : AtomicString("-internal-media-controls-playback-speed-list"));
  if (!xl_side_popup) {
    ParserAppendChild(CreatePlaybackSpeedHeaderItem());
  }
#else
  ParserAppendChild(CreatePlaybackSpeedHeaderItem());
#endif

  checked_item_ = nullptr;

#if defined(FOR_XL)
  const unsigned speed_count = xl_side_popup ? std::size(kXlSidePlaybackSpeeds)
                                             : std::size(kPlaybackSpeeds);
#else
  const unsigned speed_count = std::size(kPlaybackSpeeds);
#endif

  // Construct a menu for playback speeds.
  for (unsigned i = 0; i < speed_count; i++) {
#if defined(FOR_XL)
    const PlaybackSpeed& playback_speed =
        xl_side_popup ? kXlSidePlaybackSpeeds[i] : kPlaybackSpeeds[i];
#else
    const PlaybackSpeed& playback_speed = kPlaybackSpeeds[i];
#endif
#if defined(FOR_XL)
    String xl_label;
    const String* custom_label = nullptr;
    if (xl_side_popup) {
      xl_label = XlSidePlaybackSpeedLabel(playback_speed.playback_rate);
      custom_label = &xl_label;
    }
    auto* playback_speed_item = CreatePlaybackSpeedListItem(
        playback_speed.display_name, playback_speed.playback_rate, custom_label,
        xl_side_popup);
#else
    auto* playback_speed_item = CreatePlaybackSpeedListItem(
        playback_speed.display_name, playback_speed.playback_rate);
#endif
    playback_speed_item->setAttribute(html_names::kAriaSetsizeAttr,
                                      AtomicString::Number(speed_count));
    playback_speed_item->setAttribute(html_names::kAriaPosinsetAttr,
                                      AtomicString::Number(i + 1));
    playback_speed_item->setAttribute(
        html_names::kRoleAttr,
#if defined(FOR_XL)
        xl_side_popup ? AtomicString("menuitemradio") :
#endif
                      AtomicString("menuitemcheckbox"));
    ParserAppendChild(playback_speed_item);
  }
  RequestAnimationFrameCallback* callback =
      MakeGarbageCollected<RequestAnimationFrameCallback>(this);
  GetDocument().RequestAnimationFrame(callback, FrameCallbackType::kInternal);
}

void MediaControlPlaybackSpeedListElement::CenterCheckedItem() {
#if defined(FOR_XL)
  // Horizontal side menu: scrollIntoView would move the left column, not
  // the checked chip. Alignment is handled by PositionXlBesideButton().
  if (IsXlSidePopup()) {
    return;
  }
#endif
  if (!checked_item_) {
    return;
  }
  ScrollIntoViewOptions* options = ScrollIntoViewOptions::Create();
  options->setBlock(V8ScrollLogicalPosition::Enum::kCenter);
  checked_item_->scrollIntoViewWithOptions(options);
  checked_item_->Focus(FocusParams(FocusTrigger::kUserGesture));
}

void MediaControlPlaybackSpeedListElement::Trace(Visitor* visitor) const {
  visitor->Trace(checked_item_);
  MediaControlPopupMenuElement::Trace(visitor);
}

}  // namespace blink
