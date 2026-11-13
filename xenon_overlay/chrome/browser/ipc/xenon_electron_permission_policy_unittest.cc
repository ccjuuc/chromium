// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_electron_permission_policy.h"

#include <optional>
#include <vector>

#include "base/functional/bind.h"
#include "base/test/bind.h"
#include "base/test/task_environment.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace xenon::ipc {
namespace {

class XenonElectronPermissionPolicyTest : public testing::Test {
 protected:
  void SetUp() override {
    policy_ = base::MakeRefCounted<ElectronPermissionPolicy>(
        base::BindLambdaForTesting([&](base::Value request) {
          checks_.push_back(request.Clone());
          if (on_check_) {
            on_check_.Run();
          }
          return base::expected<bool, std::string>(decision_);
        }),
        base::BindLambdaForTesting([&](base::Value request) {
          requests_.push_back(std::move(request));
        }),
        base::BindLambdaForTesting([&] { ++changes_; }), base::Seconds(30));
  }
  void TearDown() override { policy_->Close(); }

  ElectronPermissionPolicy::ValidCallback Valid() {
    return base::BindLambdaForTesting([&] { return valid_; });
  }
  void Register(const std::string& partition,
                const char* kind,
                int id,
                bool enabled = true) {
    const auto result = policy_->SetHandler(partition, kind, id, enabled);
    ASSERT_TRUE(result.has_value()) << result.error_or("");
  }
  base::expected<bool, std::string> Check(const std::string& partition = "") {
    return policy_->Check(partition,
                          base::DictValue().Set("permission", "notifications"),
                          Valid(), true);
  }
  void Request(const std::string& partition = "") {
    policy_->Request(partition,
                     base::DictValue().Set("permission", "notifications"),
                     Valid(), base::BindLambdaForTesting([&](bool allowed) {
                       decisions_.push_back(allowed);
                     }));
  }
  int LastRequestId() const {
    return *requests_.back().GetDict().FindInt("requestId");
  }

  base::test::TaskEnvironment environment_{
      base::test::TaskEnvironment::TimeSource::MOCK_TIME};
  bool valid_ = true;
  bool decision_ = true;
  int changes_ = 0;
  base::RepeatingClosure on_check_;
  std::vector<base::Value> checks_;
  std::vector<base::Value> requests_;
  std::vector<bool> decisions_;
  scoped_refptr<ElectronPermissionPolicy> policy_;
};

TEST_F(XenonElectronPermissionPolicyTest, DefaultsApplyOnlyToLiveRequests) {
  EXPECT_EQ(Check(), true);
  EXPECT_EQ(policy_->Check("", {}, Valid(), false), false);
  Request();
  ASSERT_EQ(decisions_.size(), 1u);
  EXPECT_TRUE(decisions_[0]);
  valid_ = false;
  EXPECT_EQ(Check(), false);
  Request();
  EXPECT_FALSE(decisions_.back());
  EXPECT_TRUE(requests_.empty());
  EXPECT_TRUE(checks_.empty());
}

TEST_F(XenonElectronPermissionPolicyTest,
       EveryCheckUsesCurrentClosureAndInvalidatesReentrantRegistration) {
  Register("", "check", 1);
  EXPECT_EQ(Check(), true);
  decision_ = false;
  EXPECT_EQ(Check(), false);
  EXPECT_EQ(checks_.size(), 2u);
  decision_ = true;
  on_check_ = base::BindLambdaForTesting([&] { Register("", "check", 2); });
  EXPECT_EQ(Check(), false);
  on_check_.Reset();
  EXPECT_EQ(Check(), true);
}

TEST_F(XenonElectronPermissionPolicyTest,
       ResponseRequiresMatchingPartitionRegistrationAndRequestExactlyOnce) {
  Register("persist:a", "request", 4);
  Request("persist:a");
  ASSERT_EQ(requests_.size(), 1u);
  const int id = LastRequestId();
  EXPECT_FALSE(policy_->Respond("persist:b", 4, id, true));
  EXPECT_FALSE(policy_->Respond("persist:a", 3, id, true));
  EXPECT_TRUE(decisions_.empty());
  EXPECT_TRUE(policy_->Respond("persist:a", 4, id, true));
  EXPECT_FALSE(policy_->Respond("persist:a", 4, id, false));
  ASSERT_EQ(decisions_.size(), 1u);
  EXPECT_TRUE(decisions_[0]);
}

TEST_F(XenonElectronPermissionPolicyTest,
       ReplacingHandlerDeniesPendingAndLateResponseCannotGrantNextRequest) {
  Register("", "request", 1);
  Request();
  const int previous = LastRequestId();
  Register("", "request", 2);
  ASSERT_EQ(decisions_.size(), 1u);
  EXPECT_FALSE(decisions_[0]);
  Request();
  const int current = LastRequestId();
  EXPECT_NE(previous, current);
  EXPECT_FALSE(policy_->Respond("", 1, previous, true));
  EXPECT_FALSE(policy_->Respond("", 1, current, true));
  EXPECT_TRUE(policy_->Respond("", 2, current, false));
  ASSERT_EQ(decisions_.size(), 2u);
  EXPECT_FALSE(decisions_[1]);
}

TEST_F(XenonElectronPermissionPolicyTest, DeadlineAndRetiredDocumentDeny) {
  Register("", "request", 1);
  Request();
  const int expired = LastRequestId();
  environment_.FastForwardBy(base::Seconds(30));
  ASSERT_EQ(decisions_.size(), 1u);
  EXPECT_FALSE(decisions_[0]);
  EXPECT_FALSE(policy_->Respond("", 1, expired, true));
  Request();
  valid_ = false;
  EXPECT_TRUE(policy_->Respond("", 1, LastRequestId(), true));
  ASSERT_EQ(decisions_.size(), 2u);
  EXPECT_FALSE(decisions_[1]);
}

TEST_F(XenonElectronPermissionPolicyTest,
       OwnerExitDrainsPendingAndDeniesDefaults) {
  Register("", "request", 1);
  Request();
  Request();
  policy_->Close();
  policy_->Close();
  ASSERT_EQ(decisions_.size(), 2u);
  EXPECT_FALSE(decisions_[0]);
  EXPECT_FALSE(decisions_[1]);
  EXPECT_EQ(Check(), false);
  Request();
  EXPECT_FALSE(decisions_.back());
  EXPECT_FALSE(policy_->SetHandler("", "request", 2, true).has_value());
}

TEST_F(XenonElectronPermissionPolicyTest,
       DisplaySelectionHasSeparateRegistrationAndNeverDefaultsToSuccess) {
  std::optional<base::DictValue> selection;
  auto request = [&] {
    selection.reset();
    policy_->RequestDisplay(
        "", {}, Valid(), base::BindLambdaForTesting([&](base::DictValue value) {
          selection = std::move(value);
        }));
  };
  request();
  ASSERT_TRUE(selection.has_value());
  EXPECT_TRUE(selection->empty());
  Register("", "display", 1);
  request();
  EXPECT_FALSE(selection.has_value());
  const int id = LastRequestId();
  EXPECT_FALSE(policy_->Respond("", 1, id, true));
  EXPECT_TRUE(policy_->RespondDisplay(
      "", 1, id,
      base::DictValue().Set("video",
                            base::DictValue().Set("id", "window:1:0"))));
  ASSERT_TRUE(selection.has_value());
  EXPECT_EQ(*selection->FindDict("video")->FindString("id"), "window:1:0");
}

}  // namespace
}  // namespace xenon::ipc
