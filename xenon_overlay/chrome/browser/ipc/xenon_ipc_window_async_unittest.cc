// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>
#include <string>
#include <utility>

#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/run_loop.h"
#include "base/test/test_future.h"
#include "gin/test/v8_test.h"
#include "mojo/core/embedder/embedder.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_ipc_main_container.h"

namespace xenon::ipc {
namespace {

class XenonIpcWindowAsyncTest : public gin::V8Test, public mojom::IpcRenderer {
 protected:
  using Reply = base::OnceCallback<void(base::Value, std::string)>;
  static void SetUpTestSuite() { mojo::core::Init(); }
  void SetUp() override {
    gin::V8Test::SetUp();
    ASSERT_TRUE(temp_.CreateUniqueTempDir());
    container_ = std::make_unique<XenonIpcMainContainer>();
    XenonIpcMainContainer::WindowHooks hooks;
    hooks.call_async = base::BindRepeating(
        [](XenonIpcWindowAsyncTest* self, int32_t id,
           const std::string& command, base::Value arguments, Reply reply) {
          EXPECT_EQ(0, id);
          EXPECT_EQ("content-tracing", command);
          self->arguments_ = std::move(arguments);
          if (self->inline_reply_) {
            std::move(reply).Run(
                base::Value(base::ListValue().Append("native")), "");
          } else {
            EXPECT_FALSE(self->reply_);
            self->reply_ = std::move(reply);
          }
        },
        base::Unretained(this));
    container_->SetWindowHooks(std::move(hooks));
    XenonIpcMainContainer::EmbeddedMainModule module{
        .source = R"JS(
const {ipcMain, contentTracing} = require('electron');
let completed = false;
ipcMain.handle('categories', async () => {
  try {
    const value = await contentTracing.getCategories();
    completed = true;
    return value;
  } catch (error) { return {code:error.code, message:error.message}; }
});
ipcMain.on('completed', event => { event.returnValue = completed; });
ipcMain.handle('protected-error', async () => {
  Object.defineProperty(Error.prototype, 'code', {configurable:true,
    set() { throw new Error('prototype setter must not run'); }});
  try { await contentTracing.getCategories(); }
  catch (error) { return {code:error.code, own:Object.hasOwn(error,'code')}; }
  finally { delete Error.prototype.code; }
});
)JS",
        .virtual_path = temp_.GetPath().AppendASCII("main.js"),
        .app_path = temp_.GetPath(),
    };
    ASSERT_TRUE(container_->Initialize(std::move(module)))
        << container_->startup_error();
    container_->AddRenderer("renderer", receiver_.BindNewPipeAndPassRemote(), 1,
                            1, 0, "about:blank", true);
  }
  void TearDown() override {
    container_.reset();
    receiver_.reset();
    reply_.Reset();
    gin::V8Test::TearDown();
  }
  void Dispatch(const std::string&, base::Value) override {}
  void DispatchPostMessage(const std::string&,
                           blink::TransferableMessage) override {}
  bool Completed() {
    auto result = container_->SendSync("renderer", "completed",
                                       base::Value(base::ListValue()));
    EXPECT_TRUE(result->success);
    return result->value.is_bool() && result->value.GetBool();
  }
  base::ScopedTempDir temp_;
  mojo::Receiver<mojom::IpcRenderer> receiver_{this};
  std::unique_ptr<XenonIpcMainContainer> container_;
  Reply reply_;
  base::Value arguments_;
  bool inline_reply_ = false;
};

TEST_F(XenonIpcWindowAsyncTest, PendingNativeWorkDoesNotBlockUnrelatedIpc) {
  base::test::TestFuture<mojom::IpcResultPtr> result;
  container_->Invoke("renderer", "categories", base::Value(base::ListValue()),
                     result.GetCallback());
  base::RunLoop().RunUntilIdle();
  ASSERT_TRUE(reply_);
  ASSERT_TRUE(arguments_.is_dict());
  EXPECT_EQ("getCategories", *arguments_.GetDict().FindString("method"));
  EXPECT_FALSE(result.IsReady());
  EXPECT_FALSE(Completed());
  std::move(reply_).Run(base::Value(base::ListValue().Append("native")), "");
  EXPECT_FALSE(result.IsReady());
  auto reply = result.Take();
  ASSERT_TRUE(reply->success) << reply->error;
  EXPECT_EQ(base::Value(base::ListValue().Append("native")), reply->value);
  EXPECT_TRUE(Completed());
}

TEST_F(XenonIpcWindowAsyncTest,
       InlineHostCompletionStillUnwindsBeforeDelivery) {
  inline_reply_ = true;
  base::test::TestFuture<mojom::IpcResultPtr> result;
  container_->Invoke("renderer", "categories", base::Value(base::ListValue()),
                     result.GetCallback());
  EXPECT_FALSE(result.IsReady());
  EXPECT_FALSE(Completed());
  auto reply = result.Take();
  ASSERT_TRUE(reply->success) << reply->error;
  EXPECT_TRUE(Completed());
}

TEST_F(XenonIpcWindowAsyncTest, NativeErrorsRejectWithCodeAndMessage) {
  base::test::TestFuture<mojom::IpcResultPtr> result;
  container_->Invoke("renderer", "categories", base::Value(base::ListValue()),
                     result.GetCallback());
  base::RunLoop().RunUntilIdle();
  ASSERT_TRUE(reply_);
  std::move(reply_).Run({}, "ERR_ACCESS_DENIED: unavailable to this owner");
  auto reply = result.Take();
  ASSERT_TRUE(reply->success) << reply->error;
  ASSERT_TRUE(reply->value.is_dict());
  EXPECT_EQ("ERR_ACCESS_DENIED", *reply->value.GetDict().FindString("code"));
  EXPECT_FALSE(Completed());
}

TEST_F(XenonIpcWindowAsyncTest,
       LateReplyAfterContainerRetirementDoesNotEnterV8) {
  base::test::TestFuture<mojom::IpcResultPtr> result;
  container_->Invoke("renderer", "categories", base::Value(base::ListValue()),
                     result.GetCallback());
  base::RunLoop().RunUntilIdle();
  ASSERT_TRUE(reply_);
  container_.reset();
  std::move(reply_).Run(base::Value("late"), "");
  base::RunLoop().RunUntilIdle();
}

TEST_F(XenonIpcWindowAsyncTest, NativeErrorDoesNotInvokePrototypeSetter) {
  base::test::TestFuture<mojom::IpcResultPtr> result;
  container_->Invoke("renderer", "protected-error",
                     base::Value(base::ListValue()), result.GetCallback());
  base::RunLoop().RunUntilIdle();
  ASSERT_TRUE(reply_);
  std::move(reply_).Run({}, "ERR_ACCESS_DENIED: expected native error");
  auto reply = result.Take();
  ASSERT_TRUE(reply->success) << reply->error;
  ASSERT_TRUE(reply->value.is_dict());
  EXPECT_EQ("ERR_ACCESS_DENIED", *reply->value.GetDict().FindString("code"));
  EXPECT_TRUE(reply->value.GetDict().FindBool("own").value_or(false));
}

}  // namespace
}  // namespace xenon::ipc
