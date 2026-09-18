// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ui/xenon_web_dialog.h"

#include <memory>
#include <set>
#include <string>
#include <vector>

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

// Exercise transaction recording without platform windows. Real Widget bounds
// callbacks use the same recorder; the JavaScript contract tests verify reply
// delivery, listener reentrancy, and late asynchronous events.
class XenonHostedWindowBoundsTest : public testing::Test {
 protected:
  void SetUp() override {
    host_ = new XenonElectronWindowHost();
    host_->shutting_down_ = true;
  }

  void TearDown() override {
    host_->bounds_transaction_ = nullptr;
    host_->bounds_transaction_container_.reset();
    host_->windows_.clear();
    delete host_;
    widgets_.clear();
  }

  void AddWindow(int32_t id,
                 const gfx::Rect& bounds,
                 const std::string& container = "fixture") {
    widgets_.push_back(std::make_unique<views::Widget>());
    auto& entry = host_->windows_[id];
    entry.widget = widgets_.back().get();
    entry.bounds = bounds;
    entry.container_id = container;
  }

  void BeginTransaction() {
    changes_.clear();
    host_->bounds_transaction_ = &changes_;
    host_->bounds_transaction_container_ = "fixture";
  }

  base::Value EndTransaction(int32_t source = 1) {
    host_->bounds_transaction_ = nullptr;
    host_->bounds_transaction_container_.reset();
    return host_->MakeWindowCallReply(source, base::Value("original"),
                                      changes_);
  }

  void ObserveBounds(int32_t id, const gfx::Rect& bounds) {
    host_->RecordBoundsChange(id, bounds);
  }

  void SetInitializing(int32_t id, bool initializing) {
    host_->windows_.at(id).initializing_bounds = initializing;
  }

  void RemoveWindow(int32_t id) { host_->windows_.erase(id); }

  bool Call(int32_t id,
            const std::string& command,
            base::Value* result,
            std::string* error) {
    return host_->Call(id, command, base::Value(), result, error);
  }

  base::test::TaskEnvironment task_environment_;
  std::vector<std::unique_ptr<views::Widget>> widgets_;
  std::vector<XenonElectronWindowHost::BoundsChange> changes_;
  XenonElectronWindowHost* host_ = nullptr;
};

TEST_F(XenonHostedWindowBoundsTest, CoalescesCorrectionsWithoutFalseResize) {
  AddWindow(1, gfx::Rect(10, 20, 300, 200));
  BeginTransaction();
  ObserveBounds(1, gfx::Rect(30, 20, 314, 214));
  ObserveBounds(1, gfx::Rect(30, 20, 300, 200));
  const auto reply = EndTransaction();
  EXPECT_EQ("original", *reply.GetDict().FindString("value"));
  EXPECT_EQ(2, reply.GetDict().FindDouble("boundsRevision"));
  const auto* changes = reply.GetDict().FindList("boundsChanges");
  ASSERT_TRUE(changes);
  ASSERT_EQ(1u, changes->size());
  const auto& change = (*changes)[0].GetDict();
  EXPECT_EQ(1, change.FindInt("windowId"));
  EXPECT_EQ(2, change.FindDouble("revision"));
  EXPECT_EQ(true, change.FindBool("moved"));
  EXPECT_EQ(false, change.FindBool("resized"));
  const auto* bounds = change.FindDict("bounds");
  ASSERT_TRUE(bounds);
  EXPECT_EQ(30, bounds->FindInt("x"));
  EXPECT_EQ(300, bounds->FindInt("width"));
  EXPECT_FALSE(bounds->contains("revision"));
}

TEST_F(XenonHostedWindowBoundsTest,
       RecordsAllAffectedWindowsInFirstChangeOrder) {
  AddWindow(1, gfx::Rect(10, 20, 300, 200));
  AddWindow(2, gfx::Rect(10, 20, 300, 200));
  BeginTransaction();
  ObserveBounds(2, gfx::Rect(10, 20, 400, 200));
  ObserveBounds(1, gfx::Rect(30, 20, 300, 200));
  ObserveBounds(2, gfx::Rect(10, 20, 500, 200));
  const auto reply = EndTransaction();
  const auto* changes = reply.GetDict().FindList("boundsChanges");
  ASSERT_TRUE(changes);
  ASSERT_EQ(2u, changes->size());
  EXPECT_EQ(2, (*changes)[0].GetDict().FindInt("windowId"));
  EXPECT_EQ(2, (*changes)[0].GetDict().FindDouble("revision"));
  EXPECT_EQ(true, (*changes)[0].GetDict().FindBool("resized"));
  EXPECT_EQ(1, (*changes)[1].GetDict().FindInt("windowId"));
  EXPECT_EQ(1, (*changes)[1].GetDict().FindDouble("revision"));
  EXPECT_EQ(false, (*changes)[1].GetDict().FindBool("resized"));
  EXPECT_EQ(1, reply.GetDict().FindDouble("boundsRevision"));
}

TEST_F(XenonHostedWindowBoundsTest, OtherContainersKeepTheirOwnEventRoute) {
  AddWindow(1, gfx::Rect(10, 20, 300, 200));
  AddWindow(2, gfx::Rect(10, 20, 300, 200), "other");
  BeginTransaction();
  ObserveBounds(2, gfx::Rect(10, 20, 500, 200));
  ObserveBounds(1, gfx::Rect(30, 20, 300, 200));
  const auto reply = EndTransaction();
  const auto* changes = reply.GetDict().FindList("boundsChanges");
  ASSERT_TRUE(changes);
  ASSERT_EQ(1u, changes->size());
  EXPECT_EQ(1, (*changes)[0].GetDict().FindInt("windowId"));

  base::Value other;
  std::string error;
  ASSERT_TRUE(Call(2, "get-minimum-size", &other, &error));
  EXPECT_EQ(1, other.GetDict().FindDouble("boundsRevision"));
  EXPECT_TRUE(other.GetDict().FindList("boundsChanges")->empty());
}

TEST_F(XenonHostedWindowBoundsTest,
       ConstructionOnlyEstablishesGeometryBaseline) {
  AddWindow(1, gfx::Rect(0, 0, 800, 600));
  SetInitializing(1, true);
  BeginTransaction();
  ObserveBounds(1, gfx::Rect(10, 20, 300, 200));
  const auto initial = EndTransaction();
  ASSERT_TRUE(initial.GetDict().FindList("boundsChanges"));
  EXPECT_TRUE(initial.GetDict().FindList("boundsChanges")->empty());
  EXPECT_EQ(1, initial.GetDict().FindDouble("boundsRevision"));

  SetInitializing(1, false);
  BeginTransaction();
  ObserveBounds(1, gfx::Rect(30, 20, 300, 200));
  const auto shown = EndTransaction();
  const auto& change =
      shown.GetDict().FindList("boundsChanges")->front().GetDict();
  EXPECT_EQ(true, change.FindBool("moved"));
  EXPECT_EQ(false, change.FindBool("resized"));
  EXPECT_EQ(2, change.FindDouble("revision"));
}

TEST_F(XenonHostedWindowBoundsTest,
       UnchangedFinalBoundsDoNotImplyResizeOrMove) {
  const gfx::Rect bounds(10, 20, 300, 200);
  AddWindow(1, bounds);
  BeginTransaction();
  ObserveBounds(1, bounds);
  const auto unchanged = EndTransaction();
  EXPECT_TRUE(unchanged.GetDict().FindList("boundsChanges")->empty());
  EXPECT_EQ(0, unchanged.GetDict().FindDouble("boundsRevision"));

  BeginTransaction();
  ObserveBounds(1, gfx::Rect(20, 30, 320, 220));
  ObserveBounds(1, bounds);
  const auto corrected = EndTransaction();
  const auto& change =
      corrected.GetDict().FindList("boundsChanges")->front().GetDict();
  EXPECT_EQ(false, change.FindBool("moved"));
  EXPECT_EQ(false, change.FindBool("resized"));
  EXPECT_EQ(2, change.FindDouble("revision"));
}

TEST_F(XenonHostedWindowBoundsTest,
       SeparateCallsKeepSeparateRevisionsAndChanges) {
  AddWindow(1, gfx::Rect(10, 20, 300, 200));
  BeginTransaction();
  ObserveBounds(1, gfx::Rect(10, 20, 500, 200));
  const auto first = EndTransaction();
  EXPECT_EQ(1, first.GetDict().FindDouble("boundsRevision"));

  BeginTransaction();
  ObserveBounds(1, gfx::Rect(10, 20, 300, 200));
  const auto second = EndTransaction();
  EXPECT_EQ(2, second.GetDict().FindDouble("boundsRevision"));
  EXPECT_EQ(true, second.GetDict()
                      .FindList("boundsChanges")
                      ->front()
                      .GetDict()
                      .FindBool("resized"));
}

TEST_F(XenonHostedWindowBoundsTest, DestroyedWindowsAreOmittedFromCallReply) {
  AddWindow(1, gfx::Rect(10, 20, 300, 200));
  AddWindow(2, gfx::Rect(10, 20, 300, 200));
  BeginTransaction();
  ObserveBounds(2, gfx::Rect(20, 30, 400, 300));
  RemoveWindow(2);
  const auto reply = EndTransaction();
  EXPECT_TRUE(reply.GetDict().FindList("boundsChanges")->empty());
}

TEST_F(XenonHostedWindowBoundsTest,
       GetterWrapsValueWithoutConsumingOuterChanges) {
  AddWindow(1, gfx::Rect(10, 20, 300, 200));
  BeginTransaction();
  ObserveBounds(1, gfx::Rect(30, 20, 300, 200));
  base::Value result;
  std::string error;
  ASSERT_TRUE(Call(1, "get-minimum-size", &result, &error));
  EXPECT_TRUE(error.empty());
  EXPECT_EQ(true, result.GetDict().FindBool("__xenonWindowCall"));
  EXPECT_TRUE(result.GetDict().FindList("boundsChanges")->empty());
  EXPECT_EQ(1, result.GetDict().FindDouble("boundsRevision"));
  const auto* size = result.GetDict().FindDict("value");
  ASSERT_TRUE(size);
  EXPECT_EQ(0, size->FindInt("width"));
  EXPECT_EQ(0, size->FindInt("height"));
  ObserveBounds(1, gfx::Rect(40, 20, 300, 200));
  const auto outer = EndTransaction();
  const auto* changes = outer.GetDict().FindList("boundsChanges");
  ASSERT_EQ(1u, changes->size());
  EXPECT_EQ(2, changes->front().GetDict().FindDouble("revision"));
}

// Pair selection uses Widget identities without creating native windows. These
// tests verify which windows may move together, independently of platform APIs.
class XenonHostedWindowPairingTest : public testing::Test {
 protected:
  void SetUp() override { host_ = new XenonElectronWindowHost(); }

  void TearDown() override {
    // These fixture Widgets were never registered with the host as observers.
    host_->windows_.clear();
    delete host_;
    widgets_.clear();
  }

  void AddWindow(int32_t id,
                 int32_t parent = 0,
                 bool paired = false,
                 const std::string& container = "fixture",
                 bool has_widget = true,
                 bool transparent = false) {
    auto& entry = host_->windows_[id];
    if (has_widget) {
      widgets_.push_back(std::make_unique<views::Widget>());
      entry.widget = widgets_.back().get();
    }
    entry.parent_id = parent;
    entry.container_id = container;
    entry.transparent = transparent;
    entry.sync_bounds_with_parent = paired;
  }

  void ExpectPaired(int32_t source, const std::set<int32_t>& expected) {
    const auto actual = host_->GetPairedWindowIds(source);
    EXPECT_EQ(expected, std::set<int32_t>(actual.begin(), actual.end()));
    EXPECT_EQ(expected.size(), actual.size());
  }

  void RemoveWindow(int32_t id) { host_->windows_.erase(id); }

  base::test::TaskEnvironment task_environment_;
  std::vector<std::unique_ptr<views::Widget>> widgets_;
  XenonElectronWindowHost* host_ = nullptr;
};

TEST_F(XenonHostedWindowPairingTest, ExplicitOpaquePairWorksInBothDirections) {
  AddWindow(1);
  AddWindow(2, 1, true);

  ExpectPaired(1, {2});
  ExpectPaired(2, {1});
}

TEST_F(XenonHostedWindowPairingTest, OrdinaryOwnerDoesNotDeclarePairing) {
  AddWindow(1);
  AddWindow(2, 1);
  AddWindow(3, 1, false, "fixture", true, true);

  ExpectPaired(1, {});
  ExpectPaired(2, {});
  ExpectPaired(3, {});
}

TEST_F(XenonHostedWindowPairingTest, PairedChainsAndSiblingsMoveAsOneGroup) {
  AddWindow(1);
  AddWindow(2, 1, true);
  AddWindow(3, 1, true);
  AddWindow(4, 2, true);
  // An ordinary owned window can be the root of a separate paired group.
  AddWindow(5, 1);
  AddWindow(6, 5, true);

  ExpectPaired(1, {2, 3, 4});
  ExpectPaired(2, {1, 3, 4});
  ExpectPaired(3, {1, 2, 4});
  ExpectPaired(4, {1, 2, 3});
  ExpectPaired(5, {6});
  ExpectPaired(6, {5});
}

TEST_F(XenonHostedWindowPairingTest, MissingParentsAndOtherContainersStayApart) {
  AddWindow(1);
  AddWindow(2, 99, true);
  AddWindow(3, 1, true, "other");
  AddWindow(4, 1, true);

  ExpectPaired(1, {4});
  ExpectPaired(2, {});
  ExpectPaired(3, {});
  ExpectPaired(4, {1});
  ExpectPaired(99, {});
}

TEST_F(XenonHostedWindowPairingTest, UnavailableWidgetCannotConnectGroups) {
  AddWindow(1);
  AddWindow(2, 1, true, "fixture", false);
  AddWindow(3, 2, true);

  ExpectPaired(1, {});
  ExpectPaired(2, {});
  ExpectPaired(3, {});
}

TEST_F(XenonHostedWindowPairingTest, RemovedWindowLeavesNoPairingConnection) {
  AddWindow(1);
  AddWindow(2, 1, true);
  AddWindow(3, 2, true);
  ExpectPaired(1, {2, 3});

  RemoveWindow(2);

  ExpectPaired(1, {});
  ExpectPaired(2, {});
  ExpectPaired(3, {});
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

  GURL CommittedPairingURL() {
    return GURL(host_->windows_.at(1).committed_url);
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
       PairingURLChangesOnlyAfterSuccessfulCommit) {
  const GURL first_url("https://fixture.test/first-control");
  auto first = StartNavigation(first_url);
  EXPECT_EQ(GURL("about:blank"), CommittedPairingURL());
  first->ReadyToCommit();
  EXPECT_EQ(GURL("about:blank"), CommittedPairingURL());
  first->Commit();
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(first_url, CommittedPairingURL());

  auto canceled =
      StartNavigation(GURL("https://fixture.test/canceled-control"));
  EXPECT_EQ(first_url, CommittedPairingURL());
  canceled->Fail(net::ERR_ABORTED);
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(first_url, web_contents()->GetLastCommittedURL());
  EXPECT_EQ(first_url, CommittedPairingURL());

  const GURL next_url("https://fixture.test/next-control");
  auto next = StartNavigation(next_url);
  EXPECT_EQ(first_url, CommittedPairingURL());
  next->Commit();
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(next_url, CommittedPairingURL());
}

TEST_F(XenonHostedWindowUserAgentTest, CommittedErrorPageClearsPairingURL) {
  const GURL first_url("https://fixture.test/first-control");
  NavigateAndCommit(first_url);
  ASSERT_EQ(first_url, CommittedPairingURL());

  auto failed =
      StartNavigation(GURL("https://fixture.test/unavailable-control"));
  failed->Fail(net::ERR_TIMED_OUT);
  EXPECT_EQ(first_url, CommittedPairingURL());
  failed->CommitErrorPage();
  base::RunLoop().RunUntilIdle();
  EXPECT_TRUE(CommittedPairingURL().is_empty());

  const GURL recovery_url("https://fixture.test/recovered-control");
  NavigateAndCommit(recovery_url);
  EXPECT_EQ(recovery_url, CommittedPairingURL());
}

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
