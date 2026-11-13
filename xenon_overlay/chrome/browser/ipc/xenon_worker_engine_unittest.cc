// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
#include "xenon_overlay/chrome/browser/ipc/xenon_worker_engine.h"

#include <atomic>
#include <memory>

#include "base/functional/bind.h"
#include "base/synchronization/waitable_event.h"
#include "base/task/single_thread_task_runner.h"
#include "base/test/bind.h"
#include "base/test/run_until.h"
#include "base/test/test_future.h"
#include "base/threading/thread_restrictions.h"
#include "gin/test/v8_test.h"
#include "v8/include/v8.h"

namespace xenon::ipc {
namespace {

struct RetirementProbe {
  std::atomic<bool> release{false};
  std::atomic<bool> destroyed{false};
  std::atomic<int> shutdowns{0};
  bool fail_initialize = false;
  bool pause_factory = false;
  base::WaitableEvent factory_gate;
  scoped_refptr<base::SingleThreadTaskRunner> owner;
  base::OnceClosure created;
  base::OnceClosure first_poll;
};

class RetirementEnvironment final : public WorkerEnvironment {
 public:
  explicit RetirementEnvironment(std::shared_ptr<RetirementProbe> probe)
      : probe_(std::move(probe)) {}
  ~RetirementEnvironment() override { probe_->destroyed = true; }
  bool Initialize(WorkerOptions,
                  std::unique_ptr<WorkerMessage>,
                  std::unique_ptr<v8::Isolate::CreateParams>,
                  WorkerControl*,
                  std::string* error) override {
    if (probe_->fail_initialize) {
      *error = "Expected initialization failure";
      return false;
    }
    return true;
  }
  void Deliver(std::unique_ptr<WorkerMessage>) override {}
  void ParentPortClosed() override {}
  bool HasActiveWork() const override { return true; }
  int BeforeExit() override { return 0; }
  int Shutdown(int code, bool) override {
    ++probe_->shutdowns;
    return code;
  }
  void BeginNativeShutdown() override {}
  bool PollNativeShutdown() override {
    if (probe_->first_poll) {
      probe_->owner->PostTask(FROM_HERE, std::move(probe_->first_poll));
    }
    return probe_->release.load();
  }
  std::string DescribeNativeShutdown() const override { return "test request"; }

 private:
  const std::shared_ptr<RetirementProbe> probe_;
};

class WorkerEngineRetirementTest : public gin::V8Test {
 protected:
  WorkerEngine::EnvironmentFactory Factory(
      std::shared_ptr<RetirementProbe> probe) {
    return base::BindRepeating(
        [](std::shared_ptr<RetirementProbe> probe)
            -> std::unique_ptr<WorkerEnvironment> {
          probe->owner->PostTask(FROM_HERE, std::move(probe->created));
          if (probe->pause_factory) {
            base::ScopedAllowBaseSyncPrimitivesForTesting allow_wait;
            probe->factory_gate.Wait();
          }
          return std::make_unique<RetirementEnvironment>(std::move(probe));
        },
        std::move(probe));
  }
  bool Create(WorkerEngine& engine, WorkerOptions* options) {
    auto* isolate = instance_->isolate();
    v8::HandleScope handles(isolate);
    auto context = context_.Get(isolate);
    v8::Context::Scope scope(context);
    auto data = PreparedWorkerMessage::Serialize(
        context, v8::Undefined(isolate), v8::Undefined(isolate));
    if (!data) {
      return false;
    }
    std::string error;
    return engine.Create(options, std::move(data), isolate, &error);
  }
  void CheckRetirement(bool shutdown_engine, bool fail_initialize,
                       bool terminate_before_initialize) {
    auto probe = std::make_shared<RetirementProbe>();
    probe->owner = base::SingleThreadTaskRunner::GetCurrentDefault();
    probe->fail_initialize = fail_initialize;
    probe->pause_factory = terminate_before_initialize;
    base::test::TestFuture<void> created, first_poll;
    probe->created = created.GetCallback();
    probe->first_poll = first_poll.GetCallback();
    int exits = 0;
    WorkerEngine engine(
        Factory(probe), base::BindLambdaForTesting([&](WorkerEngine::Event event) {
          if (event.type == "exit") {
            ++exits;
            EXPECT_TRUE(probe->destroyed);
            EXPECT_EQ(1, event.payload.GetInt());
          }
        }));
    WorkerOptions options;
    ASSERT_TRUE(Create(engine, &options));
    ASSERT_TRUE(created.Wait());
    if (shutdown_engine) {
      engine.Shutdown();
      engine.Shutdown();
    } else if (!fail_initialize) {
      EXPECT_TRUE(engine.Control(options.id, "terminate"));
    }
    probe->factory_gate.Signal();
    ASSERT_TRUE(first_poll.Wait());
    EXPECT_FALSE(engine.IsDrained());
    EXPECT_FALSE(probe->destroyed);
    EXPECT_EQ(0, exits);
    probe->release = true;
    ASSERT_TRUE(base::test::RunUntil([&] { return engine.IsDrained(); }));
    EXPECT_TRUE(probe->destroyed);
    EXPECT_EQ(1, probe->shutdowns.load());
    EXPECT_EQ(shutdown_engine ? 0 : 1, exits);
  }
};

TEST_F(WorkerEngineRetirementTest, TerminateWaitsForNativeRetirement) {
  CheckRetirement(false, false, false);
}
TEST_F(WorkerEngineRetirementTest, TerminateBeforeInitializeRetainsEnvironment) {
  CheckRetirement(false, false, true);
}
TEST_F(WorkerEngineRetirementTest, FailedInitializeStillRetiresEnvironment) {
  CheckRetirement(false, true, false);
}
TEST_F(WorkerEngineRetirementTest, EngineShutdownWaitsForNativeRetirement) {
  CheckRetirement(true, false, false);
}

}  // namespace
}  // namespace xenon::ipc
