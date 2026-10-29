// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/remote/remote_input_router.h"

#include <string>
#include <utility>
#include <vector>

#include "base/containers/span.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/memory/ref_counted.h"
#include "base/run_loop.h"
#include "base/test/bind.h"
#include "content/public/browser/devtools_agent_host.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/mock_devtools_agent_host.h"
#include "content/public/test/test_renderer_host.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace xenon {

class RemoteInputRouterTestPeer {
 public:
  static void InstallHost(RemoteInputRouter& router,
                          content::DevToolsAgentHost* host) {
    router.DetachPage();
    router.agent_host_ = host;
    ASSERT_TRUE(host->AttachClient(&router));
  }
};

namespace {

class RecordingAgentHost : public content::MockDevToolsAgentHost {
 public:
  void DispatchProtocolMessage(content::DevToolsAgentHostClient* client,
                               base::span<const uint8_t> message) override {
    EXPECT_EQ(client, client_);
    auto command = base::JSONReader::ReadDict(base::as_string_view(message),
                                              base::JSON_PARSE_RFC);
    ASSERT_TRUE(command);
    commands.push_back(std::move(*command));
  }

  void Respond(int id, base::DictValue result = {}) {
    ASSERT_TRUE(client_);
    base::DictValue response;
    response.Set("id", id);
    response.Set("result", std::move(result));
    const auto json = base::WriteJson(response);
    ASSERT_TRUE(json);
    client_->DispatchProtocolMessage(this, base::as_byte_span(*json));
  }

  void RespondWithError(int id) {
    ASSERT_TRUE(client_);
    const auto json = base::WriteJson(base::DictValue().Set("id", id).Set(
        "error", base::DictValue()
                     .Set("code", -32000)
                     .Set("message", "Target unavailable")));
    ASSERT_TRUE(json);
    client_->DispatchProtocolMessage(this, base::as_byte_span(*json));
  }

  void CloseAgent() {
    ASSERT_TRUE(client_);
    auto* client = client_.get();
    client_ = nullptr;
    client->AgentHostClosed(this);
  }

  int LastId() const { return *commands.back().FindInt("id"); }

  std::vector<base::DictValue> commands;

 private:
  ~RecordingAgentHost() override = default;
};

base::DictValue KeyEvent(std::string code,
                         std::string key,
                         bool down,
                         int modifiers = 0) {
  return base::DictValue()
      .Set("generation", 1)
      .Set("type", "key")
      .Set("code", std::move(code))
      .Set("key", std::move(key))
      .Set("down", down)
      .Set("modifiers", modifiers);
}

base::DictValue PointerEvent(std::string type, double x, double y) {
  return base::DictValue()
      .Set("generation", 1)
      .Set("type", std::move(type))
      .Set("x", x)
      .Set("y", y);
}

base::DictValue LayoutMetrics(double width, double height) {
  return base::DictValue().Set(
      "cssVisualViewport",
      base::DictValue().Set("clientWidth", width).Set("clientHeight", height));
}

class RemoteInputRouterTest : public content::RenderViewHostTestHarness {
 public:
  void SetUp() override {
    content::RenderViewHostTestHarness::SetUp();
    NavigateAndCommit(GURL("https://example.test/"));
    auto* frame = web_contents()->GetPrimaryMainFrame();
    target_ = content::DesktopMediaID(
        content::DesktopMediaID::TYPE_WEB_CONTENTS, 0,
        content::WebContentsMediaCaptureId(
            frame->GetProcess()->GetDeprecatedID(), frame->GetRoutingID()));
    router_.SetTarget(target_, 1);
    host_ = base::MakeRefCounted<RecordingAgentHost>();
    RemoteInputRouterTestPeer::InstallHost(router_, host_.get());
    ASSERT_TRUE(router_.SetEnabled(true));
  }

  void TearDown() override {
    router_.Reset();
    host_.reset();
    content::RenderViewHostTestHarness::TearDown();
  }

 protected:
  RemoteInputRouter router_;
  content::DesktopMediaID target_;
  scoped_refptr<RecordingAgentHost> host_;
};

TEST_F(RemoteInputRouterTest, RejectsStaleOrMalformedInputBeforeDispatch) {
  auto event = KeyEvent("KeyA", "a", true);
  event.Set("generation", 2);
  EXPECT_FALSE(router_.HandleInput(event));
  event.Set("generation", 1);
  event.Set("type", "Runtime.evaluate");
  EXPECT_FALSE(router_.HandleInput(event));
  EXPECT_FALSE(router_.HandleInput(PointerEvent("move", -0.1, 0.5)));
  auto invalid_coordinate = PointerEvent("move", 0.5, 0.5);
  invalid_coordinate.Set("x", "NaN");
  EXPECT_FALSE(router_.HandleInput(invalid_coordinate));
  EXPECT_FALSE(router_.HandleInput(KeyEvent("NotAKey", "a", true)));
  EXPECT_FALSE(router_.HandleInput(base::DictValue()
                                       .Set("generation", 1)
                                       .Set("type", "text")
                                       .Set("text", std::string(4097, 'x'))));
  EXPECT_TRUE(host_->commands.empty());
}

TEST_F(RemoteInputRouterTest, UsesCssViewportAndPreservesClickBeforeTyping) {
  auto click = PointerEvent("button", 0.25, 0.5);
  click.Set("button", 0);
  click.Set("down", true);
  ASSERT_TRUE(router_.HandleInput(click));
  ASSERT_TRUE(router_.HandleInput(KeyEvent("KeyA", "a", true)));
  ASSERT_EQ(host_->commands.size(), 1u);
  EXPECT_EQ(*host_->commands.back().FindString("method"),
            "Page.getLayoutMetrics");

  host_->Respond(host_->LastId(), LayoutMetrics(801, 601));
  ASSERT_EQ(host_->commands.size(), 2u);
  const auto& mouse = host_->commands.back();
  EXPECT_EQ(*mouse.FindString("method"), "Input.dispatchMouseEvent");
  EXPECT_EQ(mouse.FindDoubleByDottedPath("params.x"), 200);
  EXPECT_EQ(mouse.FindDoubleByDottedPath("params.y"), 300);
  EXPECT_EQ(*mouse.FindStringByDottedPath("params.type"), "mousePressed");

  host_->Respond(host_->LastId());
  ASSERT_EQ(host_->commands.size(), 3u);
  EXPECT_EQ(*host_->commands.back().FindString("method"),
            "Input.dispatchKeyEvent");
  EXPECT_EQ(*host_->commands.back().FindStringByDottedPath("params.text"), "a");
  host_->Respond(host_->LastId());
}

TEST_F(RemoteInputRouterTest, ReleaseCancelsQueuedClickAndIgnoresLateMetrics) {
  ASSERT_TRUE(router_.HandleInput(KeyEvent("ControlLeft", "Control", true, 2)));
  host_->Respond(host_->LastId());
  auto click = PointerEvent("button", 0.5, 0.5);
  click.Set("button", 0);
  click.Set("down", true);
  ASSERT_TRUE(router_.HandleInput(click));
  const int metrics_id = host_->LastId();
  ASSERT_TRUE(router_.HandleInput(
      base::DictValue().Set("generation", 1).Set("type", "release")));
  ASSERT_EQ(host_->commands.size(), 3u);
  EXPECT_EQ(*host_->commands.back().FindStringByDottedPath("params.type"),
            "keyUp");
  EXPECT_EQ(*host_->commands.back().FindStringByDottedPath("params.code"),
            "ControlLeft");
  host_->Respond(metrics_id, LayoutMetrics(800, 600));
  EXPECT_EQ(host_->commands.size(), 3u);
  router_.ReleaseAll();
  EXPECT_EQ(host_->commands.size(), 3u);
}

TEST_F(RemoteInputRouterTest,
       ChangingTargetReleasesHeldInputAndRevokesControl) {
  ASSERT_TRUE(router_.HandleInput(KeyEvent("ShiftLeft", "Shift", true, 8)));
  host_->Respond(host_->LastId());
  auto click = PointerEvent("button", 0.5, 0.5);
  click.Set("button", 0);
  click.Set("down", true);
  ASSERT_TRUE(router_.HandleInput(click));
  host_->Respond(host_->LastId(), LayoutMetrics(800, 600));
  host_->Respond(host_->LastId());
  const size_t before = host_->commands.size();

  router_.SetTarget(target_, 2);
  ASSERT_EQ(host_->commands.size(), before + 2);
  EXPECT_EQ(*host_->commands[before].FindStringByDottedPath("params.type"),
            "keyUp");
  EXPECT_EQ(*host_->commands[before + 1].FindStringByDottedPath("params.type"),
            "mouseReleased");
  EXPECT_FALSE(router_.HandleInput(KeyEvent("KeyA", "a", true)));
  auto new_event = KeyEvent("KeyA", "a", true);
  new_event.Set("generation", 2);
  EXPECT_FALSE(router_.HandleInput(new_event));
}

TEST_F(RemoteInputRouterTest, InvalidMetricsDisableControlInsteadOfGuessing) {
  ASSERT_TRUE(router_.HandleInput(PointerEvent("move", 0.25, 0.5)));
  host_->Respond(host_->LastId(), LayoutMetrics(0, 600));
  EXPECT_FALSE(router_.HandleInput(KeyEvent("KeyA", "a", true)));
  ASSERT_EQ(host_->commands.size(), 1u);
}

TEST_F(RemoteInputRouterTest, PageModeDoesNotDispatchBrowserShortcuts) {
  EXPECT_TRUE(router_.HandleInput(KeyEvent("KeyL", "l", true, 2)));
  EXPECT_TRUE(router_.HandleInput(KeyEvent("KeyW", "w", true, 2)));
  EXPECT_TRUE(router_.HandleInput(KeyEvent("MetaLeft", "Meta", true, 4)));
  EXPECT_TRUE(host_->commands.empty());
}

TEST_F(RemoteInputRouterTest, StopClearsQueuedInputAndRequiresNewTarget) {
  ASSERT_TRUE(router_.HandleInput(PointerEvent("move", 0.25, 0.5)));
  router_.Reset();
  EXPECT_FALSE(router_.SetEnabled(true));
  EXPECT_FALSE(router_.HandleInput(KeyEvent("KeyA", "a", true)));
  EXPECT_FALSE(host_->IsAttached());
}

TEST_F(RemoteInputRouterTest, KeyUpWithChangedModifiersDoesNotLeaveKeyHeld) {
  ASSERT_TRUE(router_.HandleInput(KeyEvent("KeyL", "l", true)));
  host_->Respond(host_->LastId());
  ASSERT_TRUE(router_.HandleInput(KeyEvent("KeyL", "l", false, 2)));
  ASSERT_EQ(host_->commands.size(), 2u);
  EXPECT_EQ(*host_->commands.back().FindStringByDottedPath("params.type"),
            "keyUp");
  host_->Respond(host_->LastId());
  router_.ReleaseAll();
  EXPECT_EQ(host_->commands.size(), 2u);
}

TEST_F(RemoteInputRouterTest, UnknownPageTargetNeverEnablesControl) {
  router_.SetTarget(
      content::DesktopMediaID(content::DesktopMediaID::TYPE_WEB_CONTENTS, 0,
                              content::WebContentsMediaCaptureId(99999, 99999)),
      2);
  EXPECT_FALSE(router_.SetEnabled(true));
  EXPECT_FALSE(host_->IsAttached());
}

TEST_F(RemoteInputRouterTest, ProtocolFailureNotifiesAfterReleasingHeldInput) {
  int notifications = 0;
  router_.SetControlRevokedCallback(base::BindLambdaForTesting([&] {
    ++notifications;
    EXPECT_EQ(*host_->commands.back().FindStringByDottedPath("params.type"),
              "keyUp");
  }));
  ASSERT_TRUE(router_.HandleInput(KeyEvent("ShiftLeft", "Shift", true, 8)));
  const int failed_command = host_->LastId();
  host_->RespondWithError(failed_command);
  EXPECT_EQ(notifications, 0);
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(notifications, 1);
  EXPECT_FALSE(router_.HandleInput(KeyEvent("KeyA", "a", true)));
  host_->RespondWithError(failed_command);
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(notifications, 1);
}

TEST_F(RemoteInputRouterTest, ClosedAgentNotifiesWithoutFurtherInput) {
  int notifications = 0;
  router_.SetControlRevokedCallback(
      base::BindLambdaForTesting([&] { ++notifications; }));
  host_->CloseAgent();
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(notifications, 1);
  EXPECT_FALSE(router_.HandleInput(KeyEvent("KeyA", "a", true)));
}

TEST_F(RemoteInputRouterTest, RestrictedNavigationRevokesAndNotifies) {
  int notifications = 0;
  router_.SetControlRevokedCallback(
      base::BindLambdaForTesting([&] { ++notifications; }));
  NavigateAndCommit(GURL("file:///remote-input-test.html"));
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(notifications, 1);
  EXPECT_FALSE(host_->IsAttached());
  EXPECT_FALSE(router_.HandleInput(KeyEvent("KeyA", "a", true)));
}

TEST_F(RemoteInputRouterTest, DestroyedPageRevokesAndNotifies) {
  int notifications = 0;
  router_.SetControlRevokedCallback(
      base::BindLambdaForTesting([&] { ++notifications; }));
  DeleteContents();
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(notifications, 1);
  EXPECT_FALSE(router_.SetEnabled(true));
}

TEST_F(RemoteInputRouterTest, ExplicitStateChangesCancelPendingNotification) {
  int notifications = 0;
  router_.SetControlRevokedCallback(
      base::BindLambdaForTesting([&] { ++notifications; }));
  ASSERT_TRUE(router_.HandleInput(PointerEvent("move", 0.5, 0.5)));
  host_->RespondWithError(host_->LastId());
  ASSERT_TRUE(router_.SetEnabled(true));
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(notifications, 0);

  ASSERT_TRUE(router_.HandleInput(PointerEvent("move", 0.5, 0.5)));
  host_->RespondWithError(host_->LastId());
  router_.Reset();
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(notifications, 0);
}

TEST_F(RemoteInputRouterTest, NotificationMayResetTheRouter) {
  int notifications = 0;
  router_.SetControlRevokedCallback(base::BindLambdaForTesting([&] {
    ++notifications;
    router_.Reset();
  }));
  ASSERT_TRUE(router_.HandleInput(PointerEvent("move", 0.5, 0.5)));
  host_->RespondWithError(host_->LastId());
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(notifications, 1);
  EXPECT_FALSE(host_->IsAttached());
  EXPECT_FALSE(router_.SetEnabled(true));
}

TEST_F(RemoteInputRouterTest, PendingNotificationCannotOutliveRouter) {
  int notifications = 0;
  auto router = std::make_unique<RemoteInputRouter>();
  router->SetTarget(target_, 1);
  auto host = base::MakeRefCounted<RecordingAgentHost>();
  RemoteInputRouterTestPeer::InstallHost(*router, host.get());
  ASSERT_TRUE(router->SetEnabled(true));
  router->SetControlRevokedCallback(
      base::BindLambdaForTesting([&] { ++notifications; }));
  host->CloseAgent();
  router.reset();
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(notifications, 0);
}

}  // namespace
}  // namespace xenon
