// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_UI_XENON_MENU_RUNNER_H_
#define XENON_OVERLAY_CHROME_BROWSER_UI_XENON_MENU_RUNNER_H_

#include <stdint.h>

#include <memory>
#include <optional>
#include <string>

#include "base/functional/callback.h"
#include "ui/base/mojom/menu_source_type.mojom-forward.h"
#include "ui/gfx/native_ui_types.h"
#include "ui/views/controls/menu/menu_runner.h"

#include "xenon_overlay/chrome/browser/ui/xenon_menu_shadow.h"

namespace base {
class TimeTicks;
}

namespace gfx {
class Rect;
}

namespace ui {
class MenuModel;
}

namespace views {
class MenuButtonController;
class MenuItemView;
class Widget;
}  // namespace views

namespace xenon {

class XenonMenuRunner {
 public:
  static constexpr int kCornerRadius = 16;

  XenonMenuRunner(ui::MenuModel* menu_model,
                  int32_t run_types,
                  base::RepeatingClosure on_menu_closed_callback =
                      base::RepeatingClosure());
  XenonMenuRunner(std::unique_ptr<views::MenuItemView> menu,
                  int32_t run_types);
  XenonMenuRunner(const XenonMenuRunner&) = delete;
  XenonMenuRunner& operator=(const XenonMenuRunner&) = delete;
  ~XenonMenuRunner();

  void RunMenuAt(
      views::Widget* parent,
      views::MenuButtonController* button_controller,
      const gfx::Rect& bounds,
      views::MenuAnchorPosition anchor,
      ui::mojom::MenuSourceType source_type,
      const XenonMenuShadow& shadow = XenonMenuShadow(),
      gfx::NativeView native_view_for_gestures = gfx::NativeView(),
      std::optional<std::string> show_menu_host_duration_histogram =
          std::nullopt);

  bool IsRunning() const;
  void Cancel();
  base::TimeTicks closing_event_time() const;

 private:
  views::MenuRunner menu_runner_;
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_UI_XENON_MENU_RUNNER_H_
