// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_electron_permission_check_remote.h"

#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "base/task/single_thread_task_runner.h"
#include "base/test/bind.h"
#include "base/test/task_environment.h"
#include "base/threading/sequence_bound.h"
#include "base/threading/thread.h"
#include "mojo/core/embedder/embedder.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/bindings/sync_call_restrictions.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace xenon::ipc {
namespace {

class CheckService final : public mojom::ElectronPermissionCheck {
 public:
  CheckService(mojo::PendingReceiver<mojom::ElectronPermissionCheck> receiver,
               mojo::PendingRemote<mojom::ElectronPermissionCheck> reverse)
      : receiver_(this, std::move(receiver)) {
    if (reverse.is_valid()) {
      reverse_.Bind(std::move(reverse));
    }
  }

  void Check(base::Value request, CheckCallback reply) override;
  void ReleaseHeldReply() {
    if (held_reply_) {
      std::move(held_reply_).Run(true, "");
    }
  }

 private:
  mojo::Receiver<mojom::ElectronPermissionCheck> receiver_;
  mojo::Remote<mojom::ElectronPermissionCheck> reverse_;
  CheckCallback held_reply_;
};

void CheckService::Check(base::Value request, CheckCallback reply) {
  const std::string& command = request.GetString();
  if (command == "hold") {
    held_reply_ = std::move(reply);
    return;
  }
  if (command == "disconnect") {
    receiver_.reset();
    return;
  }
  if (command == "error") {
    std::move(reply).Run(false, "ERR_FAILED: handler threw");
    return;
  }
  if (command == "interrupt-policy") {
    std::move(reply).Run(
        mojo::SyncCallRestrictions::AreSyncCallInterruptsEnabled(), "");
    return;
  }
  if (command == "scope-active") {
    std::move(reply).Run(IsElectronPermissionCheckInProgress(), "");
    return;
  }
  if (command == "reverse") {
    mojo::ScopedAllowSyncCallForTesting allow;
    bool granted = false;
    std::string error;
    const bool received =
        reverse_->Check(base::Value("getter"), &granted, &error);
    std::move(reply).Run(
        received && granted,
        received ? error : "ERR_ABORTED: reverse disconnected");
    return;
  }
  std::move(reply).Run(command == "grant", "");
}

class ReverseGetter final : public mojom::ElectronPermissionCheck {
 public:
  ReverseGetter(mojo::PendingReceiver<mojom::ElectronPermissionCheck> receiver,
                base::RepeatingClosure callback)
      : callback_(std::move(callback)), receiver_(this, std::move(receiver)) {}

  void Check(base::Value request, CheckCallback reply) override;

 private:
  base::RepeatingClosure callback_;
  mojo::Receiver<mojom::ElectronPermissionCheck> receiver_;
};

void ReverseGetter::Check(base::Value request, CheckCallback reply) {
  EXPECT_EQ(request.GetString(), "getter");
  callback_.Run();
  std::move(reply).Run(true, "");
}

class XenonElectronPermissionCheckRemoteTest : public testing::Test {
 public:
  static void SetUpTestSuite() { mojo::core::Init(); }

 protected:
  void SetUp() override {
    EXPECT_FALSE(IsElectronPermissionCheckInProgress());
    // ContentMainRunner disables interrupts in the real Browser process.
    // Reproduce that policy instead of relying on Mojo's permissive default.
    mojo::SyncCallRestrictions::DisableSyncCallInterrupts();
    ASSERT_TRUE(service_thread_.StartWithOptions(
        base::Thread::Options(base::MessagePumpType::IO, 0)));
    ASSERT_TRUE(transport_thread_.StartWithOptions(
        base::Thread::Options(base::MessagePumpType::IO, 0)));
  }

  void TearDown() override {
    EXPECT_FALSE(IsElectronPermissionCheckInProgress());
    remote_.reset();
    service_.SynchronouslyResetForTest();
    transport_thread_.Stop();
    service_thread_.Stop();
    environment_.RunUntilIdle();
    mojo::SyncCallRestrictions::EnableSyncCallInterruptsForTesting();
  }

  void Connect(
      base::TimeDelta timeout = base::Seconds(2),
      mojo::PendingRemote<mojom::ElectronPermissionCheck> reverse = {}) {
    remote_.reset();
    service_.SynchronouslyResetForTest();
    mojo::PendingRemote<mojom::ElectronPermissionCheck> pending;
    service_.emplace(service_thread_.task_runner(),
                     pending.InitWithNewPipeAndPassReceiver(),
                     std::move(reverse));
    remote_ = std::make_unique<ElectronPermissionCheckRemote>(
        std::move(pending), transport_thread_.task_runner(), timeout);
  }

  base::expected<bool, std::string> Check(const char* request) {
    mojo::ScopedAllowSyncCallForTesting allow;
    return remote_->Check(base::Value(request));
  }

  base::test::TaskEnvironment environment_{
      base::test::TaskEnvironment::MainThreadType::IO};
  base::Thread service_thread_{"PermissionTestService"};
  base::Thread transport_thread_{"PermissionTestTransport"};
  base::SequenceBound<CheckService> service_;
  std::unique_ptr<ElectronPermissionCheckRemote> remote_;
};

TEST_F(XenonElectronPermissionCheckRemoteTest, ReturnsActualGrantAndDeny) {
  Connect();
  const auto grant = Check("grant");
  ASSERT_TRUE(grant.has_value()) << grant.error_or("");
  EXPECT_TRUE(*grant);
  const auto deny = Check("deny");
  ASSERT_TRUE(deny.has_value()) << deny.error_or("");
  EXPECT_FALSE(*deny);
}

TEST_F(XenonElectronPermissionCheckRemoteTest,
       HandlerFailureDoesNotPoisonPipe) {
  Connect();
  const auto error = Check("error");
  ASSERT_FALSE(error.has_value());
  EXPECT_EQ(error.error(), "ERR_FAILED: handler threw");
  const auto grant = Check("grant");
  ASSERT_TRUE(grant.has_value()) << grant.error_or("");
  EXPECT_TRUE(*grant);
}

TEST_F(XenonElectronPermissionCheckRemoteTest,
       ReverseSynchronousGetterDoesNotDeadlockOrPumpOrdinaryTasks) {
  mojo::PendingRemote<mojom::ElectronPermissionCheck> reverse;
  bool getter_called = false;
  auto ordinary_task_called = std::make_shared<bool>(false);
  ReverseGetter getter(
      reverse.InitWithNewPipeAndPassReceiver(), base::BindLambdaForTesting([&] {
        getter_called = true;
        EXPECT_TRUE(mojo::SyncCallRestrictions::AreSyncCallInterruptsEnabled());
        EXPECT_TRUE(IsElectronPermissionCheckInProgress());
      }));
  Connect(base::Seconds(2), std::move(reverse));
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindLambdaForTesting(
                     [ordinary_task_called] { *ordinary_task_called = true; }));
  const auto grant = Check("reverse");
  ASSERT_TRUE(grant.has_value()) << grant.error_or("");
  EXPECT_TRUE(*grant);
  EXPECT_TRUE(getter_called);
  EXPECT_FALSE(mojo::SyncCallRestrictions::AreSyncCallInterruptsEnabled());
  EXPECT_FALSE(IsElectronPermissionCheckInProgress());
  EXPECT_FALSE(*ordinary_task_called);
  environment_.RunUntilIdle();
  EXPECT_TRUE(*ordinary_task_called);
}

TEST_F(XenonElectronPermissionCheckRemoteTest,
       InterruptExceptionDoesNotEscapeToAnotherSequence) {
  Connect();
  EXPECT_FALSE(mojo::SyncCallRestrictions::AreSyncCallInterruptsEnabled());
  const auto enabled_in_service = Check("interrupt-policy");
  ASSERT_TRUE(enabled_in_service.has_value());
  EXPECT_FALSE(*enabled_in_service);
  const auto active_in_service = Check("scope-active");
  ASSERT_TRUE(active_in_service.has_value());
  EXPECT_FALSE(*active_in_service);
  EXPECT_FALSE(mojo::SyncCallRestrictions::AreSyncCallInterruptsEnabled());
}

TEST_F(XenonElectronPermissionCheckRemoteTest,
       NestedPermissionCheckPreservesOuterScope) {
  base::Thread nested_thread("NestedPermissionTestService");
  ASSERT_TRUE(nested_thread.StartWithOptions(
      base::Thread::Options(base::MessagePumpType::IO, 0)));
  mojo::PendingRemote<mojom::ElectronPermissionCheck> nested_pending;
  base::SequenceBound<CheckService> nested_service(
      nested_thread.task_runner(),
      nested_pending.InitWithNewPipeAndPassReceiver(),
      mojo::PendingRemote<mojom::ElectronPermissionCheck>());
  ElectronPermissionCheckRemote nested_remote(std::move(nested_pending),
                                              transport_thread_.task_runner(),
                                              base::Seconds(2));
  mojo::PendingRemote<mojom::ElectronPermissionCheck> reverse;
  ReverseGetter getter(
      reverse.InitWithNewPipeAndPassReceiver(), base::BindLambdaForTesting([&] {
        EXPECT_TRUE(IsElectronPermissionCheckInProgress());
        const auto granted = nested_remote.Check(base::Value("grant"));
        EXPECT_TRUE(granted.has_value() && *granted);
        EXPECT_TRUE(IsElectronPermissionCheckInProgress());
      }));
  Connect(base::Seconds(2), std::move(reverse));
  const auto granted = Check("reverse");
  EXPECT_TRUE(granted.has_value() && *granted);
  EXPECT_FALSE(IsElectronPermissionCheckInProgress());
  nested_service.SynchronouslyResetForTest();
}

TEST_F(XenonElectronPermissionCheckRemoteTest, DisconnectWakesBlockedCaller) {
  Connect();
  const auto started = base::TimeTicks::Now();
  const auto result = Check("disconnect");
  EXPECT_LT(base::TimeTicks::Now() - started, base::Seconds(2));
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), "ERR_ABORTED: permission check disconnected");
}

TEST_F(XenonElectronPermissionCheckRemoteTest,
       DeadlineWakesCallerWithoutPumpingOrdinaryTasks) {
  Connect(base::Milliseconds(80));
  auto ordinary_task_called = std::make_shared<bool>(false);
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindLambdaForTesting(
                     [ordinary_task_called] { *ordinary_task_called = true; }));
  const auto started = base::TimeTicks::Now();
  const auto result = Check("hold");
  EXPECT_GE(base::TimeTicks::Now() - started, base::Milliseconds(70));
  EXPECT_LT(base::TimeTicks::Now() - started, base::Seconds(2));
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), "ERR_TIMED_OUT: permission check deadline expired");
  EXPECT_FALSE(*ordinary_task_called);
  service_.AsyncCall(&CheckService::ReleaseHeldReply);
  service_thread_.FlushForTesting();
  const auto disconnected = Check("grant");
  ASSERT_FALSE(disconnected.has_value());
  EXPECT_EQ(disconnected.error(), "ERR_ABORTED: permission check disconnected");
  Connect();
  const auto fresh = Check("deny");
  ASSERT_TRUE(fresh.has_value()) << fresh.error_or("");
  EXPECT_FALSE(*fresh);
  environment_.RunUntilIdle();
  EXPECT_TRUE(*ordinary_task_called);
}

TEST_F(XenonElectronPermissionCheckRemoteTest,
       ReverseGetterCanDeleteOwnerAndCannotGrantAfterRetirement) {
  mojo::PendingRemote<mojom::ElectronPermissionCheck> reverse;
  ReverseGetter getter(reverse.InitWithNewPipeAndPassReceiver(),
                       base::BindLambdaForTesting([&] { remote_.reset(); }));
  Connect(base::Seconds(2), std::move(reverse));
  const auto result = Check("reverse");
  EXPECT_FALSE(remote_);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), "ERR_ABORTED: permission owner was retired");
}

TEST_F(XenonElectronPermissionCheckRemoteTest,
       ClosedAndUnboundRemotesFailSafely) {
  Connect();
  remote_->Close();
  auto result = Check("grant");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), "ERR_ABORTED: permission transport is closed");
  remote_ = std::make_unique<ElectronPermissionCheckRemote>(
      mojo::PendingRemote<mojom::ElectronPermissionCheck>(),
      transport_thread_.task_runner(), base::Seconds(2));
  result = Check("grant");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), "ERR_ABORTED: permission transport is closed");
}

}  // namespace
}  // namespace xenon::ipc
