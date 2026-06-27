// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_UI_XENON_MENU_SHADOW_MODIFIER_H_
#define XENON_OVERLAY_CHROME_BROWSER_UI_XENON_MENU_SHADOW_MODIFIER_H_

#include <memory>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "ui/views/view_observer.h"
#include "ui/views/widget/widget_observer.h"
#include "xenon_overlay/chrome/browser/ui/xenon_menu_shadow.h"

namespace ui {
class Shadow;
}

namespace views {
class ViewShadow;
class Widget;
}  // namespace views

namespace xenon {

class MenuShadowModifier : public views::WidgetObserver,
                           public views::ViewObserver {
 public:
  explicit MenuShadowModifier(const XenonMenuShadow& shadow);

  MenuShadowModifier(const MenuShadowModifier&) = delete;
  MenuShadowModifier& operator=(const MenuShadowModifier&) = delete;

  ~MenuShadowModifier() override;

  void SetParentWidget(views::Widget* parent);

  // views::WidgetObserver:
  void OnWidgetChildAdded(views::Widget* widget, views::Widget* child) override;
  void OnWidgetDestroying(views::Widget* widget) override;

  // views::ViewObserver:
  void OnViewLayerBoundsSet(views::View* view) override;
  void OnViewIsDeleting(views::View* view) override;

 private:
  void ApplyShadow();

  const XenonMenuShadow shadow_;

  raw_ptr<views::Widget> parent_widget_ = nullptr;
  raw_ptr<views::Widget> menu_widget_ = nullptr;

  std::unique_ptr<views::ViewShadow> view_shadow_;
  std::unique_ptr<ui::Shadow> compositor_shadow_;

  raw_ptr<views::View> bg_view_ = nullptr;

  bool captured_ = false;
  bool deletion_scheduled_ = false;

  base::WeakPtrFactory<MenuShadowModifier> weak_factory_{this};
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_UI_XENON_MENU_SHADOW_MODIFIER_H_
