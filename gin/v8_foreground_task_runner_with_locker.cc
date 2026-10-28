// Copyright 2017 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "gin/v8_foreground_task_runner_with_locker.h"

#include <memory>
#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/task/single_thread_task_runner.h"
#include "base/threading/platform_thread.h"
#include "base/time/time.h"
#include "gin/converter.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-locker.h"

namespace gin {

struct V8ForegroundTaskRunnerWithLocker::IsolateState {
  explicit IsolateState(v8::Isolate* isolate) : isolate(isolate) {}

  void Run(std::unique_ptr<v8::Task> task) {
    DCHECK(foreground_thread == base::PlatformThread::CurrentRef());
    if (!isolate) {
      return;
    }
    v8::Locker lock(isolate);
    v8::Isolate::Scope isolate_scope(isolate);
    task->Run();
  }

  void RunIdle(std::unique_ptr<v8::IdleTask> task, double deadline) {
    DCHECK(foreground_thread == base::PlatformThread::CurrentRef());
    if (!isolate) {
      return;
    }
    v8::Locker lock(isolate);
    v8::Isolate::Scope isolate_scope(isolate);
    task->Run(deadline);
  }

  void Invalidate() {
    DCHECK(foreground_thread == base::PlatformThread::CurrentRef());
    isolate = nullptr;
  }

  // Execution and invalidation are serialized on the foreground thread. Its
  // priority queues need not have the same sequence token.
  // Other threads may retain the state while posting tasks, but never read or
  // write this pointer. The state can outlive the isolate after invalidation.
  raw_ptr<v8::Isolate> isolate;
  const base::PlatformThreadRef foreground_thread =
      base::PlatformThread::CurrentRef();
};

V8ForegroundTaskRunnerWithLocker::V8ForegroundTaskRunnerWithLocker(
    v8::Isolate* isolate,
    scoped_refptr<base::SingleThreadTaskRunner> task_runner)
    : isolate_state_(std::make_shared<IsolateState>(isolate)),
      task_runner_(std::move(task_runner)) {
  DCHECK(task_runner_);
}

V8ForegroundTaskRunnerWithLocker::~V8ForegroundTaskRunnerWithLocker() = default;

namespace {

class IdleTaskWithLocker : public v8::IdleTask {
 public:
  explicit IdleTaskWithLocker(base::OnceCallback<void(double)> run)
      : run_(std::move(run)) {}

  void Run(double deadline_in_seconds) override {
    std::move(run_).Run(deadline_in_seconds);
  }

 private:
  base::OnceCallback<void(double)> run_;
};

}  // namespace

void V8ForegroundTaskRunnerWithLocker::PostTaskImpl(
    std::unique_ptr<v8::Task> task,
    const v8::SourceLocation& location) {
  task_runner_->PostTask(
      V8ToBaseLocation(location),
      base::BindOnce(&IsolateState::Run, isolate_state_, std::move(task)));
}

void V8ForegroundTaskRunnerWithLocker::PostNonNestableTaskImpl(
    std::unique_ptr<v8::Task> task,
    const v8::SourceLocation& location) {
  task_runner_->PostNonNestableTask(
      V8ToBaseLocation(location),
      base::BindOnce(&IsolateState::Run, isolate_state_, std::move(task)));
}

void V8ForegroundTaskRunnerWithLocker::PostDelayedTaskImpl(
    std::unique_ptr<v8::Task> task,
    double delay_in_seconds,
    const v8::SourceLocation& location) {
  task_runner_->PostDelayedTask(
      V8ToBaseLocation(location),
      base::BindOnce(&IsolateState::Run, isolate_state_, std::move(task)),
      base::Seconds(delay_in_seconds));
}

void V8ForegroundTaskRunnerWithLocker::PostIdleTaskImpl(
    std::unique_ptr<v8::IdleTask> task,
    const v8::SourceLocation& location) {
  DCHECK(IdleTasksEnabled());
  idle_task_runner()->PostIdleTask(
      V8ToBaseLocation(location),
      std::make_unique<IdleTaskWithLocker>(base::BindOnce(
          &IsolateState::RunIdle, isolate_state_, std::move(task))));
}

bool V8ForegroundTaskRunnerWithLocker::NonNestableTasksEnabled() const {
  return true;
}

void V8ForegroundTaskRunnerWithLocker::InvalidateIsolate() {
  DCHECK(task_runner_->BelongsToCurrentThread());
  // Detach before disposal. Pending and concurrently posted tasks will release
  // their payload without trying to lock or enter the disposed isolate.
  isolate_state_->Invalidate();
}

}  // namespace gin
