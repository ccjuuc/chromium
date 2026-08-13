// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/xenon_ai/xenon_ai_context_menu_observer.h"

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/strcat.h"
#include "base/strings/utf_string_conversions.h"
#include "base/supports_user_data.h"
#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/global_browser_collection.h"
#include "chrome/browser/ui/side_panel/side_panel_entry_id.h"
#include "chrome/browser/ui/views/side_panel/side_panel_coordinator.h"
#include "components/renderer_context_menu/render_view_context_menu_proxy.h"
#include "content/public/browser/context_menu_params.h"
#include "content/public/browser/web_contents.h"
#include "xenon_overlay/chrome/browser/xenon_ai/xenon_ai_service.h"
#include "xenon_overlay/chrome/browser/xenon_ai/xenon_ai_service_factory.h"

// Temporary stub IDC commands for Xenon
constexpr int IDC_XENON_AI_CONTEXT_SUMMARIZE = 61000;
constexpr int IDC_XENON_AI_CONTEXT_QUICK_ACTIONS = 61001;
constexpr int IDC_XENON_AI_CONTEXT_CHANGE_TONE = 61003;
constexpr int IDC_XENON_AI_CONTEXT_PROFESSIONALIZE = 61004;

namespace xenon {

namespace {

constexpr char kXenonAiRewriteDataKey[] = "xenon_ai_rewrite_data";

struct XenonAiRewriteData : public base::SupportsUserData::Data {
  std::string accumulated_text;
};

// The context menu (and this observer) are destroyed when the menu closes.
// Inference is async, so completion must not use WeakPtr<XenonAiContextMenuObserver>.
void ApplyInferencerRewriteOnWebContents(content::WebContents* web_contents,
                                         bool success,
                                         const std::string& text) {
  if (!web_contents) {
    LOG(WARNING) << "[xenon_ai] ApplyInferencerRewrite: null WebContents";
    return;
  }
  if (success) {
    auto* rewrite_data = static_cast<XenonAiRewriteData*>(
        web_contents->GetUserData(kXenonAiRewriteDataKey));
    if (!rewrite_data) {
      LOG(WARNING) << "[xenon_ai] ApplyInferencerRewrite: success but no "
                      "UserData (tab navigated away?)";
      return;
    }

    web_contents->RestoreFocus();

    if (!rewrite_data->accumulated_text.empty()) {
      web_contents->Undo();
    }

    base::StrAppend(&rewrite_data->accumulated_text, {text});
    VLOG(1) << "[xenon_ai] Replace accumulated_len="
            << rewrite_data->accumulated_text.size();
    web_contents->Replace(base::UTF8ToUTF16(rewrite_data->accumulated_text));
    web_contents->RemoveUserData(kXenonAiRewriteDataKey);
  } else {
    auto* rewrite_data = static_cast<XenonAiRewriteData*>(
        web_contents->GetUserData(kXenonAiRewriteDataKey));
    if (rewrite_data && !rewrite_data->accumulated_text.empty()) {
      web_contents->Undo();
    }
    web_contents->RemoveUserData(kXenonAiRewriteDataKey);
  }
  VLOG(1) << "[xenon_ai] ApplyInferencerRewrite done success=" << success;
}

}  // namespace

XenonAiContextMenuObserver::XenonAiContextMenuObserver(
    RenderViewContextMenuProxy* proxy,
    content::WebContents* web_contents,
    const content::ContextMenuParams& params)
    : proxy_(proxy),
      web_contents_(web_contents),
      params_(params),
      ai_quick_actions_submenu_(this),
      ai_change_tone_submenu_(this) {}

XenonAiContextMenuObserver::~XenonAiContextMenuObserver() = default;

void XenonAiContextMenuObserver::InitMenu(
    const content::ContextMenuParams& params) {
  if (!IsXenonAiEnabled()) {
    return;
  }

  ai_quick_actions_submenu_.AddItem(IDC_XENON_AI_CONTEXT_SUMMARIZE,
                                    u"Summarize Text (Xenon AI)");

  ai_change_tone_submenu_.AddItem(IDC_XENON_AI_CONTEXT_PROFESSIONALIZE,
                                   u"Professionalize");

  ai_quick_actions_submenu_.AddSubMenu(IDC_XENON_AI_CONTEXT_CHANGE_TONE,
                                       u"Change Tone...",
                                       &ai_change_tone_submenu_);

  proxy_->AddSubMenu(IDC_XENON_AI_CONTEXT_QUICK_ACTIONS, u"Xenon AI",
                     &ai_quick_actions_submenu_);
}

bool XenonAiContextMenuObserver::IsCommandIdSupported(int command_id) {
  return command_id >= 61000 && command_id <= 61004;
}

bool XenonAiContextMenuObserver::IsCommandIdChecked(int command_id) {
  return false;
}

bool XenonAiContextMenuObserver::IsCommandIdEnabled(int command_id) {
  return IsXenonAiEnabled();
}

bool XenonAiContextMenuObserver::IsCommandIdEnabled(int command_id) const {
  return IsXenonAiEnabled();
}

bool XenonAiContextMenuObserver::IsCommandIdChecked(int command_id) const {
  return false;
}

void XenonAiContextMenuObserver::ExecuteCommand(int command_id) {
  ExecuteCommand(command_id, 0);
}

void XenonAiContextMenuObserver::ExecuteCommand(int command_id, int event_flags) {
  if (!IsCommandIdSupported(command_id)) {
    return;
  }

  VLOG(1) << "[xenon_ai] ExecuteCommand id=" << command_id;

  bool is_rewrite_cmd = (command_id == IDC_XENON_AI_CONTEXT_PROFESSIONALIZE);
  // Brave Leo in-place rewrite requires: editable selection, opt-in, feature flag,
  // SSE streaming, command in a fixed rewrite set, source==embedder WebContents,
  // and no in-progress rewrite UserData. See xenon_ai_side_panel_context_menu_
  // changelog.md appendix A; wire XenonAiService streaming before enabling.
  bool rewrite_in_place = params_->is_editable &&
                          is_rewrite_cmd &&
                          !web_contents_->GetUserData(kXenonAiRewriteDataKey);

  VLOG(1) << "[xenon_ai] rewrite_in_place=" << rewrite_in_place
          << " is_editable=" << params_->is_editable
          << " is_rewrite_cmd=" << is_rewrite_cmd
          << " has_rewrite_userdata="
          << (web_contents_->GetUserData(kXenonAiRewriteDataKey) != nullptr);

  if (rewrite_in_place) {
    web_contents_->SetUserData(kXenonAiRewriteDataKey,
                                std::make_unique<XenonAiRewriteData>());
    XenonAiService* ai_service = XenonAiServiceFactory::GetForProfile(
        Profile::FromBrowserContext(web_contents_->GetBrowserContext()));
    if (!ai_service) {
      LOG(WARNING) << "[xenon_ai] XenonAiServiceFactory returned null";
      web_contents_->RemoveUserData(kXenonAiRewriteDataKey);
      return;
    }
    const std::string selection_utf8 =
        base::UTF16ToUTF8(params_->selection_text);
    ai_service->RunInferencerRewriteAsync(
        "Rewrite the selection to be more professional and polished. "
        "Preserve the original language (same language as the source).",
        selection_utf8,
        base::BindOnce(
            [](base::WeakPtr<content::WebContents> web_contents, bool success,
               const std::string& text) {
              if (!web_contents) {
                LOG(WARNING) << "[xenon_ai] rewrite callback: WebContents gone "
                                "(weak_ptr expired)";
                return;
              }
              ApplyInferencerRewriteOnWebContents(web_contents.get(), success,
                                                  text);
            },
            web_contents_->GetWeakPtr()));
  } else {
    // Open Xenon AI Side Panel and submit selected text
    std::string text = base::UTF16ToUTF8(params_->selection_text);

    XenonAiService* service = XenonAiServiceFactory::GetForProfile(
        Profile::FromBrowserContext(web_contents_->GetBrowserContext()));
    if (service) {
      service->SetPendingPrompt(text);
    }

    BrowserWindowInterface* browser_window =
        GlobalBrowserCollection::GetInstance()->FindBrowserWithTab(
            web_contents_);
    if (browser_window) {
      if (SidePanelCoordinator* coordinator =
              SidePanelCoordinator::From(browser_window)) {
        coordinator->Show(SidePanelEntryId::kXenonAI);
      }
    }
  }
}

bool XenonAiContextMenuObserver::IsXenonAiEnabled() const {
  return !params_->selection_text.empty();
}

}  // namespace xenon
