// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_electron_guest.h"

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
#include "content/public/browser/web_contents.h"
#include "third_party/blink/public/common/user_agent/user_agent_metadata.h"
#include "ui/base/page_transition_types.h"
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
  auto contents = content::WebContents::Create(
      content::WebContents::CreateParams(owner->GetBrowserContext()));
  auto guest = std::unique_ptr<XenonElectronGuest>(new XenonElectronGuest(
      contents.get(), container_id, std::move(preferences)));
  const int id = guest->id();
  contents->SetDelegate(guest.get());
  contents->SetUserData(kGuestKey, std::move(guest));
  owner->AttachInnerWebContents(std::move(contents), frame, false);
  return id;
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
  Guests().erase(id_);
}

void XenonElectronGuest::LoadURL(const std::string& url) {
  content::NavigationController::LoadURLParams params{GURL(url)};
  params.transition_type = ui::PAGE_TRANSITION_AUTO_TOPLEVEL;
  params.override_user_agent = content::NavigationController::UA_OVERRIDE_TRUE;
  web_contents()->GetController().LoadURLWithParams(params);
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
  if (command == "get-user-agent") {
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
  if (frame == web_contents()->GetPrimaryMainFrame()) {
    Notify("did-finish-load");
  }
}
void XenonElectronGuest::DidStartLoading() {
  Notify("did-start-loading");
}
void XenonElectronGuest::DidStopLoading() {
  Notify("did-stop-loading");
}
void XenonElectronGuest::DidFinishNavigation(content::NavigationHandle* nav) {
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
