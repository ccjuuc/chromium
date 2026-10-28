// Copyright 2023 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "gin/public/isolate_holder.h"

#include <functional>
#include <initializer_list>
#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "base/memory/raw_ref.h"
#include "base/memory/scoped_refptr.h"
#include "base/task/single_thread_task_runner.h"
#include "base/test/test_mock_time_task_runner.h"
#include "base/threading/thread.h"
#include "base/time/time.h"
#include "gin/converter.h"
#include "gin/function_template.h"
#include "gin/per_isolate_data.h"
#include "gin/public/v8_idle_task_runner.h"
#include "gin/test/v8_test.h"
#include "gin/v8_foreground_task_runner_base.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-locker.h"
#include "v8/include/v8-platform.h"
#include "v8/include/v8-template.h"

namespace gin {

// Exercises isolate and task lifetimes in kUseLocker access mode.
class UseLockerIsolateHolderTest : public V8Test {
  gin::IsolateHolder::AccessMode AccessMode() const override {
    return gin::IsolateHolder::AccessMode::kUseLocker;
  }
};

// This test exercises teardown of function templates with isolate holders.
TEST_F(UseLockerIsolateHolderTest, FunctionTemplateLifetime) {
  v8::Isolate* isolate = instance_->isolate();
  v8::HandleScope handle_scope(isolate);
  v8::Local<v8::Context> context = context_.Get(isolate);
  v8::Context::Scope context_scope(context);
  v8::Local<v8::ObjectTemplate> object_template =
      v8::ObjectTemplate::New(isolate);
  object_template->Set(
      isolate, "someFunction",
      gin::CreateFunctionTemplate(isolate, base::BindRepeating([]() {})));
  context->Global()
      ->Set(context, StringToV8(isolate, "someObjectTemplate"),
            object_template->NewInstance(context).ToLocalChecked())
      .Check();
  // There's nothing specific to assert - we just want to make sure that it
  // doesn't crash on teardown.
}

namespace {

struct TaskCounts {
  int ran = 0;
  int destroyed = 0;
};

class CountingTask : public v8::Task {
 public:
  explicit CountingTask(TaskCounts& counts) : counts_(counts) {}
  ~CountingTask() override { ++counts_->destroyed; }
  void Run() override {
    EXPECT_NE(v8::Isolate::GetCurrent(), nullptr);
    EXPECT_TRUE(v8::Locker::IsLocked(v8::Isolate::GetCurrent()));
    ++counts_->ran;
  }

 private:
  const raw_ref<TaskCounts> counts_;
};

class CountingIdleTask : public v8::IdleTask {
 public:
  explicit CountingIdleTask(TaskCounts& counts) : counts_(counts) {}
  ~CountingIdleTask() override { ++counts_->destroyed; }
  void Run(double deadline) override {
    EXPECT_NE(v8::Isolate::GetCurrent(), nullptr);
    EXPECT_TRUE(v8::Locker::IsLocked(v8::Isolate::GetCurrent()));
    ++counts_->ran;
  }

 private:
  const raw_ref<TaskCounts> counts_;
};

class QueuedIdleTaskRunner : public V8IdleTaskRunner {
 public:
  explicit QueuedIdleTaskRunner(
      scoped_refptr<base::SingleThreadTaskRunner> runner)
      : runner_(std::move(runner)) {}
  void PostIdleTask(const base::Location& location,
                    std::unique_ptr<v8::IdleTask> task) override {
    runner_->PostTask(location,
                      base::BindOnce(&v8::IdleTask::Run, std::move(task), 1.0));
  }

 private:
  const scoped_refptr<base::SingleThreadTaskRunner> runner_;
};

void PostLockerTasks(const std::shared_ptr<v8::TaskRunner>& runner,
                     TaskCounts& counts) {
  runner->PostTask(std::make_unique<CountingTask>(counts));
  runner->PostNonNestableTask(std::make_unique<CountingTask>(counts));
  runner->PostDelayedTask(std::make_unique<CountingTask>(counts), 0.1);
  if (runner->IdleTasksEnabled()) {
    runner->PostIdleTask(std::make_unique<CountingIdleTask>(counts));
  }
}

}  // namespace

TEST_F(UseLockerIsolateHolderTest,
       QueuedLockerTasksDoNotAccessDisposedIsolate) {
  auto queue = base::MakeRefCounted<base::TestMockTimeTaskRunner>();
  auto user_visible_queue =
      base::MakeRefCounted<base::TestMockTimeTaskRunner>();
  auto best_effort_queue = base::MakeRefCounted<base::TestMockTimeTaskRunner>();
  auto holder = std::make_unique<IsolateHolder>(
      queue, IsolateHolder::kUseLocker, IsolateHolder::kDisallowAtomicsWait,
      IsolateHolder::IsolateType::kTest,
      IsolateHolder::IsolateCreationMode::kNormal, nullptr, nullptr,
      user_visible_queue, best_effort_queue);
  holder->EnableIdleTasks(std::make_unique<QueuedIdleTaskRunner>(queue));
  auto* data = PerIsolateData::From(holder->isolate());
  const std::shared_ptr<v8::TaskRunner> runners[] = {
      data->task_runner(), data->user_visible_task_runner(),
      data->best_effort_task_runner()};

  TaskCounts live;
  for (const auto& runner : runners) {
    PostLockerTasks(runner, live);
  }
  for (const auto& task_queue :
       {queue, user_visible_queue, best_effort_queue}) {
    task_queue->FastForwardBy(base::Seconds(1));
  }
  EXPECT_EQ(live.ran, 10);
  EXPECT_EQ(live.destroyed, 10);

  TaskCounts pending;
  for (const auto& runner : runners) {
    PostLockerTasks(runner, pending);
  }
  holder.reset();
  for (const auto& task_queue :
       {queue, user_visible_queue, best_effort_queue}) {
    task_queue->FastForwardBy(base::Seconds(1));
  }
  EXPECT_EQ(pending.ran, 0);
  EXPECT_EQ(pending.destroyed, 10);

  // Producers may retain a TaskRunner after disposal. Later posts must also
  // release their tasks without accessing the isolate.
  TaskCounts late;
  for (const auto& runner : runners) {
    PostLockerTasks(runner, late);
  }
  for (const auto& task_queue :
       {queue, user_visible_queue, best_effort_queue}) {
    task_queue->FastForwardBy(base::Seconds(1));
  }
  EXPECT_EQ(late.ran, 0);
  EXPECT_EQ(late.destroyed, 10);
}

TEST_F(UseLockerIsolateHolderTest, ConcurrentPostingDuringIsolateDisposal) {
  auto queue = base::MakeRefCounted<base::TestMockTimeTaskRunner>();
  auto holder = std::make_unique<IsolateHolder>(
      queue, IsolateHolder::kUseLocker, IsolateHolder::IsolateType::kTest);
  auto runner = PerIsolateData::From(holder->isolate())->task_runner();
  TaskCounts pending;
  base::Thread producer("LockerTaskProducer");
  ASSERT_TRUE(producer.Start());
  producer.task_runner()->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](std::shared_ptr<v8::TaskRunner> runner, TaskCounts& counts) {
            for (int i = 0; i < 100; ++i) {
              runner->PostTask(std::make_unique<CountingTask>(counts));
            }
          },
          runner, std::ref(pending)));
  holder.reset();
  producer.Stop();
  queue->RunUntilIdle();
  EXPECT_EQ(pending.ran, 0);
  EXPECT_EQ(pending.destroyed, 100);
}

}  // namespace gin
