// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/napi/native_runtime_retirement.h"

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/memory/ref_counted.h"
#include "base/sequence_checker.h"
#include "base/task/single_thread_task_runner.h"

namespace xenon {
namespace {

class Retirement final : public base::RefCounted<Retirement> {
 public:
  Retirement(std::unique_ptr<NativeRuntimeShutdown> runtime,
             NativeRuntimeRetirementOptions options)
      : runtime_(std::move(runtime)),
        options_(std::move(options)),
        runner_(base::SingleThreadTaskRunner::GetCurrentDefault()) {
    CHECK(runtime_);
    CHECK(options_.on_stalled);
    CHECK(options_.poll_interval.is_positive());
    CHECK(options_.deadline.is_positive());
  }

  void Start() {
    CHECK(
        runner_->PostTask(FROM_HERE, base::BindOnce(&Retirement::Begin, this)));
  }

 private:
  friend class base::RefCounted<Retirement>;
  ~Retirement() {
    // Dropping scheduled tasks during task-runner shutdown is not evidence
    // that native callbacks no longer borrow this runtime. The process owner
    // must keep its sequence running or terminate the process atomically.
    CHECK(!runtime_);
  }

  void Begin() {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    started_ = base::TimeTicks::Now();
    runtime_->BeginNativeShutdown();
    Poll();
  }

  void Poll() {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (runtime_->PollNativeShutdown()) {
      runtime_.reset();
      if (options_.on_complete) {
        std::move(options_.on_complete).Run();
      }
      return;
    }
    if (options_.on_stalled &&
        base::TimeTicks::Now() - started_ >= options_.deadline) {
      std::move(options_.on_stalled).Run(runtime_->DescribeNativeShutdown());
    }
    CHECK(runner_->PostDelayedTask(FROM_HERE,
                                   base::BindOnce(&Retirement::Poll, this),
                                   options_.poll_interval));
  }

  std::unique_ptr<NativeRuntimeShutdown> runtime_;
  NativeRuntimeRetirementOptions options_;
  const scoped_refptr<base::SingleThreadTaskRunner> runner_;
  base::TimeTicks started_;
  SEQUENCE_CHECKER(sequence_checker_);
};

}  // namespace

void RetireNativeRuntime(std::unique_ptr<NativeRuntimeShutdown> runtime,
                         NativeRuntimeRetirementOptions options) {
  base::MakeRefCounted<Retirement>(std::move(runtime), std::move(options))
      ->Start();
}

}  // namespace xenon
