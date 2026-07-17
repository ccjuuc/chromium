// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_UI_VIEWS_SIDEBAR_XENON_SIDEBAR_VIEW_H_
#define XENON_OVERLAY_CHROME_BROWSER_UI_VIEWS_SIDEBAR_XENON_SIDEBAR_VIEW_H_

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/timer/timer.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/gfx/geometry/point_f.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/views/view.h"
#include "xenon_overlay/chrome/browser/sidebar/xenon_sidebar_service.h"

class Browser;
class GURL;

namespace xenon {

// Returns true when a browser window should show the Xenon shortcut sidebar.
// The sidebar is enabled by default for normal tabbed browser windows and can
// be disabled with --disable-xenon-sidebar.
bool IsXenonSidebarEnabledForBrowser(const Browser* browser);

// Returns true when the command line allows the Xenon sidebar to auto-hide.
// The per-profile user setting is stored separately in Xenon prefs.
bool IsXenonSidebarAutoHideEnabled();

// A Brave-style narrow browser sidebar for frequently used built-in pages.
class XenonSidebarView : public views::View,
                         public XenonSidebarService::Observer {
  METADATA_HEADER(XenonSidebarView, views::View)

 public:
  explicit XenonSidebarView(Browser* browser);
  ~XenonSidebarView() override;

  XenonSidebarView(const XenonSidebarView&) = delete;
  XenonSidebarView& operator=(const XenonSidebarView&) = delete;

  bool IsSidebarVisible() const;
  bool IsRightAligned() const;

  // Shows the sidebar if `point_in_screen` is within the active edge hot zone of
  // `detect_bounds_in_screen`.
  void ShowSidebarOnMouseOver(const gfx::PointF& point_in_screen,
                              const gfx::Rect& detect_bounds_in_screen);

  // views::View:
  gfx::Size CalculatePreferredSize(
      const views::SizeBounds& available_size) const override;
  gfx::Size GetMinimumSize() const override;
  void OnThemeChanged() override;
  void OnMouseEntered(const ui::MouseEvent& event) override;
  void OnMouseExited(const ui::MouseEvent& event) override;

  // XenonSidebarService::Observer:
  void OnSidebarAutoHideChanged(bool auto_hide_enabled) override;
  void OnSidebarAlignmentChanged(bool right_aligned) override;

 private:
  bool ShouldReserveWidth() const;
  bool ShouldForceShowSidebar() const;
  void ShowSidebar();
  void HideSidebar();
  void ScheduleHideSidebar();
  void ToggleSidebarAutoHide();
  void SetSidebarRightAligned(bool right_aligned);
  void OpenShortcut(const GURL& url);

  raw_ptr<Browser> browser_ = nullptr;
  raw_ptr<XenonSidebarService> service_ = nullptr;
  raw_ptr<views::View> left_align_button_ = nullptr;
  raw_ptr<views::View> right_align_button_ = nullptr;
  raw_ptr<views::View> hide_button_ = nullptr;
  bool auto_hide_enabled_ = false;
  bool sidebar_visible_ = true;
  base::OneShotTimer sidebar_hide_timer_;
  base::WeakPtrFactory<XenonSidebarView> weak_ptr_factory_{this};
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_UI_VIEWS_SIDEBAR_XENON_SIDEBAR_VIEW_H_
