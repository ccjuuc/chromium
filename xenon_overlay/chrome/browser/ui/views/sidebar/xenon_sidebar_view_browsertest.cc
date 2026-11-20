// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ui/views/sidebar/xenon_sidebar_view.h"

#include <memory>
#include <string>
#include <string_view>

#include "base/base_paths.h"
#include "base/command_line.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/task/execution_fence.h"
#include "base/test/scoped_feature_list.h"
#include "base/test/scoped_path_override.h"
#include "base/test/test_future.h"
#include "base/threading/thread_restrictions.h"
#include "base/values.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/testing_profile.h"
#include "chrome/test/base/ui_test_utils.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "content/public/test/test_utils.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/base/ui_base_features.h"
#include "ui/views/controls/button/label_button.h"
#include "ui/views/controls/scroll_view.h"
#include "ui/views/test/button_test_api.h"
#include "ui/views/test/views_test_utils.h"
#include "ui/views/view_utils.h"
#include "ui/views/widget/widget.h"
#include "url/gurl.h"
#include "xenon_overlay/buildflags/buildflags.h"
#include "xenon_overlay/chrome/browser/ui/xenon_web_dialog.h"
#include "xenon_overlay/chrome/browser/xenon_manager.h"
#include "xenon_overlay/public/mojom/xenon_ipc.mojom.h"

#if BUILDFLAG(ENABLE_XENON_SERVICE)
#include "xenon_overlay/chrome/browser/updater/xenon_update_manager.h"
#endif

namespace xenon {
namespace {

constexpr char kElectronAppDescriptor[] = "sidebar-async-fixture.xenon.json";

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

views::LabelButton* FindButton(views::View* root, std::u16string_view text) {
  if (auto* button = views::AsViewClass<views::LabelButton>(root);
      button && button->GetText() == text) {
    return button;
  }
  for (views::View* child : root->children()) {
    if (auto* button = FindButton(child, text)) {
      return button;
    }
  }
  return nullptr;
}

class XenonSidebarBrowserTest : public InProcessBrowserTest,
                                public testing::WithParamInterface<bool> {
 public:
  XenonSidebarBrowserTest() {
    // Bounds scrolling is the Windows/Linux default; layer scrolling must
    // preserve the same visible shortcuts and fixed controls.
    features_.InitWithFeatureState(features::kUiCompositorScrollWithLayers,
                                   GetParam());
  }

  void SetUpCommandLine(base::CommandLine* command_line) override {
    InProcessBrowserTest::SetUpCommandLine(command_line);
    command_line->AppendSwitch("disable-xenon-login-gate");
    command_line->AppendSwitch("disable-xenon-sidebar-auto-hide");
  }

  void SetUpInProcessBrowserTestFixture() override {
    InProcessBrowserTest::SetUpInProcessBrowserTestFixture();
#if BUILDFLAG(ENABLE_XENON_SERVICE)
    ASSERT_TRUE(updater_install_directory_.CreateUniqueTempDir());
    auto* manager = updater::XenonUpdateManager::GetInstance();
    manager->SetSystemUpdaterForTesting(nullptr);
    manager->SetInstallDirForTesting(updater_install_directory_.GetPath());
    manager->SetInstallerPackageCacheRootForTesting(
        updater_install_directory_.GetPath().AppendASCII("packages"));
    manager->SetAutoDownload(false);
    manager->SetAutoInstallOnAppQuit(false);
#endif
  }

 protected:
  XenonSidebarView* sidebar() {
    return FindView<XenonSidebarView>(
        BrowserView::GetBrowserViewForBrowser(browser()));
  }

  void ResizeBrowser(int height) {
    auto* widget =
        BrowserView::GetBrowserViewForBrowser(browser())->GetWidget();
    widget->Restore();
    widget->SetBounds(gfx::Rect(0, 0, 1000, height));
    base::test::TestFuture<void> layout_presented;
    if (GetParam()) {
      auto* scroll = FindView<views::ScrollView>(sidebar());
      ASSERT_TRUE(scroll);
      // Layer scrolling uses the compositor's active scroll bounds. Wait for
      // the resized viewport to reach that tree before attempting to scroll.
      scroll->RegisterNextSuccessfulFramePostLayoutCallback(
          layout_presented.GetCallback());
      scroll->InvalidateLayout();
    }
    views::test::RunScheduledLayout(widget);
    ASSERT_EQ(height, widget->GetWindowBoundsInScreen().height());
    if (GetParam()) {
      ASSERT_TRUE(layout_presented.Wait());
    }
  }

  void CreateElectronAppFixture() {
    base::ScopedAllowBlockingForTesting allow_blocking;
    ASSERT_TRUE(electron_app_directory_.CreateUniqueTempDir());
    const auto app = electron_app_directory_.GetPath().AppendASCII("app");
    ASSERT_TRUE(base::CreateDirectory(app));
    ASSERT_TRUE(base::WriteFile(
        app.AppendASCII("package.json"),
        R"({"name":"sidebar-fixture","version":"1.0.0","main":"main.cjs"})"));
    ASSERT_TRUE(base::WriteFile(app.AppendASCII("main.cjs"), R"JS(
      const fs = require('node:fs');
      const path = require('node:path');
      fs.appendFileSync(path.join(__dirname, 'launches.txt'), 'launch\n');
    )JS"));
    ASSERT_TRUE(base::WriteFile(
        electron_app_directory_.GetPath().AppendASCII(kElectronAppDescriptor),
        R"({"application":"app"})"));
  }

  void WaitForElectronMain(const std::string& container_id) {
    auto* manager = XenonManager::GetInstance();
    // An ordinary request shares the initialization pipe and is queued until
    // main has loaded. Its expected missing-endpoint error gives us a real
    // Utility reply after all preceding initialization messages, without a
    // test-only launcher or timing-based wait.
    base::test::TestFuture<ipc::mojom::IpcResultPtr> reply;
    manager->ElectronIpcInvoke(
        container_id, "unregistered-sidebar-test", "startup-barrier",
        base::Value(base::ListValue()), reply.GetCallback(),
        manager->service_generation(container_id));
    ASSERT_TRUE(reply.Wait());
    ASSERT_TRUE(reply.Get());
    EXPECT_FALSE(reply.Get()->success);
    EXPECT_EQ("Renderer IPC endpoint is unavailable", reply.Get()->error);
  }

  std::string ReadElectronLaunches() {
    base::ScopedAllowBlockingForTesting allow_blocking;
    std::string contents;
    EXPECT_TRUE(base::ReadFileToString(
        electron_app_directory_.GetPath().AppendASCII("app").AppendASCII(
            "launches.txt"),
        &contents));
    return contents;
  }

  base::ScopedTempDir electron_app_directory_;

 private:
  base::test::ScopedFeatureList features_;
#if BUILDFLAG(ENABLE_XENON_SERVICE)
  base::ScopedTempDir updater_install_directory_;
#endif
};

IN_PROC_BROWSER_TEST_P(XenonSidebarBrowserTest,
                       ShortcutsVisibleAfterResizeAndScroll) {
  ASSERT_TRUE(sidebar());
  auto* scroll = FindView<views::ScrollView>(sidebar());
  ASSERT_TRUE(scroll);
  auto* first = FindButton(scroll->contents(), u"CRT");
  auto* last = FindButton(scroll->contents(), u"FLG");
  ASSERT_TRUE(first);
  ASSERT_TRUE(last);

  // Re-expand after shrinking to catch stale content or viewport dimensions.
  for (int height : {650, 400, 650}) {
    SCOPED_TRACE(height);
    ASSERT_NO_FATAL_FAILURE(ResizeBrowser(height));
    ASSERT_TRUE(sidebar()->IsDrawn());
    first->ScrollViewToVisible();
    EXPECT_EQ(gfx::Size(36, 36), first->size());
    EXPECT_EQ(36, scroll->contents()->width());
    EXPECT_EQ(36, scroll->GetVisibleRect().width());
    EXPECT_GT(scroll->contents()->height(), scroll->GetVisibleRect().height());
    EXPECT_TRUE(scroll->GetVisibleRect().Contains(first->bounds()));
    EXPECT_EQ(first->GetLocalBounds(), first->GetVisibleBounds());

    auto* left = FindButton(sidebar(), u"左");
    auto* right = FindButton(sidebar(), u"右");
    auto* hide = FindButton(sidebar(), u"隐");
    ASSERT_TRUE(left);
    ASSERT_TRUE(right);
    ASSERT_TRUE(hide);
    const gfx::Rect left_bounds = left->bounds();
    const gfx::Rect right_bounds = right->bounds();
    const gfx::Rect hide_bounds = hide->bounds();

    last->ScrollViewToVisible();
    EXPECT_GT(scroll->CurrentOffset().y(), 0);
    EXPECT_EQ(gfx::Size(36, 36), last->size());
    EXPECT_TRUE(scroll->GetVisibleRect().Contains(last->bounds()));
    EXPECT_EQ(last->GetLocalBounds(), last->GetVisibleBounds());
    EXPECT_EQ(left_bounds, left->bounds());
    EXPECT_EQ(right_bounds, right->bounds());
    EXPECT_EQ(hide_bounds, hide->bounds());
    for (auto* control : {left, right, hide}) {
      EXPECT_EQ(gfx::Size(36, 24), control->size());
      EXPECT_EQ(control->GetLocalBounds(), control->GetVisibleBounds());
      EXPECT_GE(control->y(), scroll->bounds().bottom());
      EXPECT_TRUE(sidebar()->GetLocalBounds().Contains(control->bounds()));
    }
  }
}

IN_PROC_BROWSER_TEST_P(XenonSidebarBrowserTest,
                       CertificateShortcutOpensDiagnosticPage) {
  ASSERT_NO_FATAL_FAILURE(ResizeBrowser(650));
  ASSERT_TRUE(sidebar());
  auto* certificate = FindButton(sidebar(), u"CRT");
  ASSERT_TRUE(certificate);
  EXPECT_EQ(certificate->GetLocalBounds(), certificate->GetVisibleBounds());
  const int initial_tabs = browser()->GetTabStripModel()->count();
  ui_test_utils::TabAddedWaiter waiter(browser());
  views::test::ButtonTestApi(certificate).NotifyDefaultMouseClick();
  auto* contents = waiter.Wait();
  ASSERT_TRUE(contents);
  ASSERT_TRUE(content::WaitForLoadStop(contents));
  EXPECT_EQ(initial_tabs + 1, browser()->GetTabStripModel()->count());
  EXPECT_EQ(contents, browser()->GetTabStripModel()->GetActiveWebContents());
  EXPECT_EQ(GURL("chrome://xenon-certificates/"),
            contents->GetLastCommittedURL());
  EXPECT_EQ(true, content::EvalJs(contents, R"JS(
    (() => {
      const urls = Array.from(document.querySelectorAll(
          '#test-preset optgroup[label="国密测试"] option'), option => option.value);
      return ['https://test1.gmssl.cn/', 'https://sm2test.ovssl.cn/']
          .every(url => urls.includes(url));
    })()
  )JS"));
}

IN_PROC_BROWSER_TEST_P(XenonSidebarBrowserTest,
                       ElectronAppLoadingIsDeferredAndRepeatedClicksReuseMain) {
  ASSERT_NO_FATAL_FAILURE(CreateElectronAppFixture());
  base::ScopedPathOverride descriptor_directory(
      base::DIR_EXE, electron_app_directory_.GetPath(), /*is_absolute=*/true,
      /*create=*/false);
  constexpr char kContainer[] = "sidebar-deferred-launch";
  auto* manager = XenonManager::GetInstance();
  ASSERT_EQ(0u, manager->service_generation(kContainer));
  {
    base::ScopedThreadPoolExecutionFence block_background_work;
    base::ScopedDisallowBlocking disallow_blocking;
    XenonWebDialog::ShowElectronApp(browser()->GetProfile(),
                                    kElectronAppDescriptor, kContainer);
    XenonWebDialog::ShowElectronApp(browser()->GetProfile(),
                                    kElectronAppDescriptor, kContainer);
    // Even a synchronous implementation which hides its I/O with an allowance
    // must fail this assertion: no background preparation has run yet.
    EXPECT_EQ(0u, manager->service_generation(kContainer));
    EXPECT_FALSE(manager->GetElectronIpcRendererConfigForContainer(kContainer));
  }
  {
    base::ScopedDisallowBlocking disallow_blocking;
    content::RunAllTasksUntilIdle();
    ASSERT_EQ(1u, manager->service_generation(kContainer));
    ASSERT_NO_FATAL_FAILURE(WaitForElectronMain(kContainer));
  }
  EXPECT_EQ("launch\n", ReadElectronLaunches());

  // A later click must also preserve the running main module and avoid UI I/O.
  {
    base::ScopedDisallowBlocking disallow_blocking;
    XenonWebDialog::ShowElectronApp(browser()->GetProfile(),
                                    kElectronAppDescriptor, kContainer);
    content::RunAllTasksUntilIdle();
    ASSERT_EQ(1u, manager->service_generation(kContainer));
    ASSERT_NO_FATAL_FAILURE(WaitForElectronMain(kContainer));
  }
  EXPECT_EQ("launch\n", ReadElectronLaunches());
}

IN_PROC_BROWSER_TEST_P(XenonSidebarBrowserTest,
                       ElectronAppLoadingStopsWhenProfileIsDestroyed) {
  ASSERT_NO_FATAL_FAILURE(CreateElectronAppFixture());
  base::ScopedPathOverride descriptor_directory(
      base::DIR_EXE, electron_app_directory_.GetPath(), /*is_absolute=*/true,
      /*create=*/false);
  constexpr char kContainer[] = "sidebar-destroyed-profile";
  auto* manager = XenonManager::GetInstance();
  std::unique_ptr<TestingProfile> profile;
  {
    base::ScopedAllowBlockingForTesting allow_fixture_setup;
    profile = std::make_unique<TestingProfile>();
  }
  {
    base::ScopedThreadPoolExecutionFence block_background_work;
    {
      base::ScopedDisallowBlocking disallow_blocking;
      XenonWebDialog::ShowElectronApp(profile.get(), kElectronAppDescriptor,
                                      kContainer);
      EXPECT_EQ(0u, manager->service_generation(kContainer));
      EXPECT_FALSE(
          manager->GetElectronIpcRendererConfigForContainer(kContainer));
    }
    profile.reset();
  }
  {
    base::ScopedDisallowBlocking disallow_blocking;
    // Drain the real preparation task and its UI reply after releasing the
    // fence. Checking before that reply would miss a stale-profile launch.
    content::RunAllTasksUntilIdle();
  }
  EXPECT_EQ(0u, manager->service_generation(kContainer));
  EXPECT_FALSE(manager->GetElectronIpcRendererConfigForContainer(kContainer));
}

INSTANTIATE_TEST_SUITE_P(ScrollModes, XenonSidebarBrowserTest, testing::Bool());

}  // namespace
}  // namespace xenon
