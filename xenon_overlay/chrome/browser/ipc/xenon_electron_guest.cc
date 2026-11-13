// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_electron_guest.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_navigation.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_media_permission.h"
#include "net/base/net_errors.h"

#include <map>
#include <memory>
#include <utility>

#include "base/no_destructor.h"
#include "base/strings/utf_string_conversions.h"
#include "components/embedder_support/user_agent_utils.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/site_instance.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_visibility_policy.h"
#include "third_party/blink/public/common/user_agent/user_agent_metadata.h"
#include "ui/base/page_transition_types.h"
#include "xenon_overlay/buildflags/buildflags.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_session_bridge.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_zoom.h"
#include "xenon_overlay/chrome/browser/ui/xenon_electron_keyboard.h"
#include "xenon_overlay/chrome/browser/xenon_manager.h"

namespace xenon::ipc {
namespace {
const char kGuestKey[] = "xenon-electron-guest";
auto& Guests() {
  static base::NoDestructor<std::map<int, XenonElectronGuest*>> guests;
  return *guests;
}
int NextGuestId() {
  // BrowserWindow native IDs are positive. JS WebContents IDs remain a
  // separate, container-local namespace allocated by the main bootstrap.
  static int next_id = -1;
  return next_id--;
}
}  // namespace

int XenonElectronGuest::Attach(content::RenderFrameHost* frame,
                               const std::string& container_id,
                               base::DictValue preferences) {
  auto* owner = content::WebContents::FromRenderFrameHost(frame);
  auto contents = CreateStandalone(owner->GetBrowserContext(), container_id,
                                   std::move(preferences));
  if (!contents) {
    return 0;
  }
  const int id = FromWebContents(contents.get())->id();
  owner->AttachInnerWebContents(std::move(contents), frame, false);
  return id;
}

std::unique_ptr<content::WebContents> XenonElectronGuest::CreateStandalone(
    content::BrowserContext* context,
    const std::string& container_id,
    base::DictValue preferences) {
  if (!context) {
    return nullptr;
  }
  auto normalized = XenonManager::GetInstance()->NormalizeElectronWebPreferences(
      container_id, preferences);
  if (!normalized.has_value()) {
    return nullptr;
  }
  preferences = std::move(*normalized);
  const std::string* requested_partition = preferences.FindString("partition");
  const std::string partition = requested_partition ? *requested_partition : "";
  ResolveElectronSessionPartition(context, container_id, partition);
  content::WebContents::CreateParams params(context);
  params.site_instance = CreateElectronSessionSiteInstance(
      context, container_id, partition, GURL("about:blank"));
  auto contents = content::WebContents::Create(params);
  if (!contents) {
    return nullptr;
  }
#if BUILDFLAG(IS_WIN) || BUILDFLAG(IS_LINUX)
  content::SetWebContentsOcclusionEnabled(contents.get(), false);
#endif
  auto guest = std::unique_ptr<XenonElectronGuest>(new XenonElectronGuest(
      contents.get(), container_id, std::move(preferences)));
  contents->SetDelegate(guest.get());
  contents->SetUserData(kGuestKey, std::move(guest));
  // Recompute after attaching the identity, before creating the first document
  // and its network factory.
  contents->OnWebPreferencesChanged();
  return contents;
}

XenonElectronGuest* XenonElectronGuest::FromWebContents(
    content::WebContents* contents) {
  return contents ? static_cast<XenonElectronGuest*>(
                        contents->GetUserData(kGuestKey))
                  : nullptr;
}

XenonElectronGuest* XenonElectronGuest::FromId(int id) {
  auto it = Guests().find(id);
  return it == Guests().end() ? nullptr : it->second;
}

XenonElectronGuest::XenonElectronGuest(content::WebContents* contents,
                                       const std::string& container_id,
                                       base::DictValue preferences)
    : content::WebContentsObserver(contents),
      id_(NextGuestId()),
      container_id_(container_id),
      preferences_(std::move(preferences)) {
  Guests().emplace(id_, this);
  if (const auto* ua = preferences_.FindString("userAgent")) {
    blink::UserAgentOverride override;
    override.ua_string_override = *ua;
    contents->SetUserAgentOverride(override, false);
  }
}

XenonElectronGuest::~XenonElectronGuest() {
  ConfigureElectronKeyboardInput(web_contents(), container_id_, id_, false);
  Guests().erase(id_);
}

void XenonElectronGuest::SetCloseCallback(base::RepeatingClosure callback) {
  close_callback_ = std::move(callback);
}

void XenonElectronGuest::CloseContents(content::WebContents* source) {
  Notify("close");
  auto callback = close_callback_;
  if (callback) {
    callback.Run();
  }
}

void XenonElectronGuest::BeforeUnloadFired(content::WebContents* source,
                                           bool proceed,
                                           bool* proceed_to_fire_unload) {
  *proceed_to_fire_unload = proceed;
  if (!proceed) {
    Notify("will-prevent-unload");
  }
}

void XenonElectronGuest::ReadyToCommitNavigation(
    content::NavigationHandle* navigation) {
  if (!navigation->IsSameDocument()) {
    ConfigureElectronKeyboardInput(web_contents(), container_id_, id_, false);
  }
}

void XenonElectronGuest::RenderFrameCreated(content::RenderFrameHost* frame) {
  if (frame->IsActive()) {
    ConfigureElectronKeyboardInput(web_contents(), container_id_, id_,
                                   observes_before_input_, false);
  }
}

void XenonElectronGuest::RequestMediaAccessPermission(
    content::WebContents* contents,
    const content::MediaStreamRequest& request,
    content::MediaResponseCallback callback) {
  if (!HandleElectronMediaAccessRequest(contents, request, callback)) {
    content::WebContentsDelegate::RequestMediaAccessPermission(
        contents, request, std::move(callback));
  }
}

bool XenonElectronGuest::CheckMediaAccessPermission(
    content::RenderFrameHost* frame,
    const url::Origin& origin,
    blink::mojom::MediaStreamType type) {
  if (auto allowed = CheckElectronMediaAccessPermission(frame, origin, type)) {
    return *allowed;
  }
  return content::WebContentsDelegate::CheckMediaAccessPermission(frame, origin,
                                                               type);
}

void XenonElectronGuest::LoadURL(
    const std::string& url,
    base::OnceCallback<void(base::Value, std::string)> reply) {
  content::NavigationController::LoadURLParams params{GURL(url)};
  params.transition_type = ui::PAGE_TRANSITION_AUTO_TOPLEVEL;
  params.override_user_agent = content::NavigationController::UA_OVERRIDE_TRUE;
  LoadElectronURL(web_contents(), params, std::move(reply));
}

bool XenonElectronGuest::Call(const std::string& command,
                              const base::Value& arguments,
                              base::Value* result,
                              std::string* error) {
  auto* contents = web_contents();
  if (!contents) {
    *error = "Guest WebContents has been destroyed";
    return false;
  }
  const auto* options = arguments.GetIfDict();
  *result = base::Value();
  if (command == "observe-before-input") {
#if BUILDFLAG(ENABLE_XENON_SERVICE)
    const auto enabled = options ? options->FindBool("enabled") : std::nullopt;
    if (!enabled) {
      *error = "observe-before-input expects a boolean enabled";
      return false;
    }
    observes_before_input_ = *enabled;
    ConfigureElectronKeyboardInput(contents, container_id_, id_,
                                   observes_before_input_, false);
#else
    *error = "before-input-event requires ENABLE_XENON_SERVICE";
    return false;
#endif
  } else if (command == "set-visual-zoom-limits") {
    return SetElectronVisualZoomLimits(contents, arguments, error);
  } else if (command == "get-zoom-level" || command == "set-zoom-level") {
    return CallElectronPageZoom(contents, command, arguments, result, error);
  } else if (command == "get-os-process-id") {
    const auto& process =
        contents->GetPrimaryMainFrame()->GetProcess()->GetProcess();
    *result =
        base::Value(process.IsValid() ? static_cast<int>(process.Pid()) : 0);
  } else if (command == "get-user-agent") {
    auto ua = contents->GetUserAgentOverride().ua_string_override;
    *result = base::Value(ua.empty() ? embedder_support::GetUserAgent() : ua);
  } else if (command == "set-user-agent" && options &&
             options->FindString("value")) {
    blink::UserAgentOverride override;
    override.ua_string_override = *options->FindString("value");
    contents->SetUserAgentOverride(override, false);
  } else if (command == "reload") {
    contents->GetController().Reload(content::ReloadType::NORMAL, false);
  } else if (command == "stop") {
    contents->Stop();
  } else if (command == "go-back") {
    if (contents->GetController().CanGoBack()) {
      contents->GetController().GoBack();
    }
  } else if (command == "go-forward") {
    if (contents->GetController().CanGoForward()) {
      contents->GetController().GoForward();
    }
  } else if (command == "get-title") {
    *result = base::Value(base::UTF16ToUTF8(contents->GetTitle()));
  } else {
    *error = "Unsupported guest WebContents command: " + command;
    return false;
  }
  return true;
}

void XenonElectronGuest::Notify(const std::string& name, base::Value details) {
  XenonManager::GetInstance()->DispatchElectronWindowEvent(id_, "guest-" + name,
                                                           std::move(details));
}
void XenonElectronGuest::DOMContentLoaded(content::RenderFrameHost* frame) {
  if (frame == web_contents()->GetPrimaryMainFrame()) {
    Notify("dom-ready");
  }
}
void XenonElectronGuest::DidFinishLoad(content::RenderFrameHost* frame,
                                       const GURL& url) {
  if (frame == web_contents()->GetPrimaryMainFrame() && !frame->IsErrorDocument()) {
    Notify("did-finish-load");
  }
}
void XenonElectronGuest::DidFailLoad(content::RenderFrameHost* frame,
                                    const GURL& url,
                                    int error) {
  if (frame == web_contents()->GetPrimaryMainFrame() && !frame->IsErrorDocument()) {
    Notify("did-fail-load",
           base::Value(base::DictValue()
                           .Set("errorCode", error)
                           .Set("errorDescription", net::ErrorToShortString(error))
                           .Set("validatedURL", url.spec())
                           .Set("isMainFrame", true)));
  }
}
void XenonElectronGuest::DidStartLoading() {
  Notify("did-start-loading");
}
void XenonElectronGuest::DidStopLoading() {
  Notify("did-stop-loading");
}
void XenonElectronGuest::DidFinishNavigation(content::NavigationHandle* nav) {
  ConfigureElectronKeyboardInput(web_contents(), container_id_, id_,
                                 observes_before_input_, false);
  if (!nav->IsInPrimaryMainFrame()) {
    return;
  }
  base::DictValue details;
  details.Set("url", nav->GetURL().spec());
  details.Set("isMainFrame", true);
  details.Set("canGoBack", web_contents()->GetController().CanGoBack());
  details.Set("canGoForward", web_contents()->GetController().CanGoForward());
  if (nav->GetNetErrorCode() != 0) {
    details.Set("errorCode", nav->GetNetErrorCode());
    details.Set("errorDescription", net::ErrorToShortString(nav->GetNetErrorCode()));
    details.Set("validatedURL", nav->GetURL().spec());
    Notify("did-fail-load", base::Value(std::move(details)));
  } else if (nav->HasCommitted()) {
    Notify(nav->IsSameDocument() ? "did-navigate-in-page" : "did-navigate",
           base::Value(std::move(details)));
  }
}
void XenonElectronGuest::TitleWasSet(content::NavigationEntry* entry) {
  Notify("page-title-updated",
         base::Value(base::DictValue().Set(
             "title", base::UTF16ToUTF8(web_contents()->GetTitle()))));
}
void XenonElectronGuest::WebContentsDestroyed() {
  Notify("destroyed");
  Guests().erase(id_);
}
bool XenonElectronGuest::IsWebContentsCreationOverridden(
    content::RenderFrameHost* opener,
    content::SiteInstance* source,
    content::mojom::WindowContainerType type,
    const GURL& opener_url,
    const std::string& frame_name,
    const GURL& target_url) {
  Notify("new-window", base::Value(base::DictValue()
                                       .Set("url", target_url.spec())
                                       .Set("frameName", frame_name)
                                       .Set("disposition", "new-window")));
  return true;
}
}  // namespace xenon::ipc
