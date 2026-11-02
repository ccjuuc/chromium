// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/remote/remote_input_router.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/location.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "content/public/browser/devtools_agent_host.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/web_contents.h"
#include "ui/events/keycodes/dom/dom_code.h"
#include "ui/events/keycodes/dom/dom_key.h"
#include "ui/events/keycodes/dom/keycode_converter.h"
#include "ui/events/keycodes/keyboard_code_conversion.h"
#include "url/gurl.h"

#if BUILDFLAG(IS_WIN)
#include <windows.h>

#include <dwmapi.h>

#include "remoting/host/input_injector.h"
#include "remoting/proto/event.pb.h"
#include "remoting/protocol/input_event_tracker.h"
#include "third_party/webrtc/modules/desktop_capture/desktop_geometry.h"
#include "third_party/webrtc/modules/desktop_capture/win/screen_capture_utils.h"
#endif

namespace xenon {

namespace {

constexpr size_t kMaxInputQueue = 128;
constexpr size_t kMaxTextBytes = 4096;
constexpr double kMaxWheelDelta = 10000;

bool IsAllowedPageURL(const GURL& url) {
  // Privileged WebUI/extension/file targets never receive remote CDP input.
  return url.SchemeIsHTTPOrHTTPS() || url.IsAboutBlank();
}

bool IsPointerEvent(std::string_view type) {
  return type == "move" || type == "button" || type == "wheel";
}

bool IsFiniteInRange(const base::DictValue& value,
                     std::string_view key,
                     double minimum,
                     double maximum) {
  const auto number = value.FindDouble(key);
  return number && std::isfinite(*number) && *number >= minimum &&
         *number <= maximum;
}

// Browser shortcuts must not move a page-only session into browser chrome.
bool IsBrowserShortcut(const base::DictValue& event) {
  const std::string& code = *event.FindString("code");
  const int modifiers = event.FindInt("modifiers").value_or(0);
  if (code == "MetaLeft" || code == "MetaRight" || code == "F4" ||
      code == "F6" || code == "F11" || code == "F12") {
    return true;
  }
  if ((modifiers & 1) && (code == "Tab" || code == "Space")) {
    return true;
  }
  if (modifiers & (2 | 4)) {
    return code == "KeyL" || code == "KeyT" || code == "KeyW" ||
           code == "KeyN" || code == "KeyJ" || code == "KeyH" ||
           code == "KeyO" || code == "KeyP" || code == "KeyS" ||
           code == "KeyU" || code == "Tab" || code == "PageUp" ||
           code == "PageDown" || code.starts_with("Digit");
  }
  return false;
}

const char* MouseButtonName(int button) {
  switch (button) {
    case 0:
      return "left";
    case 1:
      return "middle";
    default:
      return "right";
  }
}

int MouseButtonMask(int button) {
  // CDP uses the DOM `buttons` mask (left=1, right=2, middle=4).
  return button == 0 ? 1 : (button == 1 ? 4 : 2);
}

#if BUILDFLAG(IS_WIN)
HWND TargetWindow(const content::DesktopMediaID& target) {
  return reinterpret_cast<HWND>(target.id);
}

bool IsTargetWindowOrOwned(HWND candidate, HWND target) {
  return candidate && (candidate == target || IsChild(target, candidate) ||
                       GetAncestor(candidate, GA_ROOTOWNER) == target);
}

bool IsSystemShortcut(const base::DictValue& event,
                      const remoting::protocol::InputEventTracker& tracker) {
  const std::string& code = *event.FindString("code");
  int modifiers = event.FindInt("modifiers").value_or(0);
  // Native key state comes from previously injected events (and the local
  // keyboard), so a client-provided modifier mask is not authoritative.
  if (tracker.IsKeyPressed(ui::DomCode::ALT_LEFT) ||
      tracker.IsKeyPressed(ui::DomCode::ALT_RIGHT) ||
      (GetAsyncKeyState(VK_MENU) & 0x8000)) {
    modifiers |= 1;
  }
  if (tracker.IsKeyPressed(ui::DomCode::CONTROL_LEFT) ||
      tracker.IsKeyPressed(ui::DomCode::CONTROL_RIGHT) ||
      (GetAsyncKeyState(VK_CONTROL) & 0x8000)) {
    modifiers |= 2;
  }
  return code == "MetaLeft" || code == "MetaRight" ||
         ((modifiers & 1) &&
          (code == "Tab" || code == "F4" || code == "Escape")) ||
         ((modifiers & 2) && code == "Escape") ||
         ((modifiers & 3) == 3 && code == "Delete");
}

bool GetTargetWindowRect(HWND hwnd, RECT* rect) {
  if (!IsWindow(hwnd) || !IsWindowVisible(hwnd) || IsIconic(hwnd)) {
    return false;
  }
  if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, rect,
                                   sizeof(*rect))) &&
      !GetWindowRect(hwnd, rect)) {
    return false;
  }
  return rect->right > rect->left && rect->bottom > rect->top;
}
#endif

}  // namespace

RemoteInputRouter::RemoteInputRouter() = default;

RemoteInputRouter::~RemoteInputRouter() {
  Reset();
}

void RemoteInputRouter::SetControlRevokedCallback(
    base::RepeatingClosure callback) {
  control_revoked_callback_ = std::move(callback);
}

bool RemoteInputRouter::SupportsControl(const content::DesktopMediaID& target) {
  if (target.type == content::DesktopMediaID::TYPE_WEB_CONTENTS) {
    return true;
  }
#if BUILDFLAG(IS_WIN)
  return target.id_type == content::DesktopMediaID::IdType::kPlatformNative &&
         (target.type == content::DesktopMediaID::TYPE_WINDOW ||
          target.type == content::DesktopMediaID::TYPE_SCREEN);
#else
  return false;
#endif
}

void RemoteInputRouter::SetTarget(const content::DesktopMediaID& target,
                                  int generation) {
  Reset();
  if (generation <= 0 || !SupportsControl(target)) {
    return;
  }
  target_ = target;
  generation_ = generation;
  if (target.type == content::DesktopMediaID::TYPE_WEB_CONTENTS) {
    auto* frame = content::RenderFrameHost::FromID(
        target.web_contents_id.render_process_id,
        target.web_contents_id.main_render_frame_id);
    auto* contents =
        frame ? content::WebContents::FromRenderFrameHost(frame) : nullptr;
    if (contents && contents->GetPrimaryMainFrame() == frame) {
      Observe(contents);
    }
  }
#if BUILDFLAG(IS_WIN)
  if (target.type == content::DesktopMediaID::TYPE_SCREEN) {
    std::wstring device_key;
    if (webrtc::IsScreenValid(target.id, &device_key)) {
      screen_device_key_ = std::move(device_key);
    }
  }
#endif
}

bool RemoteInputRouter::IsTargetAvailable() const {
  if (target_.type == content::DesktopMediaID::TYPE_WEB_CONTENTS) {
    return web_contents() && !web_contents()->IsBeingDestroyed() &&
           IsAllowedPageURL(web_contents()->GetLastCommittedURL());
  }
#if BUILDFLAG(IS_WIN)
  if (target_.type == content::DesktopMediaID::TYPE_WINDOW) {
    RECT rect;
    return GetTargetWindowRect(TargetWindow(target_), &rect);
  }
  if (target_.type == content::DesktopMediaID::TYPE_SCREEN) {
    return screen_device_key_ &&
           !webrtc::GetScreenRect(target_.id, screen_device_key_).is_empty();
  }
#endif
  return false;
}

bool RemoteInputRouter::SetEnabled(bool enabled) {
  weak_factory_.InvalidateWeakPtrs();
  control_revocation_pending_ = false;
  // Disable before releases, since CDP can synchronously close its target.
  enabled_ = false;
#if BUILDFLAG(IS_WIN)
  system_target_timer_.Stop();
#endif
  ReleaseAll();
  if (!enabled) {
    return true;
  }
  if (!IsTargetAvailable()) {
    return false;
  }
  if (target_.type == content::DesktopMediaID::TYPE_WEB_CONTENTS) {
    if (!agent_host_) {
      agent_host_ = content::DevToolsAgentHost::GetOrCreateFor(web_contents());
      if (!agent_host_->AttachClient(this)) {
        agent_host_.reset();
        return false;
      }
    }
    web_contents()->GetPrimaryMainFrame()->GetRenderWidgetHost()->Focus();
  }
#if BUILDFLAG(IS_WIN)
  else {
    if (target_.type == content::DesktopMediaID::TYPE_WINDOW) {
      HWND hwnd = TargetWindow(target_);
      if (!IsTargetWindowOrOwned(GetForegroundWindow(), hwnd)) {
        SetForegroundWindow(hwnd);
      }
      if (!IsTargetWindowOrOwned(GetForegroundWindow(), hwnd)) {
        return false;
      }
    }
    if (!system_injector_) {
      // Keyboard, text and mouse do not require Start(), which additionally
      // starts clipboard monitoring/touch injection. Those are not enabled by
      // granting remote pointer/keyboard control.
      auto runner = base::SingleThreadTaskRunner::GetCurrentDefault();
      system_injector_ = remoting::InputInjector::Create(runner, runner);
      system_tracker_ = std::make_unique<remoting::protocol::InputEventTracker>(
          system_injector_.get());
    }
  }
#endif
  enabled_ = true;
#if BUILDFLAG(IS_WIN)
  if (target_.type != content::DesktopMediaID::TYPE_WEB_CONTENTS) {
    system_target_timer_.Start(FROM_HERE, base::Milliseconds(200), this,
                               &RemoteInputRouter::CheckSystemTarget);
  }
#endif
  return true;
}

bool RemoteInputRouter::ValidateInput(const base::DictValue& event) const {
  const auto* type = event.FindString("type");
  if (!type || event.FindInt("generation") != generation_) {
    return false;
  }
  if (*type == "release") {
    return true;
  }
  if (const auto* modifiers = event.Find("modifiers")) {
    if (!modifiers->is_int() || modifiers->GetInt() < 0 ||
        modifiers->GetInt() > 15) {
      return false;
    }
  }
  if (IsPointerEvent(*type)) {
    if (!IsFiniteInRange(event, "x", 0, 1) ||
        !IsFiniteInRange(event, "y", 0, 1)) {
      return false;
    }
    if (*type == "button") {
      const auto button = event.FindInt("button");
      return button && *button >= 0 && *button <= 2 &&
             event.FindBool("down").has_value();
    }
    return *type != "wheel" ||
           (IsFiniteInRange(event, "deltaX", -kMaxWheelDelta, kMaxWheelDelta) &&
            IsFiniteInRange(event, "deltaY", -kMaxWheelDelta, kMaxWheelDelta));
  }
  if (*type == "text") {
    const auto* text = event.FindString("text");
    return text && !text->empty() && text->size() <= kMaxTextBytes &&
           base::IsStringUTF8(*text);
  }
  if (*type == "key") {
    const auto* code = event.FindString("code");
    const auto* key = event.FindString("key");
    return code && code->size() <= 64 && key && key->size() <= 64 &&
           base::IsStringUTF8(*key) && event.FindBool("down").has_value() &&
           ui::KeycodeConverter::CodeStringToDomCode(*code) !=
               ui::DomCode::NONE;
  }
  return false;
}

bool RemoteInputRouter::HandleInput(const base::DictValue& event) {
  if (!ValidateInput(event)) {
    return false;
  }
  const std::string& type = *event.FindString("type");
  if (type == "release") {
    ReleaseAll();
    return true;
  }
  if (!enabled_) {
    return false;
  }
  if (!IsTargetAvailable()) {
    RevokeControl();
    return false;
  }
  if (target_.type != content::DesktopMediaID::TYPE_WEB_CONTENTS) {
    if (!HandleSystemInput(event)) {
      RevokeControl();
      return false;
    }
    return true;
  }
  if (type == "key" && *event.FindBool("down") && IsBrowserShortcut(event)) {
    return true;
  }
  if (page_input_queue_.size() >= kMaxInputQueue) {
    RevokeControl();
    return false;
  }
  // Preserve button/key ordering, but avoid building up stale hover moves.
  if (type == "move" && !page_input_queue_.empty() &&
      page_input_queue_.back().FindString("type") &&
      *page_input_queue_.back().FindString("type") == "move" &&
      page_input_queue_.size() > 1) {
    page_input_queue_.back() = event.Clone();
  } else {
    page_input_queue_.push_back(event.Clone());
  }
  PumpPageInput();
  return true;
}

void RemoteInputRouter::PumpPageInput() {
  if (!enabled_ || !agent_host_ || page_input_queue_.empty() ||
      pending_metrics_id_ || pending_input_id_) {
    return;
  }
  const auto& event = page_input_queue_.front();
  if (IsPointerEvent(*event.FindString("type"))) {
    // Ask the existing page domain for CSS visual-viewport dimensions. This
    // handles browser zoom, pinch zoom and DPI without duplicating Blink math.
    pending_metrics_id_ = ++next_command_id_;
    SendPageCommand(pending_metrics_id_, "Page.getLayoutMetrics", {});
  } else {
    SendPageInput(event, 0, 0);
  }
}

void RemoteInputRouter::SendPageInput(const base::DictValue& event,
                                      double viewport_width,
                                      double viewport_height) {
  const std::string& type = *event.FindString("type");
  const int modifiers = event.FindInt("modifiers").value_or(0);
  base::DictValue params;
  std::string method;
  if (IsPointerEvent(type)) {
    last_page_x_ = *event.FindDouble("x") * std::max(0.0, viewport_width - 1);
    last_page_y_ = *event.FindDouble("y") * std::max(0.0, viewport_height - 1);
    params.Set("x", last_page_x_);
    params.Set("y", last_page_y_);
    params.Set("modifiers", modifiers);
    params.Set("type", type == "wheel" ? "mouseWheel" : "mouseMoved");
    if (type == "button") {
      const int button = *event.FindInt("button");
      const bool down = *event.FindBool("down");
      pressed_page_buttons_ =
          down ? pressed_page_buttons_ | MouseButtonMask(button)
               : pressed_page_buttons_ & ~MouseButtonMask(button);
      params.Set("type", down ? "mousePressed" : "mouseReleased");
      params.Set("button", MouseButtonName(button));
      params.Set("clickCount", 1);
    } else if (type == "wheel") {
      params.Set("deltaX", *event.FindDouble("deltaX"));
      params.Set("deltaY", *event.FindDouble("deltaY"));
    }
    params.Set("buttons", pressed_page_buttons_);
    method = "Input.dispatchMouseEvent";
  } else if (type == "key") {
    const auto& code = *event.FindString("code");
    const auto& key = *event.FindString("key");
    const bool down = *event.FindBool("down");
    if (down) {
      pressed_page_keys_[code] = key;
    } else {
      pressed_page_keys_.erase(code);
    }
    params.Set("type", down ? "keyDown" : "keyUp");
    params.Set("code", code);
    params.Set("key", key);
    params.Set("modifiers", modifiers);
    params.Set("autoRepeat", event.FindBool("repeat").value_or(false));
    params.Set("windowsVirtualKeyCode",
               static_cast<int>(ui::DomCodeToUsLayoutKeyboardCode(
                   ui::KeycodeConverter::CodeStringToDomCode(code))));
    if (down && !(modifiers & (1 | 2 | 4))) {
      if (ui::KeycodeConverter::KeyStringToDomKey(key).IsCharacter()) {
        params.Set("text", key);
      } else if (key == "Enter") {
        params.Set("text", "\r");
      }
    }
    method = "Input.dispatchKeyEvent";
  } else {
    params.Set("text", *event.FindString("text"));
    method = "Input.insertText";
  }
  pending_input_id_ = ++next_command_id_;
  SendPageCommand(pending_input_id_, std::move(method), std::move(params));
}

void RemoteInputRouter::SendPageCommand(int id,
                                        std::string method,
                                        base::DictValue params) {
  if (!agent_host_) {
    return;
  }
  base::DictValue command;
  command.Set("id", id);
  command.Set("method", std::move(method));
  command.Set("params", std::move(params));
  const auto json = base::WriteJson(command);
  if (json) {
    // A command may synchronously close/detach its target.
    auto host = agent_host_;
    host->DispatchProtocolMessage(this, base::as_byte_span(*json));
  }
}

void RemoteInputRouter::DispatchProtocolMessage(
    content::DevToolsAgentHost* agent_host,
    base::span<const uint8_t> message) {
  if (agent_host != agent_host_.get() || message.size() > 1024 * 1024) {
    return;
  }
  auto response = base::JSONReader::ReadDict(base::as_string_view(message),
                                             base::JSON_PARSE_RFC);
  if (!response) {
    return;
  }
  const int id = response->FindInt("id").value_or(0);
  if (!id || (id != pending_metrics_id_ && id != pending_input_id_)) {
    return;
  }
  if (!enabled_ || page_input_queue_.empty() || !IsTargetAvailable() ||
      response->contains("error")) {
    RevokeControl();
    return;
  }
  if (id == pending_metrics_id_) {
    pending_metrics_id_ = 0;
    const auto* viewport =
        response->FindDictByDottedPath("result.cssVisualViewport");
    if (!viewport || !IsFiniteInRange(*viewport, "clientWidth", 1, 100000) ||
        !IsFiniteInRange(*viewport, "clientHeight", 1, 100000)) {
      RevokeControl();
      return;
    }
    SendPageInput(page_input_queue_.front(),
                  *viewport->FindDouble("clientWidth"),
                  *viewport->FindDouble("clientHeight"));
  } else {
    pending_input_id_ = 0;
    page_input_queue_.pop_front();
    PumpPageInput();
  }
}

bool RemoteInputRouter::HandleSystemInput(const base::DictValue& event) {
#if BUILDFLAG(IS_WIN)
  if (!system_tracker_) {
    return false;
  }
  const std::string& type = *event.FindString("type");
  RECT rect;
  HWND hwnd = nullptr;
  if (target_.type == content::DesktopMediaID::TYPE_WINDOW) {
    hwnd = TargetWindow(target_);
    if (!GetTargetWindowRect(hwnd, &rect) ||
        !IsTargetWindowOrOwned(GetForegroundWindow(), hwnd)) {
      return false;
    }
  } else {
    const auto screen = webrtc::GetScreenRect(target_.id, screen_device_key_);
    if (screen.is_empty()) {
      return false;
    }
    rect = {screen.left(), screen.top(), screen.right(), screen.bottom()};
  }
  if (type == "key") {
    // Window-scoped control cannot activate other applications or the shell.
    if (hwnd && *event.FindBool("down") &&
        IsSystemShortcut(event, *system_tracker_)) {
      return true;
    }
    remoting::protocol::KeyEvent key;
    key.set_usb_keycode(static_cast<uint32_t>(
        ui::KeycodeConverter::CodeStringToDomCode(*event.FindString("code"))));
    key.set_pressed(*event.FindBool("down"));
    system_tracker_->InjectKeyEvent(key);
  } else if (type == "text") {
    remoting::protocol::TextEvent text;
    text.set_text(*event.FindString("text"));
    system_tracker_->InjectTextEvent(text);
  } else {
    const int x =
        rect.left + static_cast<int>(std::lround(*event.FindDouble("x") *
                                                 (rect.right - rect.left - 1)));
    const int y =
        rect.top + static_cast<int>(std::lround(*event.FindDouble("y") *
                                                (rect.bottom - rect.top - 1)));
    if (hwnd && !IsTargetWindowOrOwned(WindowFromPoint({x, y}), hwnd)) {
      return false;
    }
    remoting::protocol::MouseEvent mouse;
    // CRD's injector expects coordinates relative to the virtual desktop,
    // including on systems with monitors left/above the primary monitor.
    mouse.set_x(x - GetSystemMetrics(SM_XVIRTUALSCREEN));
    mouse.set_y(y - GetSystemMetrics(SM_YVIRTUALSCREEN));
    if (type == "button") {
      mouse.set_button(static_cast<remoting::protocol::MouseEvent::MouseButton>(
          *event.FindInt("button") + 1));
      mouse.set_button_down(*event.FindBool("down"));
    } else if (type == "wheel") {
      mouse.set_wheel_delta_x(*event.FindDouble("deltaX"));
      mouse.set_wheel_delta_y(-*event.FindDouble("deltaY"));
    }
    system_tracker_->InjectMouseEvent(mouse);
  }
  return true;
#else
  return false;
#endif
}

void RemoteInputRouter::CheckSystemTarget() {
#if BUILDFLAG(IS_WIN)
  if (!IsTargetAvailable() ||
      (target_.type == content::DesktopMediaID::TYPE_WINDOW &&
       !IsTargetWindowOrOwned(GetForegroundWindow(), TargetWindow(target_)))) {
    RevokeControl();
  }
#endif
}

void RemoteInputRouter::RevokeControl(bool reset_target) {
  const bool notify = enabled_ || control_revocation_pending_;
  if (reset_target) {
    Reset();
  } else {
    SetEnabled(false);
  }
  if (!notify) {
    return;
  }
  control_revocation_pending_ = true;
  // A CDP target can close during ReleaseAll or navigation. Notify outside that
  // stack so an owner can reset the session or destroy the router safely.
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&RemoteInputRouter::NotifyControlRevoked,
                                weak_factory_.GetWeakPtr()));
}

void RemoteInputRouter::NotifyControlRevoked() {
  control_revocation_pending_ = false;
  auto callback = control_revoked_callback_;
  if (callback) {
    callback.Run();
  }
}

void RemoteInputRouter::ReleaseAll() {
  page_input_queue_.clear();
  pending_metrics_id_ = 0;
  pending_input_id_ = 0;
  // Move state out before sending, since a target may close synchronously.
  auto keys = std::move(pressed_page_keys_);
  pressed_page_keys_.clear();
  const int buttons = std::exchange(pressed_page_buttons_, 0);
  for (const auto& [code, key] : keys) {
    base::DictValue params;
    params.Set("type", "keyUp");
    params.Set("code", code);
    params.Set("key", key);
    params.Set("modifiers", 0);
    params.Set("windowsVirtualKeyCode",
               static_cast<int>(ui::DomCodeToUsLayoutKeyboardCode(
                   ui::KeycodeConverter::CodeStringToDomCode(code))));
    SendPageCommand(++next_command_id_, "Input.dispatchKeyEvent",
                    std::move(params));
  }
  for (int button = 0; button != 3; ++button) {
    if (!(buttons & MouseButtonMask(button))) {
      continue;
    }
    base::DictValue params;
    params.Set("type", "mouseReleased");
    params.Set("x", last_page_x_);
    params.Set("y", last_page_y_);
    params.Set("button", MouseButtonName(button));
    params.Set("buttons", 0);
    params.Set("clickCount", 1);
    SendPageCommand(++next_command_id_, "Input.dispatchMouseEvent",
                    std::move(params));
  }
#if BUILDFLAG(IS_WIN)
  if (system_tracker_) {
    system_tracker_->ReleaseAll();
  }
#endif
}

void RemoteInputRouter::DetachPage() {
  if (agent_host_) {
    auto host = std::move(agent_host_);
    host->DetachClient(this);
  }
}

void RemoteInputRouter::Reset() {
  SetEnabled(false);
  DetachPage();
  Observe(nullptr);
  target_ = {};
  generation_ = 0;
#if BUILDFLAG(IS_WIN)
  system_target_timer_.Stop();
  system_tracker_.reset();
  system_injector_.reset();
  screen_device_key_.reset();
#endif
}

void RemoteInputRouter::AgentHostClosed(content::DevToolsAgentHost* host) {
  agent_host_.reset();
  RevokeControl();
}

bool RemoteInputRouter::MayAttachToURL(const GURL& url, bool is_webui) {
  return !is_webui && IsAllowedPageURL(url);
}

bool RemoteInputRouter::MayAttachToRenderFrameHost(
    content::RenderFrameHost* frame) {
  return frame && web_contents() &&
         content::WebContents::FromRenderFrameHost(frame) == web_contents() &&
         IsAllowedPageURL(frame->GetLastCommittedURL());
}

bool RemoteInputRouter::IsTrusted() {
  return false;
}
bool RemoteInputRouter::MayAccessAllCookies() {
  return false;
}
bool RemoteInputRouter::MayReadLocalFiles() {
  return false;
}
bool RemoteInputRouter::MayWriteLocalFiles() {
  return false;
}
bool RemoteInputRouter::AllowUnsafeOperations() {
  return false;
}

void RemoteInputRouter::PrimaryPageChanged(content::Page& page) {
  // Clear held input and pending metrics from the old document. The tab-level
  // agent follows ordinary web navigations; privileged destinations lose input.
  if (!IsTargetAvailable()) {
    RevokeControl();
    DetachPage();
  } else {
    ReleaseAll();
  }
}

void RemoteInputRouter::WebContentsDestroyed() {
  RevokeControl(/*reset_target=*/true);
}

}  // namespace xenon
