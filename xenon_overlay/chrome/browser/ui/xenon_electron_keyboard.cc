// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ui/xenon_electron_keyboard.h"

#include "xenon_overlay/buildflags/buildflags.h"

#if BUILDFLAG(ENABLE_XENON_SERVICE)
#include <set>
#include <utility>

#include "base/functional/bind.h"
#include "base/values.h"
#include "content/browser/renderer_host/render_widget_host_impl.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "ui/events/keycodes/dom/keycode_converter.h"
#include "xenon_overlay/chrome/browser/xenon_manager.h"
#endif

namespace xenon {

void ConfigureElectronKeyboardInput(content::WebContents* contents,
                                    const std::string& container,
                                    int32_t native_id,
                                    bool observe,
                                    bool cancel_pending) {
#if BUILDFLAG(ENABLE_XENON_SERVICE)
  if (!contents) {
    return;
  }
  content::RenderWidgetHostImpl::AsyncKeyboardEventFilter filter;
  if (observe) {
    filter = base::BindRepeating(
        [](std::string owner, uint64_t expected_generation, int32_t id,
           const input::NativeWebKeyboardEvent& event,
           base::OnceCallback<void(bool)> done) {
          using Event = blink::WebInputEvent;
          if (event.GetType() == Event::Type::kChar) {
            std::move(done).Run(false);
            return;
          }
          const int modifiers = event.GetModifiers();
          base::ListValue names;
          for (const auto& [mask, name] :
               {std::pair{Event::kShiftKey, "shift"},
                {Event::kControlKey, "control"},
                {Event::kAltKey, "alt"},
                {Event::kMetaKey, "meta"},
                {Event::kIsAutoRepeat, "isAutoRepeat"},
                {Event::kCapsLockOn, "capsLock"},
                {Event::kNumLockOn, "numLock"},
                {Event::kIsKeyPad, "isKeypad"},
                {Event::kIsLeft, "left"},
                {Event::kIsRight, "right"}}) {
            if (modifiers & mask) {
              names.Append(name);
            }
          }
          auto details =
              base::DictValue()
                  .Set("type", event.GetType() == Event::Type::kKeyUp
                                   ? "keyUp"
                                   : "keyDown")
                  .Set("key", ui::KeycodeConverter::DomKeyToKeyString(
                                  ui::DomKey(event.dom_key)))
                  .Set("code", ui::KeycodeConverter::DomCodeToCodeString(
                                   static_cast<ui::DomCode>(event.dom_code)))
                  .Set("isAutoRepeat", !!(modifiers & Event::kIsAutoRepeat))
                  .Set("isComposing", !!(modifiers & Event::kIsComposing))
                  .Set("shift", !!(modifiers & Event::kShiftKey))
                  .Set("control", !!(modifiers & Event::kControlKey))
                  .Set("alt", !!(modifiers & Event::kAltKey))
                  .Set("meta", !!(modifiers & Event::kMetaKey))
                  .Set("location", modifiers & Event::kIsKeyPad  ? 3
                                   : modifiers & Event::kIsLeft  ? 1
                                   : modifiers & Event::kIsRight ? 2
                                                                 : 0)
                  .Set("modifiers", std::move(names));
          XenonManager::GetInstance()->FilterElectronKeyboardEvent(
              owner, expected_generation, id, base::Value(std::move(details)),
              std::move(done));
        },
        container, XenonManager::GetInstance()->service_generation(container),
        native_id);
  }
  std::set<content::RenderWidgetHost*> widgets;
  contents->ForEachRenderFrameHost([&](content::RenderFrameHost* frame) {
    auto* widget = frame->GetRenderWidgetHost();
    if ((!observe || frame->IsActive()) && widgets.insert(widget).second) {
      content::RenderWidgetHostImpl::From(widget)->SetAsyncKeyboardEventFilter(
          filter, cancel_pending);
    }
  });
#endif
}

}  // namespace xenon
