#ifndef XENON_OVERLAY_CHROME_BROWSER_UI_XENON_COMMON_BUBBLE_H_
#define XENON_OVERLAY_CHROME_BROWSER_UI_XENON_COMMON_BUBBLE_H_

#include <memory>
#include <string>

#include "ui/views/bubble/bubble_dialog_delegate_view.h"

namespace views {
class View;
class Widget;
}  // namespace views

namespace xenon {

// 与 Chrome 头像菜单（ProfileMenuViewBase）一致的 Bubble：标准阴影、圆角、主题背景。
class XenonCommonBubble : public views::BubbleDialogDelegate {
 public:
  XenonCommonBubble(views::View* anchor_view, std::u16string text);
  ~XenonCommonBubble() override;

  XenonCommonBubble(const XenonCommonBubble&) = delete;
  XenonCommonBubble& operator=(const XenonCommonBubble&) = delete;

  static views::Widget* Show(views::View* anchor_view, std::u16string text);
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_UI_XENON_COMMON_BUBBLE_H_
