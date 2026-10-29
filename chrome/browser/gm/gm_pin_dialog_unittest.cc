// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/gm/gm_pin_dialog.h"

#include <windows.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/functional/bind.h"
#include "base/test/scoped_run_loop_timeout.h"
#include "base/time/time.h"
#include "chrome/grit/generated_resources.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/base/ime/text_edit_commands.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/models/combobox_model.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/controls/combobox/combobox.h"
#include "ui/views/controls/label.h"
#include "ui/views/controls/textfield/textfield.h"
#include "ui/views/test/views_test_base.h"
#include "ui/views/test/widget_test.h"
#include "ui/views/view_utils.h"
#include "ui/views/widget/widget.h"
#include "ui/views/win/hwnd_util.h"
#include "ui/views/window/dialog_delegate.h"

namespace gm_browser {
namespace {

template <typename T>
T* FindView(views::View* root) {
  if (auto* match = views::AsViewClass<T>(root)) {
    return match;
  }
  for (views::View* child : root->children()) {
    if (auto* match = FindView<T>(child)) {
      return match;
    }
  }
  return nullptr;
}

bool HasLabel(views::View* root, std::u16string_view text) {
  if (auto* label = views::AsViewClass<views::Label>(root);
      label && label->GetText().find(text) != std::u16string_view::npos) {
    return true;
  }
  for (views::View* child : root->children()) {
    if (HasLabel(child, text)) {
      return true;
    }
  }
  return false;
}

class GmPinDialogTest : public views::ViewsTestWithDesktopNativeWidget {
 protected:
  void TearDown() override {
    if (widget_) {
      widget_->CloseNow();
    }
    views::ViewsTestWithDesktopNativeWidget::TearDown();
  }

  void Show(const std::vector<std::u16string>& identities =
                {u"签名证书 A [1111]", u"签名证书 B [2222]"},
            const std::u16string& message = {}) {
    widget_ = ShowPinDialog("https://gm.example:8443", identities, message,
                            ResultCallback());
    ASSERT_TRUE(widget_);
    ASSERT_TRUE(pin());
    ASSERT_TRUE(certificates());
    ASSERT_TRUE(delegate());
  }

  views::Textfield* pin() const {
    return widget_ ? FindView<views::Textfield>(widget_->GetContentsView())
                   : nullptr;
  }
  views::Combobox* certificates() const {
    return widget_ ? FindView<views::Combobox>(widget_->GetContentsView())
                   : nullptr;
  }
  views::DialogDelegate* delegate() const {
    return widget_ ? widget_->widget_delegate()->AsDialogDelegate() : nullptr;
  }
  void AcceptAndWait() {
    ASSERT_TRUE(widget_);
    inspect_cleared_field_ = true;
    views::test::WidgetDestroyedWaiter closed(widget_.get());
    delegate()->AcceptDialog();
    closed.Wait();
  }
  void OnResult(size_t selected, std::unique_ptr<gm_key::SecurePin> value) {
    ++callback_count_;
    selected_ = selected;
    result_ = std::move(value);
    // Inspect while the acceptance/cancel callback still owns a live view.
    // Closed/destruction paths are checked separately without dereferencing it.
    if (inspect_cleared_field_) {
      ASSERT_TRUE(pin());
      EXPECT_TRUE(pin()->GetText().empty());
      EXPECT_FALSE(pin()->IsTextEditCommandEnabled(ui::TextEditCommand::UNDO));
      EXPECT_FALSE(pin()->IsTextEditCommandEnabled(ui::TextEditCommand::REDO));
    }
  }

  PinCallback ResultCallback() {
    return base::BindOnce(&GmPinDialogTest::OnResult, base::Unretained(this));
  }

  base::test::ScopedRunLoopTimeout timeout_{FROM_HERE, base::Seconds(10)};
  base::WeakPtr<views::Widget> widget_;
  int callback_count_ = 0;
  size_t selected_ = 0;
  std::unique_ptr<gm_key::SecurePin> result_;
  bool inspect_cleared_field_ = false;
};

TEST_F(GmPinDialogTest,
       PasswordFieldShowsRequestingOriginAndCertificateChoices) {
  const auto retries = l10n_util::GetStringFUTF16(IDS_GM_PIN_RETRIES, u"2");
  ASSERT_NO_FATAL_FAILURE(
      Show({u"签名证书 A [1111]", u"签名证书 B [2222]"}, retries));
  EXPECT_EQ(delegate()->GetWindowTitle(),
            l10n_util::GetStringUTF16(IDS_GM_PIN_TITLE));
  EXPECT_EQ(pin()->GetTextInputType(), ui::TEXT_INPUT_TYPE_PASSWORD);
  EXPECT_EQ(delegate()->GetInitiallyFocusedView(), pin());
  EXPECT_FALSE(pin()->GetViewAccessibility().GetCachedName().empty());
  EXPECT_TRUE(HasLabel(widget_->GetContentsView(), u"https://gm.example:8443"));
  EXPECT_TRUE(HasLabel(widget_->GetContentsView(), retries));
  EXPECT_EQ(certificates()->GetModel()->GetItemCount(), 2u);
  pin()->SetText(u"123456");
  pin()->SelectAll(false);
  EXPECT_FALSE(pin()->IsTextEditCommandEnabled(ui::TextEditCommand::COPY));
  EXPECT_FALSE(pin()->IsTextEditCommandEnabled(ui::TextEditCommand::CUT));
  EXPECT_EQ(callback_count_, 0);
}

TEST_F(GmPinDialogTest, SelectedCertificateAndPinAreReturnedOnce) {
  ASSERT_NO_FATAL_FAILURE(Show());
  ASSERT_EQ(certificates()->GetModel()->GetItemCount(), 2u);
  certificates()->SetSelectedIndex(1);
  pin()->SetText(u"123456");
  ASSERT_NO_FATAL_FAILURE(AcceptAndWait());
  ASSERT_EQ(callback_count_, 1);
  EXPECT_EQ(selected_, 1u);
  ASSERT_TRUE(result_);
  EXPECT_EQ(result_->value(),
            (std::vector<uint8_t>{'1', '2', '3', '4', '5', '6'}));
  EXPECT_FALSE(widget_);
}

TEST_F(GmPinDialogTest, MaximumLengthPrintablePinIsAccepted) {
  ASSERT_NO_FATAL_FAILURE(Show());
  pin()->SetText(std::u16string(64, u'9'));
  ASSERT_NO_FATAL_FAILURE(AcceptAndWait());
  ASSERT_EQ(callback_count_, 1);
  ASSERT_TRUE(result_);
  EXPECT_EQ(result_->value(), std::vector<uint8_t>(64, '9'));
}

TEST_F(GmPinDialogTest, CancelClearsTheFieldAndReturnsNoPinOnce) {
  ASSERT_NO_FATAL_FAILURE(Show());
  pin()->SetText(u"123456");
  inspect_cleared_field_ = true;
  views::test::WidgetDestroyedWaiter closed(widget_.get());
  delegate()->CancelDialog();
  closed.Wait();
  EXPECT_EQ(callback_count_, 1);
  EXPECT_FALSE(result_);
  EXPECT_FALSE(widget_);
}

TEST_F(GmPinDialogTest, ClosingTheWindowReturnsNoPinOnce) {
  ASSERT_NO_FATAL_FAILURE(Show());
  pin()->SetText(u"123456");
  views::test::WidgetDestroyedWaiter closed(widget_.get());
  widget_->CloseWithReason(views::Widget::ClosedReason::kCloseButtonClicked);
  closed.Wait();
  EXPECT_EQ(callback_count_, 1);
  EXPECT_FALSE(result_);
}

TEST_F(GmPinDialogTest, OwnerDestroyingTheWindowCancelsPendingPin) {
  ASSERT_NO_FATAL_FAILURE(Show());
  pin()->SetText(u"123456");
  widget_->CloseNow();
  EXPECT_EQ(callback_count_, 1);
  EXPECT_FALSE(result_);
  EXPECT_FALSE(widget_);
}

TEST_F(GmPinDialogTest, NativeWindowDestructionCancelsPendingPinOnce) {
  ASSERT_NO_FATAL_FAILURE(Show());
  pin()->SetText(u"123456");
  const HWND window = views::HWNDForWidget(widget_.get());
  ASSERT_TRUE(window);
  views::test::WidgetDestroyedWaiter closed(widget_.get());
  // Bypass Widget::Close/CloseNow, as happens when an owning native window or
  // the platform destroys the HWND. OnNativeWidgetDestroyed must still release
  // the client-owned widget and delegate and discard the pending PIN.
  ASSERT_TRUE(::DestroyWindow(window));
  closed.Wait();
  EXPECT_EQ(callback_count_, 1);
  EXPECT_FALSE(result_);
  EXPECT_FALSE(widget_);
}

TEST_F(GmPinDialogTest, NoSelectedCertificateCannotAuthorize) {
  ASSERT_NO_FATAL_FAILURE(Show());
  certificates()->SetSelectedIndex(std::nullopt);
  pin()->SetText(u"123456");
  delegate()->AcceptDialog();
  EXPECT_EQ(callback_count_, 0);
  EXPECT_FALSE(result_);
  ASSERT_TRUE(widget_);
  EXPECT_TRUE(
      HasLabel(widget_->GetContentsView(),
               l10n_util::GetStringUTF16(IDS_GM_PIN_SELECT_CERTIFICATE)));
  certificates()->SetSelectedIndex(0);
  ASSERT_NO_FATAL_FAILURE(AcceptAndWait());
  EXPECT_EQ(callback_count_, 1);
  ASSERT_TRUE(result_);
}

TEST_F(GmPinDialogTest, MissingPageCancelsWithoutCreatingWindow) {
  widget_ = ShowPinDialogForWebContents(nullptr, "https://gm.example",
                                        {u"Certificate"}, {}, ResultCallback());
  EXPECT_FALSE(widget_);
  EXPECT_EQ(callback_count_, 1);
  EXPECT_FALSE(result_);
}

TEST_F(GmPinDialogTest, MissingPageDismissesLockedNoticeWithoutCreatingWindow) {
  bool closed = false;
  widget_ = ShowPinLockedDialogForWebContents(
      nullptr, "https://gm.example",
      base::BindOnce([](bool* closed) { *closed = true; }, &closed));
  EXPECT_FALSE(widget_);
  EXPECT_TRUE(closed);
}

class GmPinDialogInvalidInputTest
    : public GmPinDialogTest,
      public testing::WithParamInterface<std::u16string> {};

TEST_P(GmPinDialogInvalidInputTest, InvalidInputDoesNotReturnAnAuthorizedPin) {
  ASSERT_NO_FATAL_FAILURE(Show());
  pin()->SetText(GetParam());
  delegate()->AcceptDialog();
  EXPECT_EQ(callback_count_, 0);
  EXPECT_FALSE(result_);
  ASSERT_TRUE(widget_);
  EXPECT_TRUE(HasLabel(widget_->GetContentsView(),
                       l10n_util::GetStringUTF16(IDS_GM_PIN_INVALID_INPUT)));
  pin()->SetText(u"123456");
  ASSERT_NO_FATAL_FAILURE(AcceptAndWait());
  EXPECT_EQ(callback_count_, 1);
  ASSERT_TRUE(result_);
}

INSTANTIATE_TEST_SUITE_P(InvalidPin,
                         GmPinDialogInvalidInputTest,
                         testing::Values(std::u16string(),
                                         std::u16string(65, u'1'),
                                         std::u16string(u"中文PIN"),
                                         std::u16string(1, u'\0'),
                                         std::u16string(1, u'\x1f'),
                                         std::u16string(1, u'\x7f')));

}  // namespace
}  // namespace gm_browser
