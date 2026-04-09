// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/xenon_ai/xenon_ai_context_menu_observer.h"

#include "base/strings/strcat.h"
#include "base/strings/utf_string_conversions.h"
#include "base/supports_user_data.h"
#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_finder.h"
#include "chrome/browser/ui/views/side_panel/side_panel_coordinator.h"
#include "chrome/browser/ui/views/side_panel/side_panel_entry_id.h"
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
  if (!IsCommandIdSupported(command_id)) return;

  bool is_rewrite_cmd = (command_id == IDC_XENON_AI_CONTEXT_PROFESSIONALIZE);
  // Brave Leo in-place rewrite requires: editable selection, opt-in, feature flag,
  // SSE streaming, command in a fixed rewrite set, source==embedder WebContents,
  // and no in-progress rewrite UserData. See xenon_ai_side_panel_context_menu_
  // changelog.md appendix A; wire XenonAiService streaming before enabling.
  bool rewrite_in_place = params_->is_editable &&
                          is_rewrite_cmd &&
                          !web_contents_->GetUserData(kXenonAiRewriteDataKey);

  if (rewrite_in_place) {
    web_contents_->SetUserData(kXenonAiRewriteDataKey,
                               std::make_unique<XenonAiRewriteData>());
    // TODO: Connect to XenonAiService and call GenerateRewriteSuggestion
  } else {
    // Open Xenon AI Side Panel and submit selected text
    std::string text = base::UTF16ToUTF8(params_->selection_text);

    XenonAiService* service = XenonAiServiceFactory::GetForProfile(
        Profile::FromBrowserContext(web_contents_->GetBrowserContext()));
    if (service) {
      service->SetPendingPrompt(text);
    }

    Browser* browser = chrome::FindBrowserWithTab(web_contents_);
    if (browser) {
      if (SidePanelCoordinator* coordinator = SidePanelCoordinator::From(browser)) {
        coordinator->Show(SidePanelEntryId::kXenonAI);
      }
    }
  }
}

bool XenonAiContextMenuObserver::IsXenonAiEnabled() const {
  return !params_->selection_text.empty();
}

void XenonAiContextMenuObserver::OnRewriteSuggestionDataReceived(
    const std::string& suggestion_delta) {
  auto* rewrite_data = static_cast<XenonAiRewriteData*>(
      web_contents_->GetUserData(kXenonAiRewriteDataKey));
  if (!rewrite_data) return;

  if (!rewrite_data->accumulated_text.empty()) {
    web_contents_->Undo();
  }

  base::StrAppend(&rewrite_data->accumulated_text, {suggestion_delta});
  web_contents_->Replace(base::UTF8ToUTF16(rewrite_data->accumulated_text));
}

void XenonAiContextMenuObserver::OnRewriteSuggestionCompleted(
    const std::string& selected_text, bool success) {
  if (!success) {
    auto* rewrite_data = static_cast<XenonAiRewriteData*>(
        web_contents_->GetUserData(kXenonAiRewriteDataKey));
    if (rewrite_data && !rewrite_data->accumulated_text.empty()) {
      web_contents_->Undo();
    }
  }
  web_contents_->RemoveUserData(kXenonAiRewriteDataKey);
}

}  // namespace xenon
