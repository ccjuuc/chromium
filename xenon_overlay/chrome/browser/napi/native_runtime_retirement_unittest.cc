// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/napi/native_runtime_retirement.h"

#include <memory>
#include <string>
#include <vector>

#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/task/single_thread_task_runner.h"
#include "base/test/task_environment.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace xenon {
namespace {

struct RuntimeProbe {
  bool busy = true;
  bool began = false;
  bool destroyed = false;
  int polls = 0;
  int stalled = 0;
  std::vector<std::string> events;
};

class Runtime final : public NativeRuntimeShutdown {
 public:
  explicit Runtime(RuntimeProbe* probe) : probe_(probe) {}
  ~Runtime() override {
    EXPECT_FALSE(probe_->busy);
    probe_->destroyed = true;
    probe_->events.push_back("destroy");
  }
  void BeginNativeShutdown() override {
    EXPECT_FALSE(probe_->began);
    probe_->began = true;
    probe_->events.push_back("begin");
  }
  bool PollNativeShutdown() override {
    ++probe_->polls;
    return !probe_->busy;
  }
  std::string DescribeNativeShutdown() const override {
    return "native completion pending";
  }

 private:
  raw_ptr<RuntimeProbe> probe_;
};

NativeRuntimeRetirementOptions Options(RuntimeProbe* probe) {
  NativeRuntimeRetirementOptions options;
  options.poll_interval = base::Milliseconds(10);
  options.deadline = base::Milliseconds(50);
  options.on_stalled = base::BindOnce(
      [](RuntimeProbe* probe, std::string message) {
        ++probe->stalled;
        EXPECT_EQ("native completion pending", message);
        EXPECT_FALSE(probe->destroyed);
      },
      probe);
  options.on_complete = base::BindOnce(
      [](RuntimeProbe* probe) {
        EXPECT_TRUE(probe->destroyed);
        probe->events.push_back("release-process");
      },
      probe);
  return options;
}

TEST(NativeRuntimeRetirementTest,
     DefersTeardownUntilNativeCallbackStackReturns) {
  base::test::TaskEnvironment tasks{
      base::test::TaskEnvironment::TimeSource::MOCK_TIME};
  RuntimeProbe probe;
  probe.busy = false;
  RetireNativeRuntime(std::make_unique<Runtime>(&probe), Options(&probe));
  EXPECT_FALSE(probe.began);
  EXPECT_FALSE(probe.destroyed);
  tasks.RunUntilIdle();
  EXPECT_TRUE(probe.destroyed);
  EXPECT_EQ((std::vector<std::string>{"begin", "destroy", "release-process"}),
            probe.events);
}

TEST(NativeRuntimeRetirementTest, RetainsWholeRuntimeAcrossOwnerDestruction) {
  base::test::TaskEnvironment tasks{
      base::test::TaskEnvironment::TimeSource::MOCK_TIME};
  RuntimeProbe probe;
  auto runtime = std::make_unique<Runtime>(&probe);
  RetireNativeRuntime(std::move(runtime), Options(&probe));
  tasks.RunUntilIdle();
  EXPECT_FALSE(runtime);
  EXPECT_TRUE(probe.began);
  EXPECT_FALSE(probe.destroyed);
  // Completion gets its normal task turn; retirement never spins a nested
  // loop or blocks the sequence waiting for this task to run.
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(
                     [](RuntimeProbe* probe) {
                       EXPECT_FALSE(probe->destroyed);
                       probe->busy = false;
                       probe->events.push_back("native-complete");
                     },
                     &probe));
  tasks.FastForwardBy(base::Milliseconds(10));
  EXPECT_TRUE(probe.destroyed);
  EXPECT_EQ((std::vector<std::string>{"begin", "native-complete", "destroy",
                                      "release-process"}),
            probe.events);
}

TEST(NativeRuntimeRetirementTest,
     DeadlineReportsOnceWithoutFreeingBusyResources) {
  base::test::TaskEnvironment tasks{
      base::test::TaskEnvironment::TimeSource::MOCK_TIME};
  RuntimeProbe probe;
  RetireNativeRuntime(std::make_unique<Runtime>(&probe), Options(&probe));
  tasks.FastForwardBy(base::Milliseconds(100));
  EXPECT_EQ(1, probe.stalled);
  EXPECT_FALSE(probe.destroyed);
  EXPECT_LE(probe.polls, 11);
  probe.busy = false;
  tasks.FastForwardBy(base::Milliseconds(10));
  EXPECT_TRUE(probe.destroyed);
  EXPECT_EQ(1, probe.stalled);
}

}  // namespace
}  // namespace xenon
