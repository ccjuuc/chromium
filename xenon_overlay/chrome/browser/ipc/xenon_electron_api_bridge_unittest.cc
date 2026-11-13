// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_electron_api_bridge.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/containers/span.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/test/bind.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "build/build_config.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/base/clipboard/clipboard.h"
#include "ui/base/clipboard/test/test_clipboard.h"
#include "ui/gfx/codec/png_codec.h"
#include "ui/native_theme/mock_os_settings_provider.h"
#include "xenon_overlay/chrome/browser/updater/xenon_update_manager.h"

#if BUILDFLAG(IS_MAC)
#include "base/apple/foundation_util.h"
#include "base/mac/mac_util.h"
#endif

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

TEST_F(XenonElectronApiBridgeTest, SystemThemeReadsCurrentPlatformProvider) {
  ui::MockOsSettingsProvider provider;
  provider.SetPreferredColorScheme(ui::NativeTheme::PreferredColorScheme::kDark);
  provider.SetForcedColorsActive(true);
  provider.SetPrefersInvertedColors(true);
  provider.SetPrefersReducedTransparency(true);
  provider.SetAccentColor(SkColorSetARGB(0x80, 0x12, 0x34, 0xab));
  auto state = Call(base::DictValue().Set("operation", "nativeTheme.getState"));
  ASSERT_TRUE(state->success) << state->error;
  EXPECT_EQ(true, state->value.GetDict().FindBool("shouldUseDarkColors"));
  EXPECT_EQ(true, state->value.GetDict().FindBool("shouldUseHighContrastColors"));
  EXPECT_EQ(true, state->value.GetDict().FindBool("shouldUseInvertedColorScheme"));
  EXPECT_EQ(true, state->value.GetDict().FindBool("prefersReducedTransparency"));
  auto accent = Call(base::DictValue().Set("operation", "systemPreferences.getAccentColor"));
  ASSERT_TRUE(accent->success) << accent->error;
  EXPECT_EQ("1234ab80", accent->value.GetString());
  provider.SetPreferredColorScheme(ui::NativeTheme::PreferredColorScheme::kLight);
  auto light = Call(base::DictValue().Set("operation", "nativeTheme.getState"));
  ASSERT_TRUE(light->success);
  EXPECT_EQ(false, light->value.GetDict().FindBool("shouldUseDarkColors"));
}

TEST_F(XenonElectronApiBridgeTest, MissingSystemAccentIsNotFabricated) {
  ui::MockOsSettingsProvider provider;
  auto result = Call(base::DictValue().Set("operation", "systemPreferences.getAccentColor"));
  EXPECT_FALSE(result->success);
  EXPECT_TRUE(result->error.starts_with("ERR_NOT_SUPPORTED:"));
}

TEST_F(XenonElectronApiBridgeTest, SystemThemeNotificationsTrackChangesAndUnsubscribe) {
  ui::MockOsSettingsProvider provider;
  provider.SetAccentColor(SK_ColorRED);
  std::vector<base::DictValue> events;
  SetElectronThemeEventCallback(base::BindLambdaForTesting(
      [&](base::DictValue details) { events.push_back(std::move(details)); }));
  base::ScopedClosureRunner clear_callback(base::BindOnce(
      [] { SetElectronThemeEventCallback({}); }));
  EXPECT_TRUE(events.empty());
  provider.SetAccentColor(SK_ColorBLUE);
  ASSERT_EQ(1u, events.size());
  EXPECT_EQ(true, events.back().FindBool("accentChanged"));
  EXPECT_EQ(false, events.back().FindBool("nativeThemeChanged"));
  ASSERT_TRUE(events.back().FindDict("state"));
  EXPECT_EQ("0000ffff", *events.back().FindDict("state")->FindString("accentColor"));
  provider.SetAccentColor(SK_ColorBLUE);
  EXPECT_EQ(1u, events.size());
  provider.SetPreferredColorScheme(ui::NativeTheme::PreferredColorScheme::kDark);
  ASSERT_EQ(2u, events.size());
  EXPECT_EQ(false, events.back().FindBool("accentChanged"));
  EXPECT_EQ(true, events.back().FindBool("nativeThemeChanged"));
  EXPECT_EQ(true, events.back().FindDict("state")->FindBool("shouldUseDarkColors"));
  SetElectronThemeEventCallback({});
  provider.SetAccentColor(SK_ColorGREEN);
  EXPECT_EQ(2u, events.size());
}

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

base::DictValue ClipboardRepresentation(std::string_view mime,
                                         std::string_view bytes) {
  return base::DictValue()
      .Set("mime", mime)
      .Set("data", base::Base64Encode(bytes));
}

TEST_F(XenonElectronApiBridgeTest,
       ClipboardItemsCommitAllTextRepresentationsTogether) {
  const std::pair<std::string, std::string> representations[] = {
      {"text/plain", "Unicode: \xe4\xb8\xad\xe6\x96\x87"},
      {"text/html", "<strong>fixture</strong>"},
      {"text/rtf", "{\\rtf1 fixture}"},
      {"image/svg+xml", "<svg xmlns=\"http://www.w3.org/2000/svg\"/>"},
  };
  base::ListValue items;
  items.Append(base::ListValue().Append(
      ClipboardRepresentation("text/plain", "superseded")));
  for (const auto& [mime, bytes] : representations) {
    items.Append(base::ListValue().Append(ClipboardRepresentation(mime, bytes)));
  }
  auto write = Call(base::DictValue()
                        .Set("operation", "clipboard.writeItems")
                        .Set("items", std::move(items)));
  ASSERT_TRUE(write->success) << write->error;
  auto types = Call(base::DictValue().Set("operation", "clipboard.readTypes"));
  ASSERT_TRUE(types->success) << types->error;
  ASSERT_TRUE(types->value.is_list());
  EXPECT_EQ(4u, types->value.GetList().size());
  for (const auto& [mime, bytes] : representations) {
    auto has = Call(base::DictValue()
                        .Set("operation", "clipboard.has")
                        .Set("mime", mime));
    ASSERT_TRUE(has->success) << has->error;
    EXPECT_TRUE(has->value.GetBool()) << mime;
    auto read = Call(base::DictValue()
                         .Set("operation", "clipboard.readType")
                         .Set("mime", mime));
    ASSERT_TRUE(read->success) << read->error;
    EXPECT_EQ(base::Base64Encode(bytes), read->value.GetString()) << mime;
  }
}

TEST_F(XenonElectronApiBridgeTest,
       ClipboardPngAndHtmlRoundTripThroughNativeImageStorage) {
  SkBitmap original;
  original.allocN32Pixels(2, 1);
  original.eraseColor(SK_ColorBLUE);
  const auto png = gfx::PNGCodec::EncodeBGRASkBitmap(original, false);
  ASSERT_TRUE(png);
  base::ListValue representations;
  representations.Append(base::DictValue()
                             .Set("mime", "image/png")
                             .Set("data", base::Base64Encode(*png)));
  representations.Append(
      ClipboardRepresentation("text/html", "<img width=\"2\" height=\"1\">"));
  auto write = Call(base::DictValue()
                        .Set("operation", "clipboard.writeItems")
                        .Set("items", base::ListValue().Append(
                                          std::move(representations))));
  ASSERT_TRUE(write->success) << write->error;
  auto read = Call(base::DictValue()
                       .Set("operation", "clipboard.readType")
                       .Set("mime", "image/png"));
  ASSERT_TRUE(read->success) << read->error;
  auto copied_png = base::Base64Decode(read->value.GetString());
  ASSERT_TRUE(copied_png);
  SkBitmap copied = gfx::PNGCodec::Decode(*copied_png);
  ASSERT_FALSE(copied.isNull());
  EXPECT_EQ(2, copied.width());
  EXPECT_EQ(1, copied.height());
  EXPECT_EQ(SK_ColorBLUE, copied.getColor(0, 0));
  EXPECT_EQ(SK_ColorBLUE, copied.getColor(1, 0));
  auto html = Call(base::DictValue().Set("operation", "clipboard.readHTML"));
  ASSERT_TRUE(html->success) << html->error;
  EXPECT_EQ("<img width=\"2\" height=\"1\">", html->value.GetString());
  auto has_png = Call(base::DictValue()
                          .Set("operation", "clipboard.has")
                          .Set("mime", "image/png"));
  ASSERT_TRUE(has_png->success) << has_png->error;
  EXPECT_TRUE(has_png->value.GetBool());
}

TEST_F(XenonElectronApiBridgeTest,
       ClipboardInvalidBatchDoesNotOverwritePreviousContents) {
  ASSERT_TRUE(Call(base::DictValue()
                       .Set("operation", "clipboard.writeText")
                       .Set("text", "preserved"))->success);
  base::ListValue invalid_representations;
  invalid_representations.Append(ClipboardRepresentation("image/png", "bad PNG"));
  invalid_representations.Append(
      ClipboardRepresentation("application/x-unsupported", "content"));
  invalid_representations.Append(
      base::DictValue().Set("mime", "text/html").Set("data", "not base64"));
  invalid_representations.Append(base::DictValue().Set("mime", 42));
  for (auto& invalid : invalid_representations) {
    base::ListValue items;
    items.Append(base::ListValue().Append(
        ClipboardRepresentation("text/plain", "must not be written")));
    items.Append(base::ListValue().Append(std::move(invalid)));
    auto failed = Call(base::DictValue()
                           .Set("operation", "clipboard.writeItems")
                           .Set("items", std::move(items)));
    EXPECT_FALSE(failed->success);
    auto previous = Call(base::DictValue().Set("operation", "clipboard.readText"));
    ASSERT_TRUE(previous->success) << previous->error;
    EXPECT_EQ("preserved", previous->value.GetString());
  }
  // An empty write has no representations to commit, matching Electron's
  // ScopedClipboardWriter behavior rather than implicitly clearing the buffer.
  ASSERT_TRUE(Call(base::DictValue()
                       .Set("operation", "clipboard.writeItems")
                       .Set("items", base::ListValue()))->success);
  auto previous = Call(base::DictValue().Set("operation", "clipboard.readText"));
  ASSERT_TRUE(previous->success) << previous->error;
  EXPECT_EQ("preserved", previous->value.GetString());
}

TEST_F(XenonElectronApiBridgeTest,
       ClipboardEmptyReadsAndUnsupportedMimeAreExplicit) {
  auto types = Call(base::DictValue().Set("operation", "clipboard.readTypes"));
  ASSERT_TRUE(types->success) << types->error;
  EXPECT_TRUE(types->value.GetList().empty());
  auto has = Call(base::DictValue()
                      .Set("operation", "clipboard.has")
                      .Set("mime", "text/plain"));
  ASSERT_TRUE(has->success) << has->error;
  EXPECT_FALSE(has->value.GetBool());
  auto missing = Call(base::DictValue().Set("operation", "clipboard.has"));
  EXPECT_FALSE(missing->success);
  EXPECT_TRUE(missing->error.starts_with("ERR_INVALID_ARG_TYPE:"));
  auto unsupported = Call(base::DictValue()
                              .Set("operation", "clipboard.readType")
                              .Set("mime", "application/x-unsupported"));
  EXPECT_FALSE(unsupported->success);
  EXPECT_TRUE(unsupported->error.starts_with("ERR_NOT_SUPPORTED:"));
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

#if BUILDFLAG(IS_MAC)
TEST_F(XenonElectronApiBridgeTest, LoginItemQueryRejectsCallerSelectedPaths) {
  auto invalid = Call(base::DictValue()
                          .Set("operation", "app.getLoginItemSettings")
                          .Set("options", "invalid"));
  EXPECT_FALSE(invalid->success);
  EXPECT_TRUE(invalid->error.starts_with("ERR_INVALID_ARG_TYPE:"));
  auto path = Call(base::DictValue()
                       .Set("operation", "app.getLoginItemSettings")
                       .Set("options", base::DictValue().Set(
                                           "path", "/Applications/Other.app")));
  EXPECT_FALSE(path->success);
  EXPECT_TRUE(path->error.starts_with("ERR_NOT_SUPPORTED:"));
}

TEST_F(XenonElectronApiBridgeTest, LoginItemQueryNeverSucceedsWithoutBundle) {
  if (base::apple::AmIBundled()) {
    GTEST_SKIP() << "This check requires the standalone unbundled test host";
  }
  auto result = Call(base::DictValue().Set("operation", "app.getLoginItemSettings"));
  EXPECT_FALSE(result->success);
  EXPECT_TRUE(result->error.starts_with("ERR_NOT_SUPPORTED:"));
}

TEST_F(XenonElectronApiBridgeTest, LoginItemQueryReadsAbsentItemWithoutRegistering) {
  base::ScopedTempDir directory;
  ASSERT_TRUE(directory.CreateUniqueTempDir());
  const auto path = directory.GetPath().AppendASCII("UnregisteredFixture.app");
  const auto result = base::mac::GetLoginItemSettings(path);
  if (!result) {
    GTEST_SKIP() << "System Login Items list is unavailable in this test session";
  }
  EXPECT_FALSE(result->open_at_login);
  EXPECT_FALSE(result->open_as_hidden);
  EXPECT_FALSE(base::mac::GetLoginItemSettings(base::FilePath()));
  EXPECT_FALSE(base::mac::GetLoginItemSettings(base::FilePath("relative.app")));
}
#else
TEST_F(XenonElectronApiBridgeTest, LoginItemQueryIsExplicitlyUnsupported) {
  auto result = Call(base::DictValue().Set("operation", "app.getLoginItemSettings"));
  EXPECT_FALSE(result->success);
  EXPECT_TRUE(result->error.starts_with("ERR_NOT_SUPPORTED:"));
}
#endif

TEST_F(XenonElectronApiBridgeTest, AutoUpdaterConfigurationUsesNativeState) {
  auto* manager = updater::XenonUpdateManager::GetInstance();
  const bool auto_download = manager->GetAutoDownload();
  const bool install_on_quit = manager->GetAutoInstallOnAppQuit();
  base::ScopedClosureRunner restore(base::BindLambdaForTesting([=] {
    manager->SetAutoDownload(auto_download);
    manager->SetAutoInstallOnAppQuit(install_on_quit);
  }));
  auto configured = Call(base::DictValue()
                             .Set("operation", "autoUpdater.configure")
                             .Set("autoDownload", false)
                             .Set("autoInstallOnAppQuit", false));
  ASSERT_TRUE(configured->success);
  EXPECT_FALSE(manager->GetAutoDownload());
  EXPECT_FALSE(manager->GetAutoInstallOnAppQuit());
  auto invalid = Call(base::DictValue()
                          .Set("operation", "autoUpdater.configure")
                          .Set("autoDownload", true)
                          .Set("autoInstallOnAppQuit", "false"));
  EXPECT_FALSE(invalid->success);
  EXPECT_FALSE(manager->GetAutoDownload());
  manager->SetAutoDownload(true);
  auto read = Call(base::DictValue()
                       .Set("operation", "autoUpdater.getConfiguration"));
  ASSERT_TRUE(read->success);
  EXPECT_EQ(read->value.GetDict().FindBool("autoDownload"), true);
  EXPECT_EQ(read->value.GetDict().FindBool("autoInstallOnAppQuit"), false);
}

TEST_F(XenonElectronApiBridgeTest, AutoUpdaterForwardsNativeErrors) {
  auto* manager = updater::XenonUpdateManager::GetInstance();
  const std::string feed = manager->GetFeedURL();
  base::ScopedClosureRunner restore(base::BindLambdaForTesting([=] {
    SetAutoUpdaterEventCallback({});
    manager->SetFeedURL(feed);
  }));
  base::test::TestFuture<std::string, base::ListValue> event;
  SetAutoUpdaterEventCallback(
      event.GetRepeatingCallback<const std::string&, base::ListValue>());
  manager->SetFeedURL("");
  Call(base::DictValue().Set("operation", "autoUpdater.checkForUpdates"));
  ASSERT_TRUE(event.IsReady());
  EXPECT_EQ(event.Get<0>(), "error");
  ASSERT_EQ(event.Get<1>().size(), 1U);
  EXPECT_EQ(event.Get<1>()[0].GetString(), "No update feed URL configured");
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
