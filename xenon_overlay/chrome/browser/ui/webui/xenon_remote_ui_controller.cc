// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ui/webui/xenon_remote_ui_controller.h"

#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/notreached.h"
#include "base/values.h"
#include "build/build_config.h"
#include "chrome/browser/media/webrtc/capture_policy_utils.h"
#include "chrome/browser/media/webrtc/desktop_media_list.h"
#include "chrome/browser/media/webrtc/desktop_media_picker_controller.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface_iterator.h"
#include "content/public/browser/desktop_media_id.h"
#include "content/public/browser/desktop_streams_registry.h"
#include "content/public/browser/global_routing_id.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_data_source.h"
#include "content/public/browser/web_ui_message_handler.h"
#include "content/public/common/url_constants.h"
#include "services/network/public/mojom/content_security_policy.mojom.h"
#include "ui/base/base_window.h"
#include "url/gurl.h"
#include "url/origin.h"
#include "xenon_overlay/chrome/browser/remote/remote_input_router.h"
#include "xenon_overlay/resources/grit/xenon_resources.h"

#if BUILDFLAG(IS_WIN)
#include "ui/aura/window.h"
#endif

namespace xenon {
namespace {

constexpr char kRemoteHost[] = "xenon-remote";

bool IsHostDocument(const GURL& url) {
  return url.SchemeIs(content::kChromeUIScheme) && url.host() == kRemoteHost &&
         (url.path() == "/" || url.path() == "/index.html");
}

content::DesktopMediaID::Type SourceTypeForMode(const std::string& mode) {
  if (mode == "tab") {
    return content::DesktopMediaID::TYPE_WEB_CONTENTS;
  }
  if (mode == "window") {
    return content::DesktopMediaID::TYPE_WINDOW;
  }
  if (mode == "screen") {
    return content::DesktopMediaID::TYPE_SCREEN;
  }
  return content::DesktopMediaID::TYPE_NONE;
}

DesktopMediaList::Type ListTypeForSource(content::DesktopMediaID::Type type) {
  switch (type) {
    case content::DesktopMediaID::TYPE_WEB_CONTENTS:
      return DesktopMediaList::Type::kWebContents;
    case content::DesktopMediaID::TYPE_WINDOW:
      return DesktopMediaList::Type::kWindow;
    case content::DesktopMediaID::TYPE_SCREEN:
      return DesktopMediaList::Type::kScreen;
    case content::DesktopMediaID::TYPE_NONE:
      NOTREACHED();
  }
}

// A native window picker also contains other applications. Validate its result
// against live browser windows instead of treating every window as a browser.
bool IsBrowserWindow(const content::DesktopMediaID& source, Profile* profile) {
#if defined(USE_AURA) || BUILDFLAG(IS_MAC)
  auto selected = content::DesktopMediaID::GetNativeWindowById(source);
  if (!selected) {
    return false;
  }
  for (auto* browser : GetAllBrowserWindowInterfaces()) {
    if (browser->GetProfile() != profile || !browser->GetWindow()) {
      continue;
    }
    auto window = browser->GetWindow()->GetNativeWindow();
    if (window == selected) {
      return true;
    }
#if BUILDFLAG(IS_WIN)
    // The Windows native media list registers the WindowTreeHost's root;
    // BrowserWindowInterface exposes the widget's child window in that host.
    if (window && window->GetRootWindow() == selected->GetRootWindow()) {
      return true;
    }
#endif
  }
#endif
  return false;
}

class XenonRemoteMessageHandler : public content::WebUIMessageHandler,
                                  public content::WebContentsObserver {
 public:
  explicit XenonRemoteMessageHandler(content::WebContents* contents)
      : content::WebContentsObserver(contents) {}

  ~XenonRemoteMessageHandler() override { ResetSession(false); }

 private:
  void RegisterMessages() override {
    web_ui()->RegisterMessageCallback(
        "remoteGetCapabilities",
        base::BindRepeating(&XenonRemoteMessageHandler::GetCapabilities,
                            base::Unretained(this)));
    web_ui()->RegisterMessageCallback(
        "remoteChooseSource",
        base::BindRepeating(&XenonRemoteMessageHandler::ChooseSource,
                            base::Unretained(this)));
    web_ui()->RegisterMessageCallback(
        "remoteSetControl",
        base::BindRepeating(&XenonRemoteMessageHandler::SetControl,
                            base::Unretained(this)));
    web_ui()->RegisterMessageCallback(
        "remoteInput", base::BindRepeating(&XenonRemoteMessageHandler::Input,
                                           base::Unretained(this)));
    web_ui()->RegisterMessageCallback(
        "remoteReleaseInput",
        base::BindRepeating(&XenonRemoteMessageHandler::ReleaseInput,
                            base::Unretained(this)));
    web_ui()->RegisterMessageCallback(
        "remoteStop", base::BindRepeating(&XenonRemoteMessageHandler::Stop,
                                          base::Unretained(this)));
  }

  void OnJavascriptDisallowed() override { ResetSession(false); }

  void PrimaryPageChanged(content::Page& page) override {
    StopForDocumentChange();
  }

  void PrimaryMainFrameRenderProcessGone(
      base::TerminationStatus status) override {
    StopForDocumentChange();
  }

  void WebContentsDestroyed() override { StopForDocumentChange(); }

  // Every entry point checks the current document too, as navigation within a
  // WebUI host can reuse the controller. viewer.html never gains a host API.
  bool CanHandleRequest() {
    auto* contents = web_contents();
    auto* frame = contents ? contents->GetPrimaryMainFrame() : nullptr;
    return frame && frame->IsActive() &&
           IsHostDocument(frame->GetLastCommittedURL());
  }

  bool BeginRequest(const base::ListValue& args) {
    if (args.empty() || !args[0].is_string() || !CanHandleRequest()) {
      return false;
    }
    AllowJavascript();
    return true;
  }

  void Reject(const base::Value& callback_id, const std::string& error) {
    if (IsJavascriptAllowed()) {
      RejectJavascriptCallback(callback_id, base::Value(error));
    }
  }

  void ResetSession(bool settle_pending) {
    weak_factory_.InvalidateWeakPtrs();
    picker_.reset();
    input_router_.Reset();
    source_ = content::DesktopMediaID();
    control_enabled_ = false;
    if (!pending_callback_id_.empty()) {
      if (settle_pending) {
        Reject(base::Value(pending_callback_id_), "Source selection canceled.");
      }
      pending_callback_id_.clear();
    }
    // Never wrap and accidentally accept input belonging to an earlier source.
    if (generation_ < std::numeric_limits<int>::max()) {
      ++generation_;
    }
  }

  void StopForDocumentChange() {
    if (IsJavascriptAllowed()) {
      DisallowJavascript();
    } else {
      ResetSession(false);
    }
  }

  void GetCapabilities(const base::ListValue& args) {
    if (!BeginRequest(args)) {
      return;
    }
    base::DictValue result;
    result.Set("tabControlSupported",
               RemoteInputRouter::SupportsControl(content::DesktopMediaID(
                   content::DesktopMediaID::TYPE_WEB_CONTENTS, 0)));
    result.Set("windowControlSupported",
               RemoteInputRouter::SupportsControl(content::DesktopMediaID(
                   content::DesktopMediaID::TYPE_WINDOW, 0)));
    result.Set("screenControlSupported",
               RemoteInputRouter::SupportsControl(content::DesktopMediaID(
                   content::DesktopMediaID::TYPE_SCREEN, 0)));
    ResolveJavascriptCallback(args[0], result);
  }

  void ChooseSource(const base::ListValue& args) {
    if (!BeginRequest(args)) {
      return;
    }
    if (args.size() != 2 || !args[1].is_dict()) {
      Reject(args[0], "Invalid source request.");
      return;
    }
    const auto& options = args[1].GetDict();
    const std::string* mode = options.FindString("mode");
    const auto type =
        mode ? SourceTypeForMode(*mode) : content::DesktopMediaID::TYPE_NONE;
    if (type == content::DesktopMediaID::TYPE_NONE) {
      Reject(args[0], "Unknown sharing mode.");
      return;
    }

    // Revoke the old input target before presenting any new capture UI.
    ResetSession(true);
    if (generation_ == std::numeric_limits<int>::max()) {
      Reject(args[0], "Reload the sharing page to start a new session.");
      return;
    }
    auto* contents = web_contents();
    auto* frame = contents->GetPrimaryMainFrame();
    const auto origin = frame->GetLastCommittedOrigin();
    auto capture_level =
        capture_policy::GetAllowedCaptureLevel(origin.GetURL(), contents);
    std::vector<DesktopMediaList::Type> types{ListTypeForSource(type)};
    capture_policy::FilterMediaList(types, capture_level);
    if (types.empty()) {
      Reject(args[0], "This sharing mode is disabled by browser policy.");
      return;
    }

    auto policy_filter = capture_policy::GetIncludableWebContentsFilter(
        origin.GetURL(), capture_level);
    auto* profile = Profile::FromBrowserContext(contents->GetBrowserContext());
    DesktopMediaPickerController::Params params(
        DesktopMediaPicker::Params::RequestSource::kExtension);
    params.web_contents = contents;
    params.context = contents->GetTopLevelNativeWindow();
    params.parent = params.context;
    params.app_name = u"Xenon Remote";
    params.target_name = params.app_name;
    params.request_audio = options.FindBool("audio").value_or(false);
    params.restricted_by_policy =
        capture_level != AllowedScreenCaptureLevel::kUnrestricted;
    params.includable_web_contents_filter = base::BindRepeating(
        [](Profile* profile, DesktopMediaList::WebContentsFilter policy_filter,
           content::WebContents* candidate) {
          return candidate->GetBrowserContext() == profile &&
                 candidate->GetLastCommittedURL().host() != kRemoteHost &&
                 policy_filter.Run(candidate);
        },
        profile, std::move(policy_filter));
    pending_callback_id_ = args[0].GetString();
    picker_ = std::make_unique<DesktopMediaPickerController>();
    picker_->Show(
        params, types,
        base::BindOnce(&XenonRemoteMessageHandler::OnSourceSelected,
                       weak_factory_.GetWeakPtr(), frame->GetGlobalId(), origin,
                       generation_, *mode));
  }

  void OnSourceSelected(content::GlobalRenderFrameHostId frame_id,
                        url::Origin origin,
                        int generation,
                        std::string mode,
                        const std::string& error,
                        content::DesktopMediaID source) {
    picker_.reset();
    auto* contents = web_contents();
    auto* frame = contents ? contents->GetPrimaryMainFrame() : nullptr;
    if (!IsJavascriptAllowed() || !CanHandleRequest() || !frame ||
        frame->GetGlobalId() != frame_id ||
        frame->GetLastCommittedOrigin() != origin ||
        generation_ != generation) {
      ResetSession(false);
      return;
    }
    const base::Value callback_id(std::exchange(pending_callback_id_, {}));
    if (!error.empty()) {
      Reject(callback_id, error);
      return;
    }
    if (source.is_null()) {
      Reject(callback_id, "Source selection canceled.");
      return;
    }
    if (source.type != SourceTypeForMode(mode)) {
      Reject(callback_id,
             "The selected source does not match the sharing mode.");
      return;
    }

    // Policy/profile/source checks are repeated after the asynchronous dialog.
    const auto capture_level =
        capture_policy::GetAllowedCaptureLevel(origin.GetURL(), contents);
    std::vector<DesktopMediaList::Type> types{ListTypeForSource(source.type)};
    capture_policy::FilterMediaList(types, capture_level);
    auto* profile = Profile::FromBrowserContext(contents->GetBrowserContext());
    if (types.empty()) {
      Reject(callback_id, "This sharing mode is disabled by browser policy.");
      return;
    }
    if (source.type == content::DesktopMediaID::TYPE_WINDOW &&
        !IsBrowserWindow(source, profile)) {
      Reject(callback_id, "Choose a browser window from this profile.");
      return;
    }
    if (source.type == content::DesktopMediaID::TYPE_WEB_CONTENTS) {
      auto* target_frame = content::RenderFrameHost::FromID(
          source.web_contents_id.render_process_id,
          source.web_contents_id.main_render_frame_id);
      auto* target =
          target_frame ? content::WebContents::FromRenderFrameHost(target_frame)
                       : nullptr;
      auto filter = capture_policy::GetIncludableWebContentsFilter(
          origin.GetURL(), capture_level);
      if (!target_frame || !target_frame->IsActive() || !target ||
          target->GetBrowserContext() != profile ||
          target->GetLastCommittedURL().host() == kRemoteHost ||
          !filter.Run(target)) {
        Reject(callback_id, "The selected page is no longer available.");
        return;
      }
    }

    std::string stream_id =
        content::DesktopStreamsRegistry::GetInstance()->RegisterStream(
            frame_id.child_id.GetUnsafeValue(), frame_id.frame_routing_id,
            origin, source, content::kRegistryStreamTypeDesktop);
    if (stream_id.empty()) {
      Reject(callback_id, "Could not prepare the selected capture source.");
      return;
    }
    source_ = source;
    input_router_.SetTarget(source_, generation_);
    // ResetSession invalidates this callback before resetting the router, so a
    // queued revocation cannot outlive the selected source or this handler.
    input_router_.SetControlRevokedCallback(
        base::BindRepeating(&XenonRemoteMessageHandler::OnControlRevoked,
                            weak_factory_.GetWeakPtr(), generation_));
    base::DictValue result;
    result.Set("streamId", std::move(stream_id));
    result.Set("generation", generation_);
    result.Set("mode", mode);
    result.Set("canRequestAudioTrack", source.audio_share);
    result.Set("controlSupported", RemoteInputRouter::SupportsControl(source));
    ResolveJavascriptCallback(callback_id, result);
  }

  void SetControl(const base::ListValue& args) {
    if (!BeginRequest(args)) {
      return;
    }
    if (args.size() != 2 || !args[1].is_dict()) {
      Reject(args[0], "Invalid control request.");
      return;
    }
    const auto& options = args[1].GetDict();
    const auto generation = options.FindInt("generation");
    const auto enabled = options.FindBool("enabled");
    if (!generation || *generation != generation_ || !enabled ||
        source_.is_null()) {
      Reject(args[0], "The selected source has changed or stopped.");
      return;
    }
    if (*enabled) {
      control_enabled_ = input_router_.SetEnabled(true);
    } else {
      input_router_.SetEnabled(false);
      control_enabled_ = false;
    }
    base::DictValue result;
    result.Set("enabled", control_enabled_);
    ResolveJavascriptCallback(args[0], result);
  }

  void Input(const base::ListValue& args) {
    if (!CanHandleRequest() || !IsJavascriptAllowed() || args.size() != 1 ||
        !args[0].is_dict()) {
      return;
    }
    const auto& event = args[0].GetDict();
    const auto generation = event.FindInt("generation");
    if (!generation || *generation != generation_) {
      return;
    }
    const auto* type = event.FindString("type");
    if (type && *type == "release") {
      input_router_.ReleaseAll();
      return;
    }
    if (!control_enabled_) {
      return;
    }
    if (!input_router_.HandleInput(event)) {
      input_router_.SetEnabled(false);
      OnControlRevoked(generation_);
    }
  }

  void OnControlRevoked(int generation) {
    if (generation != generation_ || !control_enabled_) {
      return;
    }
    control_enabled_ = false;
    if (!IsJavascriptAllowed() || !CanHandleRequest()) {
      return;
    }
    base::DictValue result;
    result.Set("enabled", false);
    result.Set("generation", generation_);
    result.Set("reason", "The target is unavailable or cannot accept input.");
    FireWebUIListener("remote-control-changed", result);
  }

  void ReleaseInput(const base::ListValue& args) {
    if (CanHandleRequest()) {
      input_router_.ReleaseAll();
    }
  }

  void Stop(const base::ListValue& args) {
    if (!BeginRequest(args)) {
      return;
    }
    ResetSession(true);
    ResolveJavascriptCallback(args[0], base::DictValue());
  }

  std::unique_ptr<DesktopMediaPickerController> picker_;
  RemoteInputRouter input_router_;
  content::DesktopMediaID source_;
  int generation_ = 0;
  bool control_enabled_ = false;
  std::string pending_callback_id_;
  base::WeakPtrFactory<XenonRemoteMessageHandler> weak_factory_{this};
};

}  // namespace

XenonRemoteUIController::XenonRemoteUIController(content::WebUI* web_ui,
                                                 const GURL& url)
    : content::WebUIController(web_ui) {
  auto* source = content::WebUIDataSource::CreateAndAdd(
      web_ui->GetWebContents()->GetBrowserContext(), kRemoteHost);
  source->SetDefaultResource(IDR_XENON_REMOTE_HTML);
  source->AddResourcePath("index.html", IDR_XENON_REMOTE_HTML);
  source->AddResourcePath("index.css", IDR_XENON_REMOTE_CSS);
  source->AddResourcePath("index.js", IDR_XENON_REMOTE_JS);
  source->AddResourcePath("session.js", IDR_XENON_REMOTE_SESSION_JS);
  source->AddResourcePath("viewer.html", IDR_XENON_REMOTE_VIEWER_HTML);
  source->AddResourcePath("viewer.js", IDR_XENON_REMOTE_VIEWER_JS);
  source->OverrideContentSecurityPolicy(
      network::mojom::CSPDirectiveName::MediaSrc, "media-src 'self' blob:;");
  if (IsHostDocument(url)) {
    web_ui->AddMessageHandler(
        std::make_unique<XenonRemoteMessageHandler>(web_ui->GetWebContents()));
  }
}

XenonRemoteUIController::~XenonRemoteUIController() = default;

XenonRemoteUIConfig::XenonRemoteUIConfig()
    : content::WebUIConfig(content::kChromeUIScheme, kRemoteHost) {}

XenonRemoteUIConfig::~XenonRemoteUIConfig() = default;

bool XenonRemoteUIConfig::IsWebUIEnabled(
    content::BrowserContext* browser_context) {
  const auto* profile = Profile::FromBrowserContext(browser_context);
  return profile->IsRegularProfile() && !profile->IsGuestSession();
}

std::unique_ptr<content::WebUIController>
XenonRemoteUIConfig::CreateWebUIController(content::WebUI* web_ui,
                                           const GURL& url) {
  return std::make_unique<XenonRemoteUIController>(web_ui, url);
}

}  // namespace xenon
