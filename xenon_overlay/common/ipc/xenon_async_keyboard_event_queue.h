// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_COMMON_IPC_XENON_ASYNC_KEYBOARD_EVENT_QUEUE_H_
#define XENON_OVERLAY_COMMON_IPC_XENON_ASYNC_KEYBOARD_EVENT_QUEUE_H_

#include <cstddef>
#include <deque>
#include <optional>

#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/time/time.h"
#include "base/timer/timer.h"

namespace xenon::ipc {

// Serializes asynchronous preventDefault decisions without blocking the input
// sequence. Cancellation, timeout and overflow discard pending events rather
// than replaying input whose target or application generation may have changed.
class AsyncKeyboardEventQueue final {
 public:
  using Reply = base::OnceCallback<void(bool prevented)>;
  using Filter = base::OnceCallback<void(Reply)>;
  using Dispatch = base::OnceCallback<void(bool prevented)>;

  explicit AsyncKeyboardEventQueue(base::TimeDelta timeout = base::Seconds(5),
                                   size_t capacity = 512);
  ~AsyncKeyboardEventQueue();

  AsyncKeyboardEventQueue(const AsyncKeyboardEventQueue&) = delete;
  AsyncKeyboardEventQueue& operator=(const AsyncKeyboardEventQueue&) = delete;

  // The filter is invoked from a posted task. At most one filter or dispatch
  // is active; dispatch may enqueue more work or destroy this queue.
  bool Enqueue(Filter filter, Dispatch dispatch);
  // Permanent: future Enqueue calls return false and late replies are ignored.
  void Cancel();
  bool HasPending() const;
  bool is_canceled() const;

 private:
  struct Pending {
    Filter filter;
    Dispatch dispatch;
  };

  void ScheduleNext();
  void StartNext();
  void OnReply(bool prevented);

  const base::TimeDelta timeout_;
  const size_t capacity_;
  std::deque<Pending> pending_;
  std::optional<Pending> active_;
  base::OneShotTimer timer_;
  bool canceled_ = false;
  bool next_posted_ = false;
  bool dispatching_ = false;
  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<AsyncKeyboardEventQueue> weak_factory_{this};
};

}  // namespace xenon::ipc

#endif  // XENON_OVERLAY_COMMON_IPC_XENON_ASYNC_KEYBOARD_EVENT_QUEUE_H_
