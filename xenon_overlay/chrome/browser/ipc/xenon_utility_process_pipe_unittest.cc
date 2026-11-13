// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_utility_process_pipe.h"

#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/task/thread_pool.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace xenon::ipc {
namespace {

class UtilityProcessPipeTest : public testing::Test {
 protected:
  base::test::TaskEnvironment task_environment_;
};

TEST_F(UtilityProcessPipeTest, PreservesBinaryDataAndReadsOnlyRequestedRange) {
  base::File writer;
  std::string error;
  auto pipe = UtilityProcessPipe::Create(&writer, &error);
  ASSERT_TRUE(pipe) << error;
  const uint8_t bytes[] = {0, 255, 128, 3, 4, 5};
  ASSERT_TRUE(writer.WriteAtCurrentPosAndCheck(bytes));
  auto first = pipe->Read(2);
  EXPECT_TRUE(first.error.empty()) << first.error;
  EXPECT_EQ((std::vector<uint8_t>{0, 255}), first.data);
  EXPECT_FALSE(first.eof);
  auto rest = pipe->Read(65536);
  EXPECT_TRUE(rest.error.empty()) << rest.error;
  EXPECT_EQ((std::vector<uint8_t>{128, 3, 4, 5}), rest.data);
  writer.Close();
  auto end = pipe->Read(1);
  EXPECT_TRUE(end.error.empty()) << end.error;
  EXPECT_TRUE(end.eof);
}

TEST_F(UtilityProcessPipeTest,
       CancellationReleasesPendingReadWithoutClosingItsHandleEarly) {
  base::File writer;
  std::string error;
  auto pipe = UtilityProcessPipe::Create(&writer, &error);
  ASSERT_TRUE(pipe) << error;
  base::test::TestFuture<void> pending;
  pipe->SetReadPendingCallbackForTesting(pending.GetSequenceBoundCallback());
  base::test::TestFuture<UtilityProcessPipe::ReadResult> result;
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock()},
      base::BindOnce(&UtilityProcessPipe::Read, pipe, 1024),
      result.GetCallback());
  ASSERT_TRUE(pending.Wait());
  // The OS operation is waiting on an empty pipe. Closing a different
  // reference must not free the read handle or its OVERLAPPED prematurely.
  pipe->Cancel();
  pipe.reset();
  auto cancelled = result.Take();
  EXPECT_TRUE(cancelled.eof);
  EXPECT_TRUE(cancelled.error.empty()) << cancelled.error;
  EXPECT_TRUE(cancelled.data.empty());
}

TEST_F(UtilityProcessPipeTest, RejectsUnboundedReadsBeforeTouchingThePipe) {
  base::File writer;
  std::string error;
  auto pipe = UtilityProcessPipe::Create(&writer, &error);
  ASSERT_TRUE(pipe) << error;
  EXPECT_FALSE(pipe->Read(0).error.empty());
  EXPECT_FALSE(pipe->Read(65537).error.empty());
}

}  // namespace
}  // namespace xenon::ipc
