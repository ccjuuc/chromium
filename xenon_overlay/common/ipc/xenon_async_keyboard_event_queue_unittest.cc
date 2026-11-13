// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/common/ipc/xenon_async_keyboard_event_queue.h"

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/test/bind.h"
#include "base/test/task_environment.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace xenon::ipc {
namespace {

class AsyncKeyboardEventQueueTest : public testing::Test {
 protected:
  using Reply = AsyncKeyboardEventQueue::Reply;

  AsyncKeyboardEventQueue::Filter Hold(int id) {
    return base::BindLambdaForTesting([this, id](Reply reply) {
      started_.push_back(id);
      replies_.push_back(std::move(reply));
    });
  }

  AsyncKeyboardEventQueue::Dispatch Record(int id) {
    return base::BindLambdaForTesting([this, id](bool prevented) {
      dispatched_.emplace_back(id, prevented);
    });
  }

  base::test::TaskEnvironment tasks_{
      base::test::TaskEnvironment::TimeSource::MOCK_TIME};
  std::vector<int> started_;
  std::vector<Reply> replies_;
  std::vector<std::pair<int, bool>> dispatched_;
};

TEST_F(AsyncKeyboardEventQueueTest, PreservesFifoWithOneFilterInFlight) {
  AsyncKeyboardEventQueue queue;
  for (int id = 0; id < 3; ++id) {
    ASSERT_TRUE(queue.Enqueue(Hold(id), Record(id)));
  }
  EXPECT_TRUE(started_.empty());
  for (int id = 0; id < 3; ++id) {
    tasks_.RunUntilIdle();
    ASSERT_EQ(static_cast<size_t>(id + 1), started_.size());
    EXPECT_EQ(id, started_.back());
    ASSERT_EQ(static_cast<size_t>(id + 1), replies_.size());
    std::move(replies_[id]).Run(id == 1);
    // The next filter is posted only after dispatch has returned.
    EXPECT_EQ(static_cast<size_t>(id + 1), started_.size());
  }
  EXPECT_EQ(
      (std::vector<std::pair<int, bool>>{{0, false}, {1, true}, {2, false}}),
      dispatched_);
}

TEST_F(AsyncKeyboardEventQueueTest, SynchronousRepliesDoNotRecurse) {
  AsyncKeyboardEventQueue queue;
  int depth = 0;
  int maximum_depth = 0;
  for (int id = 0; id < 128; ++id) {
    ASSERT_TRUE(queue.Enqueue(base::BindLambdaForTesting([&](Reply reply) {
                                maximum_depth =
                                    std::max(maximum_depth, ++depth);
                                std::move(reply).Run(false);
                                --depth;
                              }),
                              Record(id)));
  }
  tasks_.RunUntilIdle();
  EXPECT_EQ(1, maximum_depth);
  ASSERT_EQ(128u, dispatched_.size());
  for (int id = 0; id < 128; ++id) {
    EXPECT_EQ(id, dispatched_[id].first);
  }
}

TEST_F(AsyncKeyboardEventQueueTest, DispatchMayEnqueueWithoutOvertaking) {
  AsyncKeyboardEventQueue queue;
  ASSERT_TRUE(
      queue.Enqueue(Hold(0), base::BindLambdaForTesting([&](bool prevented) {
                      dispatched_.emplace_back(0, prevented);
                      EXPECT_TRUE(queue.Enqueue(Hold(2), Record(2)));
                      // A nested loop must not start another filter
                      // before the current dispatch has returned.
                      tasks_.RunUntilIdle();
                      EXPECT_EQ(1u, started_.size());
                    })));
  ASSERT_TRUE(queue.Enqueue(Hold(1), Record(1)));
  for (int id = 0; id < 3; ++id) {
    tasks_.RunUntilIdle();
    ASSERT_EQ(static_cast<size_t>(id + 1), replies_.size());
    std::move(replies_[id]).Run(false);
  }
  EXPECT_EQ((std::vector<int>{0, 1, 2}), started_);
}

TEST_F(AsyncKeyboardEventQueueTest,
       CancelDropsPendingAndLateRepliesPermanently) {
  AsyncKeyboardEventQueue queue;
  ASSERT_TRUE(queue.Enqueue(Hold(0), Record(0)));
  ASSERT_TRUE(queue.Enqueue(Hold(1), Record(1)));
  tasks_.RunUntilIdle();
  ASSERT_EQ(1u, replies_.size());
  queue.Cancel();
  queue.Cancel();
  EXPECT_FALSE(queue.Enqueue(Hold(2), Record(2)));
  std::move(replies_[0]).Run(false);
  tasks_.RunUntilIdle();
  EXPECT_EQ((std::vector<int>{0}), started_);
  EXPECT_TRUE(dispatched_.empty());
}

TEST_F(AsyncKeyboardEventQueueTest,
       FiveSecondTimeoutDiscardsRatherThanReplays) {
  AsyncKeyboardEventQueue queue;
  ASSERT_TRUE(queue.Enqueue(Hold(0), Record(0)));
  ASSERT_TRUE(queue.Enqueue(Hold(1), Record(1)));
  tasks_.RunUntilIdle();
  tasks_.FastForwardBy(base::Seconds(5) - base::Milliseconds(1));
  ASSERT_EQ(1u, replies_.size());
  EXPECT_TRUE(dispatched_.empty());
  tasks_.FastForwardBy(base::Milliseconds(1));
  std::move(replies_[0]).Run(false);
  tasks_.RunUntilIdle();
  EXPECT_FALSE(queue.Enqueue(Hold(2), Record(2)));
  EXPECT_EQ((std::vector<int>{0}), started_);
  EXPECT_TRUE(dispatched_.empty());
}

TEST_F(AsyncKeyboardEventQueueTest, OverflowCancelsAndReleasesOwnedCallbacks) {
  AsyncKeyboardEventQueue queue;
  auto token = std::make_shared<int>(1);
  std::weak_ptr<int> observer = token;
  ASSERT_TRUE(queue.Enqueue(Hold(0), base::BindLambdaForTesting([token](bool) {
                              static_cast<void>(token);
                            })));
  token.reset();
  tasks_.RunUntilIdle();
  EXPECT_FALSE(observer.expired());
  for (int id = 1; id < 512; ++id) {
    ASSERT_TRUE(queue.Enqueue(Hold(id), Record(id)));
  }
  EXPECT_FALSE(queue.Enqueue(Hold(512), Record(512)));
  EXPECT_TRUE(observer.expired());
  ASSERT_EQ(1u, replies_.size());
  std::move(replies_[0]).Run(false);
  tasks_.RunUntilIdle();
  EXPECT_FALSE(queue.Enqueue(Hold(513), Record(513)));
  EXPECT_EQ((std::vector<int>{0}), started_);
  EXPECT_TRUE(dispatched_.empty());
}

TEST_F(AsyncKeyboardEventQueueTest, DispatchCanDestroyQueue) {
  auto queue = std::make_unique<AsyncKeyboardEventQueue>();
  ASSERT_TRUE(queue->Enqueue(
      base::BindOnce([](Reply reply) { std::move(reply).Run(false); }),
      base::BindLambdaForTesting([&](bool) { queue.reset(); })));
  ASSERT_TRUE(queue->Enqueue(Hold(1), Record(1)));
  tasks_.RunUntilIdle();
  EXPECT_FALSE(queue);
  EXPECT_TRUE(started_.empty());
  EXPECT_TRUE(dispatched_.empty());
}

TEST_F(AsyncKeyboardEventQueueTest, FilterCanDestroyQueueWithoutReplying) {
  auto queue = std::make_unique<AsyncKeyboardEventQueue>();
  ASSERT_TRUE(queue->Enqueue(
      base::BindLambdaForTesting([&](Reply) { queue.reset(); }), Record(0)));
  tasks_.RunUntilIdle();
  EXPECT_FALSE(queue);
  EXPECT_TRUE(dispatched_.empty());
}

}  // namespace
}  // namespace xenon::ipc
