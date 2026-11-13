// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_electron_document_registration.h"

#include "base/functional/bind.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "base/time/time.h"
#include "content/public/test/browser_task_environment.h"
#include "content/public/test/test_renderer_host.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace xenon::ipc {
namespace {
using Reply = base::test::TestFuture<std::string>;

class XenonElectronDocumentRegistrationTest
    : public content::RenderViewHostTestHarness {
 public:
  XenonElectronDocumentRegistrationTest()
      : RenderViewHostTestHarness(
            base::test::TaskEnvironment::TimeSource::MOCK_TIME) {}

 protected:
  void SetUp() override {
    RenderViewHostTestHarness::SetUp();
    NavigateAndCommit(GURL("https://registration.test/first"));
  }
};

TEST_F(XenonElectronDocumentRegistrationTest,
       MissingRegistrationDoesNotWaitForSomeFuturePage) {
  Reply reply;
  WaitForElectronDocumentRegistration(main_rfh(), reply.GetCallback());
  EXPECT_FALSE(reply.IsReady());
  EXPECT_TRUE(reply.Get().empty());
}

TEST_F(XenonElectronDocumentRegistrationTest,
       BrowserQueueAcceptanceDoesNotCountAsUtilityRegistration) {
  auto complete = BeginElectronDocumentRegistration(main_rfh(), "first");
  Reply reply;
  WaitForElectronDocumentRegistration(main_rfh(), reply.GetCallback());
  task_environment()->RunUntilIdle();
  EXPECT_FALSE(reply.IsReady());
  std::move(complete).Run(true);
  EXPECT_EQ(reply.Get(), "first");
}

TEST_F(XenonElectronDocumentRegistrationTest,
       RebindingRetiresOldWaitersAndIgnoresLateAcknowledgements) {
  auto old_complete = BeginElectronDocumentRegistration(main_rfh(), "old");
  Reply old_reply;
  WaitForElectronDocumentRegistration(main_rfh(), old_reply.GetCallback());
  auto current_complete = BeginElectronDocumentRegistration(main_rfh(), "new");
  Reply current_reply;
  WaitForElectronDocumentRegistration(main_rfh(), current_reply.GetCallback());
  std::move(old_complete).Run(true);
  task_environment()->RunUntilIdle();
  EXPECT_TRUE(old_reply.Get().empty());
  EXPECT_FALSE(current_reply.IsReady());
  std::move(current_complete).Run(true);
  EXPECT_EQ(current_reply.Get(), "new");
}

TEST_F(XenonElectronDocumentRegistrationTest,
       RevocationInvalidatesAlreadyQueuedSuccessReplies) {
  auto complete = BeginElectronDocumentRegistration(main_rfh(), "page");
  std::move(complete).Run(true);
  Reply reply;
  WaitForElectronDocumentRegistration(main_rfh(), reply.GetCallback());
  RevokeElectronDocumentRegistration(main_rfh(), "page");
  EXPECT_TRUE(reply.Get().empty());
}

TEST_F(XenonElectronDocumentRegistrationTest,
       RetiredOwnerCannotUseAnAlreadyRegisteredDocument) {
  bool current = true;
  auto complete = BeginElectronDocumentRegistration(
      main_rfh(), "page",
      base::BindRepeating([](bool* current) { return *current; }, &current));
  std::move(complete).Run(true);
  Reply reply;
  WaitForElectronDocumentRegistration(main_rfh(), reply.GetCallback());
  current = false;
  EXPECT_TRUE(reply.Get().empty());
}

TEST_F(XenonElectronDocumentRegistrationTest,
       NavigationCannotTransferOldReadinessToNewDocument) {
  auto complete = BeginElectronDocumentRegistration(main_rfh(), "old");
  Reply old_reply;
  WaitForElectronDocumentRegistration(main_rfh(), old_reply.GetCallback());
  NavigateAndCommit(GURL("https://registration.test/new"));
  std::move(complete).Run(true);
  EXPECT_TRUE(old_reply.Get().empty());
  Reply new_reply;
  WaitForElectronDocumentRegistration(main_rfh(), new_reply.GetCallback());
  EXPECT_TRUE(new_reply.Get().empty());
}

TEST_F(XenonElectronDocumentRegistrationTest,
       TimeoutRejectsWaitersWithoutInventingRegistration) {
  auto complete = BeginElectronDocumentRegistration(main_rfh(), "slow");
  Reply reply;
  WaitForElectronDocumentRegistration(main_rfh(), reply.GetCallback());
  task_environment()->FastForwardBy(base::Seconds(30));
  EXPECT_TRUE(reply.Get().empty());
  // An actual later registration can satisfy a new caller, but cannot change
  // the completed timeout into success.
  std::move(complete).Run(true);
  Reply later;
  WaitForElectronDocumentRegistration(main_rfh(), later.GetCallback());
  EXPECT_EQ(later.Get(), "slow");
}

TEST_F(XenonElectronDocumentRegistrationTest,
       DestroyedPageRejectsPendingWaiter) {
  auto complete = BeginElectronDocumentRegistration(main_rfh(), "closing");
  Reply reply;
  WaitForElectronDocumentRegistration(main_rfh(), reply.GetCallback());
  DeleteContents();
  std::move(complete).Run(true);
  EXPECT_TRUE(reply.Get().empty());
}

}  // namespace
}  // namespace xenon::ipc
