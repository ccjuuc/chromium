// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_electron_power_save_blocker.h"

#include <memory>
#include <vector>

#include "base/functional/bind.h"
#include "base/test/task_environment.h"
#include "build/build_config.h"
#include "build/config/linux/dbus/buildflags.h"
#include "mojo/core/embedder/embedder.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace xenon::ipc {
namespace {

using WakeLockType = device::mojom::WakeLockType;

class FakeWakeLock final : public device::mojom::WakeLock {
 public:
  FakeWakeLock(WakeLockType initial_type,
               mojo::PendingReceiver<device::mojom::WakeLock> receiver)
      : type(initial_type), receiver(this, std::move(receiver)) {
    this->receiver.set_disconnect_handler(base::BindOnce(
        [](FakeWakeLock* self) { self->connected = self->active = false; },
        base::Unretained(this)));
  }
  void RequestWakeLock() override { active = true; }
  void CancelWakeLock() override { active = false; }
  void AddClient(mojo::PendingReceiver<device::mojom::WakeLock>) override {
    ADD_FAILURE() << "The broker must not share a lock across owners";
  }
  void ChangeType(WakeLockType, ChangeTypeCallback callback) override {
    ADD_FAILURE() << "Context-free locks must retain their creation type";
    std::move(callback).Run(false);
  }
  void HasWakeLockForTests(HasWakeLockForTestsCallback callback) override {
    std::move(callback).Run(active);
  }
  WakeLockType type;
  bool active = false;
  bool connected = true;
  mojo::Receiver<device::mojom::WakeLock> receiver;
};

class XenonElectronPowerSaveBlockerTest : public testing::Test {
 public:
  static void SetUpTestSuite() { mojo::core::Init(); }

 protected:
  XenonElectronPowerSaveBlockerTest()
      : blocker_(base::BindRepeating(
            [](XenonElectronPowerSaveBlockerTest* self,
               WakeLockType type,
               mojo::PendingReceiver<device::mojom::WakeLock> receiver) {
              self->locks_.push_back(
                  std::make_unique<FakeWakeLock>(type, std::move(receiver)));
            },
            base::Unretained(this))) {}

  int Start(const std::string& owner, const std::string& type) {
    auto result = blocker_.Call(
        owner, base::Value(
                   base::DictValue().Set("method", "start").Set("type", type)));
    EXPECT_TRUE(result.has_value()) << result.error_or("");
    environment_.RunUntilIdle();
    return result.has_value() ? result->GetInt() : -1;
  }
  bool CallId(const std::string& owner, const char* method, int id) {
    auto result = blocker_.Call(
        owner,
        base::Value(base::DictValue().Set("method", method).Set("id", id)));
    EXPECT_TRUE(result.has_value()) << result.error_or("");
    environment_.RunUntilIdle();
    return result.has_value() && result->GetBool();
  }

  base::test::TaskEnvironment environment_;
  std::vector<std::unique_ptr<FakeWakeLock>> locks_;
  ElectronPowerSaveBlocker blocker_;
};

#if BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC) || BUILDFLAG(IS_CHROMEOS) || \
    (BUILDFLAG(IS_LINUX) && BUILDFLAG(USE_DBUS))
TEST_F(XenonElectronPowerSaveBlockerTest,
       AggregatesPrecedenceAndRestoresLowerLock) {
  const int app = Start("app:1", "prevent-app-suspension");
  const int display = Start("app:1", "prevent-display-sleep");
  ASSERT_EQ(locks_.size(), 2u);
  EXPECT_FALSE(locks_[0]->active);
  EXPECT_FALSE(locks_[0]->connected);
  EXPECT_TRUE(locks_[1]->active);
  EXPECT_EQ(locks_[1]->type, WakeLockType::kPreventDisplaySleep);
  EXPECT_TRUE(CallId("app:1", "stop", display));
  ASSERT_EQ(locks_.size(), 3u);
  EXPECT_FALSE(locks_[1]->active);
  EXPECT_FALSE(locks_[1]->connected);
  EXPECT_EQ(locks_[2]->type, WakeLockType::kPreventAppSuspension);
  EXPECT_TRUE(locks_[2]->active);
  EXPECT_TRUE(CallId("app:1", "isStarted", app));
  EXPECT_TRUE(CallId("app:1", "stop", app));
  EXPECT_FALSE(locks_[2]->active);
  EXPECT_FALSE(locks_[2]->connected);
  EXPECT_FALSE(CallId("app:1", "stop", app));
}

TEST_F(XenonElectronPowerSaveBlockerTest,
       ExitAfterUpgradeClosesAllEndpointsWithoutChangingTheirTypes) {
  Start("app:1", "prevent-app-suspension");
  Start("app:1", "prevent-display-sleep");
  ASSERT_EQ(locks_.size(), 2u);
  blocker_.ReleaseOwner("app:1");
  environment_.RunUntilIdle();
  EXPECT_EQ(locks_[0]->type, WakeLockType::kPreventAppSuspension);
  EXPECT_EQ(locks_[1]->type, WakeLockType::kPreventDisplaySleep);
  for (const auto& lock : locks_) {
    EXPECT_FALSE(lock->active);
    EXPECT_FALSE(lock->connected);
  }
}

TEST_F(XenonElectronPowerSaveBlockerTest,
       OwnerAndGenerationCannotStopAnotherLock) {
  const int first = Start("app:1", "prevent-display-sleep");
  const int second = Start("other:1", "prevent-app-suspension");
  EXPECT_FALSE(CallId("app:2", "isStarted", first));
  EXPECT_FALSE(CallId("other:1", "stop", first));
  blocker_.ReleaseOwner("app:1");
  environment_.RunUntilIdle();
  EXPECT_FALSE(locks_[0]->active);
  EXPECT_TRUE(locks_[1]->active);
  EXPECT_TRUE(CallId("other:1", "isStarted", second));
  const int restarted = Start("app:2", "prevent-app-suspension");
  EXPECT_NE(first, restarted);
  EXPECT_FALSE(CallId("app:2", "isStarted", first));
}

TEST_F(XenonElectronPowerSaveBlockerTest, ServiceDisconnectRetiresIds) {
  const int id = Start("app:1", "prevent-app-suspension");
  locks_[0]->receiver.reset();
  environment_.RunUntilIdle();
  EXPECT_FALSE(CallId("app:1", "isStarted", id));
  const int next = Start("app:1", "prevent-display-sleep");
  EXPECT_NE(id, next);
  ASSERT_EQ(locks_.size(), 2u);
  EXPECT_TRUE(locks_[1]->active);
}

TEST_F(XenonElectronPowerSaveBlockerTest, DestructorClosesNativeEndpoint) {
  auto transient =
      std::make_unique<ElectronPowerSaveBlocker>(base::BindRepeating(
          [](std::vector<std::unique_ptr<FakeWakeLock>>* locks,
             WakeLockType type,
             mojo::PendingReceiver<device::mojom::WakeLock> receiver) {
            locks->push_back(
                std::make_unique<FakeWakeLock>(type, std::move(receiver)));
          },
          &locks_));
  ASSERT_TRUE(transient->Call(
      "app:1", base::Value(base::DictValue()
                               .Set("method", "start")
                               .Set("type", "prevent-app-suspension"))));
  environment_.RunUntilIdle();
  ASSERT_TRUE(locks_[0]->active);
  transient.reset();
  environment_.RunUntilIdle();
  EXPECT_FALSE(locks_[0]->active);
  EXPECT_FALSE(locks_[0]->connected);
}
#endif

TEST_F(XenonElectronPowerSaveBlockerTest, InvalidRequestsDoNotAcquireLocks) {
  EXPECT_FALSE(
      blocker_.Call("", base::Value(base::DictValue().Set("method", "start"))));
  EXPECT_FALSE(blocker_.Call("app:1", base::Value()));
  EXPECT_FALSE(blocker_.Call(
      "app:1",
      base::Value(
          base::DictValue().Set("method", "start").Set("type", "invalid"))));
  EXPECT_FALSE(blocker_.Call(
      "app:1",
      base::Value(base::DictValue().Set("method", "stop").Set("id", "0"))));
  EXPECT_FALSE(blocker_.Call(
      "app:1", base::Value(base::DictValue().Set("method", "invented"))));
  EXPECT_TRUE(locks_.empty());
}

}  // namespace
}  // namespace xenon::ipc
