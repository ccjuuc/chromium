// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/common/ipc/xenon_async_keyboard_event_queue.h"

#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/task/sequenced_task_runner.h"

namespace xenon::ipc {

AsyncKeyboardEventQueue::AsyncKeyboardEventQueue(base::TimeDelta timeout,
                                                 size_t capacity)
    : timeout_(timeout), capacity_(capacity) {
  CHECK(timeout_.is_positive());
  CHECK(capacity_ > 0u);
}

AsyncKeyboardEventQueue::~AsyncKeyboardEventQueue() {
  Cancel();
}

bool AsyncKeyboardEventQueue::Enqueue(Filter filter, Dispatch dispatch) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  CHECK(filter);
  CHECK(dispatch);
  if (canceled_) {
    return false;
  }
  if (pending_.size() + (active_ || dispatching_ ? 1 : 0) >= capacity_) {
    Cancel();
    return false;
  }
  pending_.push_back({std::move(filter), std::move(dispatch)});
  ScheduleNext();
  return true;
}

void AsyncKeyboardEventQueue::Cancel() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  canceled_ = true;
  timer_.Stop();
  weak_factory_.InvalidateWeakPtrs();
  active_.reset();
  pending_.clear();
  next_posted_ = false;
}

bool AsyncKeyboardEventQueue::HasPending() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return !pending_.empty() || active_.has_value() || dispatching_;
}

bool AsyncKeyboardEventQueue::is_canceled() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return canceled_;
}

void AsyncKeyboardEventQueue::ScheduleNext() {
  if (canceled_ || next_posted_ || active_ || dispatching_ ||
      pending_.empty()) {
    return;
  }
  next_posted_ = true;
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&AsyncKeyboardEventQueue::StartNext,
                                weak_factory_.GetWeakPtr()));
}

void AsyncKeyboardEventQueue::StartNext() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  next_posted_ = false;
  if (canceled_ || active_ || dispatching_ || pending_.empty()) {
    return;
  }
  active_.emplace(std::move(pending_.front()));
  pending_.pop_front();
  timer_.Start(FROM_HERE, timeout_,
               base::BindOnce(&AsyncKeyboardEventQueue::Cancel,
                              weak_factory_.GetWeakPtr()));
  auto filter = std::move(active_->filter);
  std::move(filter).Run(base::BindOnce(&AsyncKeyboardEventQueue::OnReply,
                                       weak_factory_.GetWeakPtr()));
  // Synchronous replies can dispatch user code and destroy the queue. Do not
  // access any member after invoking the filter.
}

void AsyncKeyboardEventQueue::OnReply(bool prevented) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (canceled_ || !active_) {
    return;
  }
  timer_.Stop();
  auto dispatch = std::move(active_->dispatch);
  active_.reset();
  dispatching_ = true;
  auto alive = weak_factory_.GetWeakPtr();
  std::move(dispatch).Run(prevented);
  if (!alive) {
    return;
  }
  dispatching_ = false;
  ScheduleNext();
}

}  // namespace xenon::ipc
