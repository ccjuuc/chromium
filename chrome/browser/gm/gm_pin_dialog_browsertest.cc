// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/gm/gm_pin_dialog.h"

#include <memory>
#include <utility>
#include <vector>

#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/run_loop.h"
#include "base/test/run_until.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "components/web_modal/web_contents_modal_dialog_manager.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "ui/base/mojom/dialog_button.mojom.h"
#include "ui/base/mojom/ui_base_types.mojom-shared.h"
#include "ui/base/page_transition_types.h"
#include "ui/views/controls/combobox/combobox.h"
#include "ui/views/controls/textfield/textfield.h"
#include "ui/views/view_utils.h"
#include "ui/views/widget/widget.h"
#include "ui/views/window/dialog_delegate.h"
#include "url/gurl.h"

#if defined(GM_BROWSER_TEST_HAS_XENON_UPDATER)
#include "xenon_overlay/chrome/browser/updater/xenon_update_manager.h"
#endif

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

// These tests use the native page-modal UI directly. They do not install a
// KeyManager prompt callback, load an SKF driver, or authorize a hardware Key.
class GmPinDialogBrowserTest : public InProcessBrowserTest {
 public:
  void SetUpInProcessBrowserTestFixture() override {
    InProcessBrowserTest::SetUpInProcessBrowserTestFixture();
#if defined(GM_BROWSER_TEST_HAS_XENON_UPDATER)
    // Keep updater staging/cleanup inside the test's temporary installation,
    // matching the other tests in this focused browser-test executable.
    ASSERT_TRUE(updater_install_directory_.CreateUniqueTempDir());
    auto* updater = xenon::updater::XenonUpdateManager::GetInstance();
    updater->SetSystemUpdaterForTesting(nullptr);
    updater->SetInstallDirForTesting(updater_install_directory_.GetPath());
    updater->SetInstallerPackageCacheRootForTesting(
        updater_install_directory_.GetPath().AppendASCII("packages"));
    updater->SetAutoDownload(false);
    updater->SetAutoInstallOnAppQuit(false);
#endif
  }

  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();
    ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), GURL("about:blank")));
  }

  void TearDownOnMainThread() override {
    if (widget_) {
      widget_->CloseNow();
    }
    InProcessBrowserTest::TearDownOnMainThread();
  }

 protected:
  void Show(content::WebContents* web_contents) {
    widget_ = ShowPinDialogForWebContents(
        web_contents, "https://gm.example:8443",
        {u"Signing certificate A", u"Signing certificate B"}, {},
        base::BindOnce(&GmPinDialogBrowserTest::OnResult,
                       base::Unretained(this)));
    ASSERT_NO_FATAL_FAILURE(ExpectPageModal(web_contents));
    ASSERT_TRUE(pin());
    EXPECT_EQ(pin()->GetTextInputType(), ui::TEXT_INPUT_TYPE_PASSWORD);
    ASSERT_TRUE(FindView<views::Combobox>(widget_->GetContentsView()));
    EXPECT_EQ(callback_count_, 0);
  }

  void ExpectPageModal(content::WebContents* web_contents) {
    ASSERT_TRUE(widget_);
    ASSERT_TRUE(widget_->GetNativeWindow());
    ASSERT_TRUE(delegate());
    EXPECT_EQ(delegate()->GetModalType(), ui::mojom::ModalType::kChild);
    auto* manager =
        web_modal::WebContentsModalDialogManager::FromWebContents(web_contents);
    ASSERT_TRUE(manager);
    EXPECT_TRUE(manager->IsDialogActive());
    ASSERT_TRUE(
        base::test::RunUntil([&] { return widget_ && widget_->IsVisible(); }));
  }

  void ExpectClosed() {
    ASSERT_TRUE(base::test::RunUntil([&] { return !widget_; }));
    base::RunLoop().RunUntilIdle();
  }

  views::Textfield* pin() const {
    return widget_ ? FindView<views::Textfield>(widget_->GetContentsView())
                   : nullptr;
  }

  views::DialogDelegate* delegate() const {
    return widget_ ? widget_->widget_delegate()->AsDialogDelegate() : nullptr;
  }

  base::WeakPtr<views::Widget> widget_;
  int callback_count_ = 0;
  size_t selected_ = 0;
  std::unique_ptr<gm_key::SecurePin> result_;
  int locked_notice_close_count_ = 0;

 private:
  void OnResult(size_t selected, std::unique_ptr<gm_key::SecurePin> value) {
    ++callback_count_;
    selected_ = selected;
    result_ = std::move(value);
  }

#if defined(GM_BROWSER_TEST_HAS_XENON_UPDATER)
  base::ScopedTempDir updater_install_directory_;
#endif
};

IN_PROC_BROWSER_TEST_F(GmPinDialogBrowserTest,
                       PageModalAcceptReturnsSelectedPinOnce) {
  auto* web_contents = browser()->GetTabStripModel()->GetActiveWebContents();
  ASSERT_NO_FATAL_FAILURE(Show(web_contents));
  FindView<views::Combobox>(widget_->GetContentsView())->SetSelectedIndex(1);
  pin()->SetText(u"123456");
  delegate()->AcceptDialog();
  ASSERT_NO_FATAL_FAILURE(ExpectClosed());
  EXPECT_EQ(callback_count_, 1);
  EXPECT_EQ(selected_, 1u);
  ASSERT_TRUE(result_);
  EXPECT_EQ(result_->value(),
            (std::vector<uint8_t>{'1', '2', '3', '4', '5', '6'}));
  EXPECT_FALSE(
      web_modal::WebContentsModalDialogManager::FromWebContents(web_contents)
          ->IsDialogActive());
}

IN_PROC_BROWSER_TEST_F(GmPinDialogBrowserTest,
                       ClosingOwningTabCancelsPendingPinOnce) {
  ASSERT_TRUE(AddTabAtIndex(1, GURL("about:blank"), ui::PAGE_TRANSITION_TYPED));
  auto* tabs = browser()->GetTabStripModel();
  ASSERT_EQ(tabs->count(), 2);
  ASSERT_NO_FATAL_FAILURE(Show(tabs->GetActiveWebContents()));
  pin()->SetText(u"123456");
  tabs->CloseWebContentsAt(tabs->active_index(),
                           TabCloseTypes::CLOSE_USER_GESTURE);
  ASSERT_NO_FATAL_FAILURE(ExpectClosed());
  EXPECT_EQ(tabs->count(), 1);
  EXPECT_EQ(callback_count_, 1);
  EXPECT_FALSE(result_);
}

IN_PROC_BROWSER_TEST_F(GmPinDialogBrowserTest,
                       ClosingOwningBrowserCancelsPendingPinOnce) {
  // Keep the fixture's original browser alive while destroying the host.
  Browser* host = CreateBrowser(browser()->GetProfile());
  ASSERT_NO_FATAL_FAILURE(
      Show(host->GetTabStripModel()->GetActiveWebContents()));
  pin()->SetText(u"123456");
  CloseBrowserSynchronously(host);
  ASSERT_NO_FATAL_FAILURE(ExpectClosed());
  EXPECT_EQ(callback_count_, 1);
  EXPECT_FALSE(result_);
}

IN_PROC_BROWSER_TEST_F(GmPinDialogBrowserTest,
                       LockedNoticeIsPageModalAndClosesOnce) {
  auto* web_contents = browser()->GetTabStripModel()->GetActiveWebContents();
  widget_ = ShowPinLockedDialogForWebContents(
      web_contents, "https://gm.example:8443",
      base::BindOnce([](int* count) { ++*count; },
                     &locked_notice_close_count_));
  ASSERT_NO_FATAL_FAILURE(ExpectPageModal(web_contents));
  EXPECT_FALSE(pin());
  EXPECT_EQ(delegate()->buttons(),
            static_cast<int>(ui::mojom::DialogButton::kOk));
  EXPECT_EQ(locked_notice_close_count_, 0);
  delegate()->AcceptDialog();
  ASSERT_NO_FATAL_FAILURE(ExpectClosed());
  EXPECT_EQ(locked_notice_close_count_, 1);
  EXPECT_FALSE(
      web_modal::WebContentsModalDialogManager::FromWebContents(web_contents)
          ->IsDialogActive());
}

}  // namespace
}  // namespace gm_browser
