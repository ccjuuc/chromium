// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_electron_api_bridge.h"

#include <string>
#include <utility>

#include "base/files/scoped_temp_dir.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "build/build_config.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/base/clipboard/clipboard.h"
#include "ui/base/clipboard/test/test_clipboard.h"

namespace xenon::ipc {
namespace {

class XenonElectronApiBridgeTest : public testing::Test {
 protected:
  void SetUp() override {
    // TestClipboard is entirely in memory: these tests never touch the user's
    // operating-system clipboard or launch an external application.
    ui::TestClipboard::CreateForCurrentThread();
  }

  void TearDown() override {
    ui::Clipboard::DestroyClipboardForCurrentThread();
  }

  mojom::IpcResultPtr Call(base::DictValue arguments) {
    base::test::TestFuture<mojom::IpcResultPtr> future;
    CallElectronApi(base::Value(std::move(arguments)), future.GetCallback());
    return future.Take();
  }

  base::test::TaskEnvironment task_environment_{
      base::test::TaskEnvironment::MainThreadType::UI};
};

TEST_F(XenonElectronApiBridgeTest, ClipboardTextRoundTripAndClear) {
  auto write = Call(base::DictValue()
                        .Set("operation", "clipboard.writeText")
                        .Set("text", "Unicode: \xe4\xb8\xad\xe6\x96\x87"));
  ASSERT_TRUE(write->success) << write->error;
  auto read = Call(base::DictValue().Set("operation", "clipboard.readText"));
  ASSERT_TRUE(read->success) << read->error;
  EXPECT_EQ("Unicode: \xe4\xb8\xad\xe6\x96\x87", read->value.GetString());
  ASSERT_TRUE(
      Call(base::DictValue().Set("operation", "clipboard.clear"))->success);
  auto empty = Call(base::DictValue().Set("operation", "clipboard.readText"));
  ASSERT_TRUE(empty->success) << empty->error;
  EXPECT_TRUE(empty->value.GetString().empty());
}

TEST_F(XenonElectronApiBridgeTest, ClipboardHtmlRoundTrip) {
  auto write = Call(base::DictValue()
                        .Set("operation", "clipboard.writeHTML")
                        .Set("markup", "<strong>fixture</strong>"));
  ASSERT_TRUE(write->success) << write->error;
  auto read = Call(base::DictValue().Set("operation", "clipboard.readHTML"));
  ASSERT_TRUE(read->success) << read->error;
  EXPECT_EQ("<strong>fixture</strong>", read->value.GetString());
}

TEST_F(XenonElectronApiBridgeTest, RendererArgumentListUsesTheSameBridge) {
  auto write = Call(base::DictValue()
                        .Set("operation", "clipboard.writeText")
                        .Set("text", "fixture from main"));
  ASSERT_TRUE(write->success) << write->error;
  base::test::TestFuture<mojom::IpcResultPtr> future;
  CallElectronApi(base::Value(base::ListValue().Append(
                      base::DictValue().Set("operation", "clipboard.readText"))),
                  future.GetCallback());
  auto read = future.Take();
  ASSERT_TRUE(read->success) << read->error;
  EXPECT_EQ("fixture from main", read->value.GetString());
  for (auto invalid : {0, 1, 2}) {
    base::ListValue values;
    for (int i = 0; i < invalid; ++i) values.Append(42);
    CallElectronApi(base::Value(std::move(values)), future.GetCallback());
    auto result = future.Take();
    EXPECT_FALSE(result->success);
    EXPECT_TRUE(result->error.starts_with("ERR_INVALID_ARG_TYPE:"));
  }
}

TEST_F(XenonElectronApiBridgeTest, ClipboardRejectsInvalidContentAndBuffer) {
  auto content = Call(base::DictValue()
                          .Set("operation", "clipboard.writeText")
                          .Set("text", 42));
  EXPECT_FALSE(content->success);
  EXPECT_TRUE(content->error.starts_with("ERR_INVALID_ARG_TYPE:"));
  auto buffer = Call(base::DictValue()
                         .Set("operation", "clipboard.readText")
                         .Set("type", "selection"));
  EXPECT_FALSE(buffer->success);
  EXPECT_TRUE(buffer->error.starts_with("ERR_NOT_SUPPORTED:"));
}

TEST_F(XenonElectronApiBridgeTest, InvalidRequestsAndUnknownApisFail) {
  base::test::TestFuture<mojom::IpcResultPtr> future;
  CallElectronApi(base::Value(42), future.GetCallback());
  auto invalid = future.Take();
  EXPECT_FALSE(invalid->success);
  EXPECT_TRUE(invalid->error.starts_with("ERR_INVALID_ARG_TYPE:"));
  auto missing = Call(base::DictValue());
  EXPECT_FALSE(missing->success);
  EXPECT_TRUE(missing->error.starts_with("ERR_INVALID_ARG_TYPE:"));
  auto unknown = Call(base::DictValue().Set("operation", "shell.fakeSuccess"));
  EXPECT_FALSE(unknown->success);
  EXPECT_TRUE(unknown->error.starts_with("ERR_NOT_SUPPORTED:"));
}

#if BUILDFLAG(IS_WIN)
TEST_F(XenonElectronApiBridgeTest, ShellRejectsInvalidTargetsBeforeLaunch) {
  for (const auto* operation :
       {"shell.openExternal", "shell.openPath", "shell.showItemInFolder"}) {
    auto missing = Call(base::DictValue().Set("operation", operation));
    EXPECT_FALSE(missing->success);
    EXPECT_TRUE(missing->error.starts_with("ERR_INVALID_ARG_TYPE:"));
    auto empty = Call(base::DictValue()
                          .Set("operation", operation)
                          .Set("url", "")
                          .Set("path", ""));
    EXPECT_FALSE(empty->success);
    EXPECT_TRUE(empty->error.starts_with("ERR_INVALID_ARG_VALUE:"));
  }
  auto url = Call(base::DictValue()
                      .Set("operation", "shell.openExternal")
                      .Set("url", "not a URL"));
  EXPECT_FALSE(url->success);
  EXPECT_TRUE(url->error.starts_with("ERR_INVALID_ARG_VALUE:"));
  auto nul =
      Call(base::DictValue()
               .Set("operation", "shell.openExternal")
               .Set("url", std::string("https://invalid.example/\0", 25)));
  EXPECT_FALSE(nul->success);
  EXPECT_TRUE(nul->error.starts_with("ERR_INVALID_ARG_VALUE:"));
}

TEST_F(XenonElectronApiBridgeTest, ShellRejectsInvalidOptionsBeforeLaunch) {
  auto type = Call(base::DictValue()
                       .Set("operation", "shell.openExternal")
                       .Set("url", "https://invalid.example/")
                       .Set("options", 7));
  EXPECT_FALSE(type->success);
  EXPECT_TRUE(type->error.starts_with("ERR_INVALID_ARG_TYPE:"));
  auto unknown =
      Call(base::DictValue()
               .Set("operation", "shell.openExternal")
               .Set("url", "https://invalid.example/")
               .Set("options", base::DictValue().Set("unsupported", true)));
  EXPECT_FALSE(unknown->success);
  EXPECT_TRUE(unknown->error.starts_with("ERR_NOT_SUPPORTED:"));
  auto activate =
      Call(base::DictValue()
               .Set("operation", "shell.openExternal")
               .Set("url", "https://invalid.example/")
               .Set("options", base::DictValue().Set("activate", 1)));
  EXPECT_FALSE(activate->success);
  EXPECT_TRUE(activate->error.starts_with("ERR_INVALID_ARG_TYPE:"));
}

TEST_F(XenonElectronApiBridgeTest, MissingPathPreservesElectronResultContract) {
  base::ScopedTempDir directory;
  ASSERT_TRUE(directory.CreateUniqueTempDir());
  const std::string missing =
      directory.GetPath().AppendASCII("not-created.txt").AsUTF8Unsafe();
  auto open = Call(base::DictValue()
                       .Set("operation", "shell.openPath")
                       .Set("path", missing));
  ASSERT_TRUE(open->success) << open->error;
  ASSERT_TRUE(open->value.is_string());
  EXPECT_FALSE(open->value.GetString().empty());
  auto reveal = Call(base::DictValue()
                         .Set("operation", "shell.showItemInFolder")
                         .Set("path", missing));
  EXPECT_FALSE(reveal->success);
  EXPECT_TRUE(reveal->error.starts_with("ENOENT:"));
}
#endif  // BUILDFLAG(IS_WIN)

}  // namespace
}  // namespace xenon::ipc
