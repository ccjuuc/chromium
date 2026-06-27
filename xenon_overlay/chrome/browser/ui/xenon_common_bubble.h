#ifndef XENON_OVERLAY_CHROME_BROWSER_UI_XENON_COMMON_BUBBLE_H_
#define XENON_OVERLAY_CHROME_BROWSER_UI_XENON_COMMON_BUBBLE_H_

#include <memory>
#include <string>

#include "ui/gfx/geometry/rect.h"
#include "ui/views/bubble/bubble_dialog_delegate_view.h"
#include "xenon_overlay/chrome/browser/ui/xenon_menu_shadow.h"

class WebUIBubbleManager;

namespace views {
class BubbleDialogDelegate;
class FrameView;
class View;
class Widget;
}  // namespace views

namespace xenon {

// 与 Chrome 头像菜单（ProfileMenuViewBase）一致的
// Bubble：标准阴影、圆角、主题背景。
class XenonCommonBubble : public views::BubbleDialogDelegate {
 public:
  static constexpr int kCornerRadius = 16;

  XenonCommonBubble(views::View* anchor_view,
                    std::u16string text,
                    const XenonMenuShadow& shadow,
                    views::BubbleBorder::Arrow arrow);
  ~XenonCommonBubble() override;

  XenonCommonBubble(const XenonCommonBubble&) = delete;
  XenonCommonBubble& operator=(const XenonCommonBubble&) = delete;

  static views::Widget* Show(views::View* anchor_view,
                             std::u16string text,
                             const XenonMenuShadow& shadow = XenonMenuShadow());
  static views::Widget* ShowAt(
      views::View* parent_view,
      const gfx::Rect& anchor_rect,
      views::BubbleBorder::Arrow arrow,
      std::u16string text,
      const XenonMenuShadow& shadow = XenonMenuShadow());
  static void ConfigureBeforeWidgetInitialization(
      views::BubbleDialogDelegate* bubble_delegate);
  static void ApplyStyle(views::BubbleDialogDelegate* bubble_delegate,
                         const XenonMenuShadow& shadow = XenonMenuShadow());
  static void ConfigureWebUIBubbleManager(WebUIBubbleManager* bubble_manager);
  static void ApplyWebUIBubbleStyle(
      WebUIBubbleManager* bubble_manager,
      const XenonMenuShadow& shadow = XenonMenuShadow());

 private:
  // views::WidgetDelegate:
  std::unique_ptr<views::FrameView> CreateFrameView(
      views::Widget* widget) override;

  const XenonMenuShadow shadow_;
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_UI_XENON_COMMON_BUBBLE_H_
