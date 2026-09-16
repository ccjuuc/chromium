// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/base64.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/test/bind.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "gin/test/v8_test.h"
#include "mojo/core/embedder/embedder.h"
#include "net/http/http_status_code.h"
#include "net/url_request/redirect_info.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/url_loader_completion_status.h"
#include "services/network/public/cpp/weak_wrapper_shared_url_loader_factory.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "services/network/test/test_url_loader_factory.h"
#include "services/network/test/test_utils.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_ipc_main_container.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_network_request_bridge.h"

namespace xenon::ipc {
namespace {

base::Value RequestArguments(const std::string& url) {
  base::DictValue request;
  request.Set("url", url);
  base::ListValue arguments;
  arguments.Append(std::move(request));
  return base::Value(std::move(arguments));
}

class XenonMainNetworkTest : public gin::V8Test {
 protected:
  static void SetUpTestSuite() { mojo::core::Init(); }
  network::TestURLLoaderFactory factory_;
};

TEST_F(XenonMainNetworkTest, HttpErrorStatusAndBinaryUploadAreNotFabricated) {
  const std::string body("\0\xff\x80", 3);
  factory_.AddResponse("https://fixture.test/binary", body,
                       net::HTTP_NOT_FOUND);
  std::string uploaded;
  factory_.SetInterceptor(
      base::BindLambdaForTesting([&](const network::ResourceRequest& request) {
        uploaded = network::GetUploadData(request);
        EXPECT_EQ("POST", request.method);
      }));
  auto arguments = RequestArguments("https://fixture.test/binary");
  auto& request = arguments.GetList()[0].GetDict();
  request.Set("method", "POST");
  request.Set("bodyBase64", base::Base64Encode(body));
  base::test::TestFuture<mojom::IpcResultPtr> future;
  PerformNetworkRequest(factory_.GetSafeWeakWrapper(), std::move(arguments),
                        future.GetCallback());
  auto result = future.Take();
  ASSERT_TRUE(result->success) << result->error;
  EXPECT_EQ(body, uploaded);
  EXPECT_EQ(404, result->value.GetDict().FindInt("statusCode"));
  EXPECT_EQ(base::Base64Encode(body),
            *result->value.GetDict().FindString("bodyBase64"));
}

TEST_F(XenonMainNetworkTest, AbortDisconnectsTheActualLoader) {
  base::test::TestFuture<mojom::IpcResultPtr> future;
  auto cancel = PerformNetworkRequest(
      factory_.GetSafeWeakWrapper(),
      RequestArguments("https://fixture.test/hang"), future.GetCallback());
  ASSERT_TRUE(factory_.IsPending("https://fixture.test/hang"));
  ASSERT_TRUE(cancel);
  std::move(cancel).Run();
  auto result = future.Take();
  EXPECT_FALSE(result->success);
  EXPECT_TRUE(result->error.starts_with("ABORT_ERR:"));
  EXPECT_FALSE(factory_.IsPending("https://fixture.test/hang"));
}

TEST_F(XenonMainNetworkTest, OversizedResponseIsAnError) {
  factory_.AddResponse("https://fixture.test/large",
                       std::string(5 * 1024 * 1024 + 1, 'x'));
  base::test::TestFuture<mojom::IpcResultPtr> future;
  PerformNetworkRequest(factory_.GetSafeWeakWrapper(),
                        RequestArguments("https://fixture.test/large"),
                        future.GetCallback());
  auto result = future.Take();
  EXPECT_FALSE(result->success);
  EXPECT_NE(std::string::npos,
            result->error.find("ERR_INSUFFICIENT_RESOURCES"));
  auto oversized = RequestArguments("https://fixture.test/upload");
  oversized.GetList()[0].GetDict().Set(
      "bodyBase64", std::string(((32 * 1024 * 1024 + 2) / 3) * 4 + 1, 'A'));
  base::test::TestFuture<mojom::IpcResultPtr> upload;
  PerformNetworkRequest(factory_.GetSafeWeakWrapper(), std::move(oversized),
                        upload.GetCallback());
  EXPECT_FALSE(upload.Get()->success);
  EXPECT_TRUE(upload.Get()->error.starts_with("ERR_BUFFER_TOO_LARGE:"));
  EXPECT_EQ(1u, factory_.total_requests());
}

TEST_F(XenonMainNetworkTest, RedirectErrorStopsTheLoader) {
  net::RedirectInfo redirect;
  redirect.status_code = 302;
  redirect.new_method = "GET";
  redirect.new_url = GURL("https://fixture.test/final");
  network::TestURLLoaderFactory::Redirects redirects;
  redirects.emplace_back(redirect,
                         network::CreateURLResponseHead(net::HTTP_FOUND));
  factory_.AddResponse(GURL("https://fixture.test/redirect"),
                       network::CreateURLResponseHead(net::HTTP_OK),
                       "must not arrive", network::URLLoaderCompletionStatus(),
                       std::move(redirects));
  auto arguments = RequestArguments("https://fixture.test/redirect");
  arguments.GetList()[0].GetDict().Set("redirect", "error");
  base::test::TestFuture<mojom::IpcResultPtr> future;
  PerformNetworkRequest(factory_.GetSafeWeakWrapper(), std::move(arguments),
                        future.GetCallback());
  auto result = future.Take();
  EXPECT_FALSE(result->success);
  EXPECT_TRUE(result->error.starts_with("ERR_HTTP_REDIRECT:"));
}

TEST_F(XenonMainNetworkTest, ContainerShutdownCancelsPendingLoader) {
  base::ScopedTempDir directory;
  ASSERT_TRUE(directory.CreateUniqueTempDir());
  base::FilePath canonical;
  ASSERT_TRUE(base::NormalizeFilePath(directory.GetPath(), &canonical));
  XenonIpcMainContainer::EmbeddedMainModule module;
  module.app_path = canonical;
  module.virtual_path = canonical.AppendASCII("main.js");
  module.source = "fetch('https://fixture.test/shutdown').catch(() => {});";
  XenonIpcMainContainer container;
  container.SetNetworkLoaderFactory(factory_.GetSafeWeakWrapper());
  ASSERT_TRUE(container.Initialize(std::move(module)))
      << container.startup_error();
  ASSERT_TRUE(factory_.IsPending("https://fixture.test/shutdown"));
  container.Shutdown();
  EXPECT_FALSE(factory_.IsPending("https://fixture.test/shutdown"));
}

TEST_F(XenonMainNetworkTest, FetchAndHttpUseRealV8AndNetworkLoader) {
  factory_.AddResponse("https://fixture.test/binary",
                       std::string("\0\xff\x80", 3), net::HTTP_NOT_FOUND);
  factory_.AddResponse("http://fixture.test/text", "network response");
  factory_.AddResponse("https://fixture.test/json",
                       "\xef\xbb\xbf{\"ok\":true}");
  base::ScopedTempDir directory;
  ASSERT_TRUE(directory.CreateUniqueTempDir());
  base::FilePath canonical;
  ASSERT_TRUE(base::NormalizeFilePath(directory.GetPath(), &canonical));
  XenonIpcMainContainer::EmbeddedMainModule module;
  module.app_path = canonical;
  module.virtual_path = canonical.AppendASCII("main.js");
  module.source = R"JS(
    const {ipcMain} = require('electron');
    ipcMain.handle('test:network', async () => {
      const request = new Request('https://fixture.test/binary', {headers: {'x-test': 'yes'}});
      if (!(await (await fetch('https://fixture.test/json')).json()).ok) return false;
      const response = await fetch(request);
      if (response.status !== 404 || response.ok ||
          Buffer.from(await response.arrayBuffer()).toString('hex') !== '00ff80') return false;
      const text = await new Promise((resolve, reject) => {
        require('http').get('http://fixture.test/text', response => {
          let text = ''; response.setEncoding('utf8');
          response.on('data', chunk => text += chunk);
          response.on('end', () => resolve(text));
        }).on('error', reject);
      });
      const controller = new AbortController();
      const pending = fetch('https://fixture.test/hang', {signal: controller.signal});
      controller.abort(new Error('caller cancelled'));
      let aborted = false;
      try { await pending; } catch (error) { aborted = error.message === 'caller cancelled'; }
      return text === 'network response' && aborted;
    });
  )JS";
  XenonIpcMainContainer container;
  container.SetNetworkLoaderFactory(factory_.GetSafeWeakWrapper());
  ASSERT_TRUE(container.Initialize(std::move(module)))
      << container.startup_error();
  base::test::TestFuture<mojom::IpcResultPtr> future;
  container.Invoke("renderer", "test:network", base::Value(base::ListValue()),
                   future.GetCallback());
  auto result = future.Take();
  ASSERT_TRUE(result->success) << result->error;
  ASSERT_TRUE(result->value.is_bool());
  EXPECT_TRUE(result->value.GetBool());
}
}  // namespace
}  // namespace xenon::ipc
