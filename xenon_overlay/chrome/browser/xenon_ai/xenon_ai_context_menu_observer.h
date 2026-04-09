// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_XENON_AI_XENON_AI_CONTEXT_MENU_OBSERVER_H_
#define XENON_OVERLAY_CHROME_BROWSER_XENON_AI_XENON_AI_CONTEXT_MENU_OBSERVER_H_

#include <memory>
#include <string>

#include "base/memory/raw_ptr.h"
#include "base/memory/raw_ref.h"
#include "base/memory/weak_ptr.h"
#include "components/renderer_context_menu/render_view_context_menu_observer.h"
#include "ui/menus/simple_menu_model.h"

class RenderViewContextMenuProxy;
class Profile;

namespace content {
class WebContents;
struct ContextMenuParams;
}  // namespace content

namespace xenon {

// Observer for the RenderViewContextMenu to inject Xenon AI features such as
// "Summarize" and "Rewrite/Change Tone" into the user's web page context menus.
// Capable of streaming suggestions in-place if the selected area is editable.
class XenonAiContextMenuObserver : public RenderViewContextMenuObserver,
                                   public ui::SimpleMenuModel::Delegate {
 public:
  XenonAiContextMenuObserver(RenderViewContextMenuProxy* proxy,
                             content::WebContents* web_contents,
                             const content::ContextMenuParams& params);
  XenonAiContextMenuObserver(const XenonAiContextMenuObserver&) = delete;
  XenonAiContextMenuObserver& operator=(const XenonAiContextMenuObserver&) =
      delete;
  ~XenonAiContextMenuObserver() override;

  // RenderViewContextMenuObserver:
  void InitMenu(const content::ContextMenuParams& params) override;
  bool IsCommandIdSupported(int command_id) override;
  bool IsCommandIdChecked(int command_id) override;
  bool IsCommandIdEnabled(int command_id) override;
  void ExecuteCommand(int command_id) override;

  // ui::SimpleMenuModel::Delegate:
  void ExecuteCommand(int command_id, int event_flags) override;
  bool IsCommandIdEnabled(int command_id) const override;
  bool IsCommandIdChecked(int command_id) const override;

 private:
  // Whether Xenon AI context menu features are enabled in settings and profile.
  bool IsXenonAiEnabled() const;

  // Handlers for the streaming response of in-place rewrite operations.
  void OnRewriteSuggestionDataReceived(const std::string& suggestion_delta);
  void OnRewriteSuggestionCompleted(const std::string& selected_text,
                                   bool success);

  base::raw_ptr<RenderViewContextMenuProxy> proxy_;
  base::raw_ptr<content::WebContents> web_contents_;
  const base::raw_ref<const content::ContextMenuParams> params_;

  // Submenu models
  ui::SimpleMenuModel ai_quick_actions_submenu_;
  ui::SimpleMenuModel ai_change_tone_submenu_;

  base::WeakPtrFactory<XenonAiContextMenuObserver> weak_ptr_factory_{this};
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_XENON_AI_XENON_AI_CONTEXT_MENU_OBSERVER_H_
