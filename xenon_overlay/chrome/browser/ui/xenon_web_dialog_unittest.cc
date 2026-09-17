// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ui/xenon_web_dialog.h"

#include <memory>
#include <string>

#include "base/functional/bind.h"
#include "base/run_loop.h"
#include "base/test/task_environment.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/navigation_simulator.h"
#include "content/public/test/test_renderer_host.h"
#include "net/base/net_errors.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/common/user_agent/user_agent_metadata.h"
#include "ui/views/widget/widget.h"
#include "xenon_overlay/chrome/browser/ui/xenon_electron_window_host.h"

namespace xenon {

// These tests exercise the real delegate/host close decisions and entry
// selection without creating HWNDs, WebContents, or an application container.
class XenonHostedWindowCloseTest : public testing::Test {
 protected:
  void SetUp() override {
    dialog_ = new XenonWebDialog(
        GURL("about:blank"), 320, 200, u"fixture",
        ui::mojom::ModalType::kNone, base::OnceClosure(), true, false, false,
        false, false);
    host_ = new XenonElectronWindowHost();
    // Selection needs an identity, not an initialized native window.
    widget_ = std::make_unique<views::Widget>();
  }

  void TearDown() override {
    // The fixture identities were never registered as Widget observers.
    host_->windows_.clear();
    delete host_;
    delete dialog_;
    widget_.reset();
  }

  void AttachCloseHandler() {
    dialog_->SetCloseRequestHandler(base::BindRepeating(
        &XenonHostedWindowCloseTest::OnCloseRequested, base::Unretained(this)));
  }

  bool OnCloseRequested() {
    ++close_requests_;
    return authorized_;
  }

  bool RequestNativeClose() {
    return static_cast<ui::WebDialogDelegate*>(dialog_)->OnDialogCloseRequested();
  }

  bool RequestDomClose() {
    bool close = true;
    static_cast<ui::WebDialogDelegate*>(dialog_)->OnCloseContents(nullptr, &close);
    return close;
  }

  void DisableOrdinaryNativeClose() { dialog_->set_can_close(false); }

  void AddWindow(int32_t id,
                 bool loaded,
                 bool shown,
                 int32_t parent = 0,
                 const std::string& container = "fixture") {
    auto& entry = host_->windows_[id];
    entry.widget = widget_.get();
    entry.container_id = container;
    entry.has_loaded_url = loaded;
    entry.ever_shown = shown;
    entry.parent_id = parent;
    entry.sync_bounds_with_parent = parent > 0;
  }

  int32_t EntryWindow() {
    return host_->FindEntryWindowForContainer("fixture");
  }

  void AuthorizeWindow(int32_t id) {
    host_->windows_.at(id).close_authorized = true;
  }

  bool RequestWindowClose(int32_t id) { return host_->RequestClose(id); }
  void BeginContainerFailure() { host_->closing_containers_.insert("fixture"); }
  void BeginShutdown() { host_->shutting_down_ = true; }

  base::test::TaskEnvironment task_environment_;
  std::unique_ptr<views::Widget> widget_;
  XenonWebDialog* dialog_ = nullptr;
  XenonElectronWindowHost* host_ = nullptr;
  bool authorized_ = false;
  int close_requests_ = 0;
};

TEST_F(XenonHostedWindowCloseTest, OrdinaryDialogsKeepNativeAndDomClose) {
  EXPECT_TRUE(RequestNativeClose());
  EXPECT_TRUE(RequestDomClose());
  DisableOrdinaryNativeClose();
  EXPECT_FALSE(RequestNativeClose());
  EXPECT_TRUE(RequestDomClose());
}

TEST_F(XenonHostedWindowCloseTest, HostedNativeAndDomCloseWaitForMainDecision) {
  AttachCloseHandler();
  EXPECT_FALSE(RequestNativeClose());
  EXPECT_FALSE(RequestDomClose());
  EXPECT_FALSE(RequestNativeClose());
  EXPECT_EQ(3, close_requests_);

  authorized_ = true;
  EXPECT_TRUE(RequestNativeClose());
  EXPECT_TRUE(RequestDomClose());
}

TEST_F(XenonHostedWindowCloseTest, HostedEscapeDoesNotRequestWindowClose) {
  auto* delegate = static_cast<ui::WebDialogDelegate*>(dialog_);
  EXPECT_TRUE(delegate->ShouldCloseDialogOnEscape());

  AttachCloseHandler();
  EXPECT_FALSE(delegate->ShouldCloseDialogOnEscape());
  authorized_ = true;
  EXPECT_FALSE(delegate->ShouldCloseDialogOnEscape());
  EXPECT_EQ(0, close_requests_);

  // Removing the host restores ordinary WebDialog policy.
  dialog_->SetCloseRequestHandler(base::RepeatingCallback<bool()>());
  EXPECT_TRUE(delegate->ShouldCloseDialogOnEscape());
}

TEST_F(XenonHostedWindowCloseTest, OrdinaryEscapeKeepsDelegateConfiguration) {
  auto* delegate = static_cast<ui::WebDialogDelegate*>(dialog_);
  delegate->set_close_dialog_on_escape(false);
  EXPECT_FALSE(delegate->ShouldCloseDialogOnEscape());
  AttachCloseHandler();
  EXPECT_FALSE(delegate->ShouldCloseDialogOnEscape());
  dialog_->SetCloseRequestHandler(base::RepeatingCallback<bool()>());
  EXPECT_FALSE(delegate->ShouldCloseDialogOnEscape());
}

TEST_F(XenonHostedWindowCloseTest, MainAuthorizedCloseBypassesAnotherRequest) {
  AddWindow(1, true, true);
  EXPECT_FALSE(RequestWindowClose(1));
  AuthorizeWindow(1);
  EXPECT_TRUE(RequestWindowClose(1));
  EXPECT_TRUE(RequestWindowClose(99));
}

TEST_F(XenonHostedWindowCloseTest, ContainerFailureAndShutdownCannotBeVetoed) {
  AddWindow(1, true, true);
  AddWindow(2, true, true, 0, "other");
  BeginContainerFailure();
  EXPECT_TRUE(RequestWindowClose(1));
  EXPECT_FALSE(RequestWindowClose(2));
  BeginShutdown();
  EXPECT_TRUE(RequestWindowClose(2));
}

TEST_F(XenonHostedWindowCloseTest, NeverShownHelpersCannotBecomeEntryWindows) {
  AddWindow(1, true, false);
  AddWindow(2, false, true);
  AddWindow(3, true, true, 0, "other");
  EXPECT_EQ(0, EntryWindow());
  // A main window hidden by its cancellable close remains the same entry.
  AddWindow(4, true, true);
  EXPECT_EQ(4, EntryWindow());
  AuthorizeWindow(4);
  EXPECT_EQ(0, EntryWindow());
}

TEST_F(XenonHostedWindowCloseTest, EntryPrefersTopLevelButSupportsOwnedContent) {
  AddWindow(1, false, true);
  AddWindow(2, true, true, 1);
  EXPECT_EQ(2, EntryWindow());
  AddWindow(3, true, true);
  EXPECT_EQ(3, EntryWindow());
}

class XenonHostedWindowUserAgentTest
    : public content::RenderViewHostTestHarness {
 protected:
  void SetUp() override {
    content::RenderViewHostTestHarness::SetUp();
    host_ = new XenonElectronWindowHost();
    // Exercise real navigation/UA behavior without routing events to ipcMain.
    host_->shutting_down_ = true;
    host_->windows_[1].has_loaded_url = true;
    host_->ObserveWebContents(1, web_contents());
    NavigateAndCommit(GURL("about:blank"));
    SetUserAgent("fixture/initial");
  }

  void TearDown() override {
    delete host_;
    content::RenderViewHostTestHarness::TearDown();
  }

  void SetUserAgent(const std::string& value) {
    host_->UpdateUserAgent(1, web_contents(), value);
  }

  std::string AppliedUserAgent() {
    return web_contents()->GetUserAgentOverride().ua_string_override;
  }

  std::unique_ptr<content::NavigationSimulator> StartNavigation(
      const GURL& url = GURL("https://fixture.test/player")) {
    content::NavigationController::LoadURLParams params(url);
    params.override_user_agent =
        content::NavigationController::UA_OVERRIDE_TRUE;
    web_contents()->GetController().LoadURLWithParams(params);
    return content::NavigationSimulator::CreateFromPending(
        web_contents()->GetController());
  }

  void RemoveWindow() { host_->windows_.clear(); }

  void SetHasLoadedUrl(bool loaded) {
    host_->windows_.at(1).has_loaded_url = loaded;
  }

  XenonElectronWindowHost* host_ = nullptr;
};

TEST_F(XenonHostedWindowUserAgentTest,
       UpdateDuringNavigationDoesNotReloadPreviousBlankDocument) {
  auto navigation = StartNavigation();
  navigation->ReadyToCommit();
  auto* pending = web_contents()->GetController().GetPendingEntry();
  ASSERT_TRUE(pending);

  SetUserAgent("fixture/updated");
  EXPECT_EQ(pending, web_contents()->GetController().GetPendingEntry());
  EXPECT_EQ("fixture/initial", AppliedUserAgent());

  navigation->Commit();
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(GURL("https://fixture.test/player"),
            web_contents()->GetLastCommittedURL());
  EXPECT_FALSE(web_contents()->GetController().GetPendingEntry());
  EXPECT_EQ("fixture/updated", AppliedUserAgent());
}

TEST_F(XenonHostedWindowUserAgentTest,
       InitialDefaultUserAgentAppliesBeforeFirstApplicationLoad) {
  SetHasLoadedUrl(false);
  auto blank = content::NavigationSimulator::CreateBrowserInitiated(
      GURL("about:blank"), web_contents());
  blank->Start();
  ASSERT_TRUE(web_contents()->IsLoading());

  SetUserAgent("fixture/default");
  EXPECT_EQ("fixture/default", AppliedUserAgent());
  blank->Commit();
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ("fixture/default", AppliedUserAgent());
}

TEST_F(XenonHostedWindowUserAgentTest, LatestPendingUserAgentWins) {
  auto navigation = StartNavigation();
  SetUserAgent("fixture/first");
  SetUserAgent("fixture/latest");
  navigation->Commit();
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ("fixture/latest", AppliedUserAgent());
}

TEST_F(XenonHostedWindowUserAgentTest,
       CommittedDocumentFinishesLoadingBeforeUserAgentChanges) {
  auto navigation = StartNavigation();
  navigation->SetKeepLoading(true);
  SetUserAgent("fixture/after-load");
  navigation->Commit();
  base::RunLoop().RunUntilIdle();
  EXPECT_TRUE(web_contents()->IsLoading());
  EXPECT_FALSE(web_contents()->GetController().GetPendingEntry());
  EXPECT_EQ("fixture/initial", AppliedUserAgent());

  navigation->StopLoading();
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(web_contents()->IsLoading());
  EXPECT_FALSE(web_contents()->GetController().GetPendingEntry());
  EXPECT_EQ("fixture/after-load", AppliedUserAgent());
}

TEST_F(XenonHostedWindowUserAgentTest,
       DeferredUpdateDoesNotReplaceNavigationStartedBeforeItsTask) {
  auto first = StartNavigation();
  first->SetKeepLoading(true);
  SetUserAgent("fixture/after-first");
  first->Commit();
  first->StopLoading();

  const GURL next_url("https://fixture.test/next-player");
  auto second = StartNavigation(next_url);
  second->SetKeepLoading(true);
  auto* pending = web_contents()->GetController().GetPendingEntry();
  ASSERT_TRUE(pending);
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(pending, web_contents()->GetController().GetPendingEntry());
  EXPECT_EQ("fixture/initial", AppliedUserAgent());

  SetUserAgent("fixture/after-second");
  second->Commit();
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(web_contents()->GetController().GetPendingEntry());
  EXPECT_EQ("fixture/initial", AppliedUserAgent());
  second->StopLoading();
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(next_url, web_contents()->GetLastCommittedURL());
  EXPECT_FALSE(web_contents()->GetController().GetPendingEntry());
  EXPECT_EQ("fixture/after-second", AppliedUserAgent());
}

TEST_F(XenonHostedWindowUserAgentTest, LoadedDocumentUpdatesImmediately) {
  NavigateAndCommit(GURL("https://fixture.test/player"));
  SetUserAgent("fixture/loaded");
  EXPECT_EQ("fixture/loaded", AppliedUserAgent());
  EXPECT_FALSE(web_contents()->GetController().GetPendingEntry());
  SetUserAgent("");
  EXPECT_TRUE(AppliedUserAgent().empty());
}

TEST_F(XenonHostedWindowUserAgentTest,
       FailedNavigationAppliesPendingUserAgent) {
  auto navigation = StartNavigation();
  SetUserAgent("fixture/after-cancel");
  navigation->Fail(net::ERR_ABORTED);
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(GURL("about:blank"), web_contents()->GetLastCommittedURL());
  EXPECT_EQ("fixture/after-cancel", AppliedUserAgent());
}

TEST_F(XenonHostedWindowUserAgentTest, DestroyedWindowCancelsDeferredUpdate) {
  auto navigation = StartNavigation();
  SetUserAgent("fixture/discarded");
  navigation->Commit();
  RemoveWindow();
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ("fixture/initial", AppliedUserAgent());
}

}  // namespace xenon
