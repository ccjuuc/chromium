// Copyright 2017 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GIN_V8_FOREGROUND_TASK_RUNNER_WITH_LOCKER_H_
#define GIN_V8_FOREGROUND_TASK_RUNNER_WITH_LOCKER_H_

#include <memory>

#include "base/memory/scoped_refptr.h"
#include "gin/v8_foreground_task_runner_base.h"

namespace base {
class SingleThreadTaskRunner;
}

namespace gin {

class V8ForegroundTaskRunnerWithLocker : public V8ForegroundTaskRunnerBase {
 public:
  V8ForegroundTaskRunnerWithLocker(
      v8::Isolate* isolate,
      scoped_refptr<base::SingleThreadTaskRunner> task_runner);

  ~V8ForegroundTaskRunnerWithLocker() override;

  // v8::Platform implementation.
  bool NonNestableTasksEnabled() const override;

  void InvalidateIsolate() override;

 private:
  // v8::Platform implementation.
  void PostTaskImpl(std::unique_ptr<v8::Task> task,
                    const v8::SourceLocation& location) override;
  void PostNonNestableTaskImpl(std::unique_ptr<v8::Task> task,
                               const v8::SourceLocation& location) override;
  void PostDelayedTaskImpl(std::unique_ptr<v8::Task> task,
                           double delay_in_seconds,
                           const v8::SourceLocation& location) override;
  void PostIdleTaskImpl(std::unique_ptr<v8::IdleTask> task,
                        const v8::SourceLocation& location) override;

  struct IsolateState;
  // Posting threads only copy this shared state. Its isolate is accessed and
  // invalidated on the foreground thread, before entering v8::Locker.
  const std::shared_ptr<IsolateState> isolate_state_;
  scoped_refptr<base::SingleThreadTaskRunner> task_runner_;
};

}  // namespace gin

#endif  // GIN_V8_FOREGROUND_TASK_RUNNER_WITH_LOCKER_H_
