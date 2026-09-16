// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_zlib_bridge.h"

#include <memory>
#include <string>
#include <tuple>
#include <utility>

#include "base/base64.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/test/test_future.h"
#include "gin/test/v8_test.h"
#include "mojo/core/embedder/embedder.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_ipc_main_container.h"

namespace xenon::ipc {
namespace {

TEST(XenonZlibBridgeTest, DecodesNodeReferenceVectorsAndCompressesRealBytes) {
  const base::Value::BlobStorage input = {0, 255, 128, 65, 66, 0, 255};
  for (const auto& [compress, decompress, reference] :
       {std::tuple{"gzip", "gunzip", "H4sIAAAAAAAACmP43+DoxPAfAFtN+rUHAAAA"},
        std::tuple{"deflate", "inflate", "eJxj+N/g6MTwHwALSgMC"},
        std::tuple{"deflateRaw", "inflateRaw", "Y/jf4OjE8B8A"}}) {
    std::string encoded;
    ASSERT_TRUE(base::Base64Decode(reference, &encoded));
    auto decoded =
        PerformZlibCall(decompress, {encoded.begin(), encoded.end()}, {});
    ASSERT_TRUE(decoded->success) << decoded->error;
    EXPECT_EQ(input, decoded->value.GetBlob());
    auto compressed = PerformZlibCall(compress, input, {});
    ASSERT_TRUE(compressed->success) << compressed->error;
    EXPECT_NE(input, compressed->value.GetBlob());
    auto round_trip =
        PerformZlibCall(decompress, compressed->value.GetBlob(), {});
    ASSERT_TRUE(round_trip->success) << round_trip->error;
    EXPECT_EQ(input, round_trip->value.GetBlob());
  }
}

TEST(XenonZlibBridgeTest, MultiChunkAndConcatenatedGzipHaveBoundedRealOutput) {
  const base::Value::BlobStorage input(100000, 65);
  auto compressed = PerformZlibCall("gzip", input, {});
  ASSERT_TRUE(compressed->success);
  auto members = compressed->value.GetBlob();
  members.insert(members.end(), compressed->value.GetBlob().begin(),
                 compressed->value.GetBlob().end());
  for (const auto* operation : {"gunzip", "unzip"}) {
    auto decoded = PerformZlibCall(operation, members, {});
    ASSERT_TRUE(decoded->success) << decoded->error;
    EXPECT_EQ(200000u, decoded->value.GetBlob().size());
    base::DictValue options;
    options.Set("maxOutputLength", 99999);
    auto limited = PerformZlibCall(operation, members, std::move(options));
    EXPECT_FALSE(limited->success);
    EXPECT_TRUE(limited->error.starts_with("ERR_BUFFER_TOO_LARGE:"));
  }
}

TEST(XenonZlibBridgeTest, InvalidDataAndUnsupportedOptionsFailExplicitly) {
  for (const auto* operation : {"gunzip", "inflate", "inflateRaw", "unzip"}) {
    auto corrupt = PerformZlibCall(operation, {1, 2, 3, 4}, {});
    EXPECT_FALSE(corrupt->success);
    auto empty = PerformZlibCall(operation, {}, {});
    EXPECT_FALSE(empty->success);
  }
  base::DictValue options;
  options.Set("dictionary", "unsupported");
  EXPECT_FALSE(PerformZlibCall("deflate", {1}, std::move(options))->success);
  EXPECT_FALSE(PerformZlibCall("made-up", {1}, {})->success);
  auto gzip = PerformZlibCall("gzip", {}, {});
  ASSERT_TRUE(gzip->success);
  auto empty = PerformZlibCall("gunzip", gzip->value.GetBlob(), {});
  ASSERT_TRUE(empty->success);
  EXPECT_TRUE(empty->value.GetBlob().empty());
}

class XenonZlibContainerTest : public gin::V8Test {
 protected:
  static void SetUpTestSuite() { mojo::core::Init(); }
};

TEST_F(XenonZlibContainerTest, SyncAndWorkerCallbacksUseRealNativeCompression) {
  base::ScopedTempDir directory;
  ASSERT_TRUE(directory.CreateUniqueTempDir());
  base::FilePath canonical;
  ASSERT_TRUE(base::NormalizeFilePath(directory.GetPath(), &canonical));
  XenonIpcMainContainer::EmbeddedMainModule module;
  module.app_path = canonical;
  module.virtual_path = canonical.AppendASCII("main.js");
  module.source = R"JS(
    const {ipcMain} = require('electron');
    const zlib = require('zlib');
    ipcMain.handle('test:zlib', async () => {
      const original = Buffer.from([0, 255, 128, 65, 66, 0, 255]);
      for (const [compress, decompress] of [['gzip', 'gunzip'], ['deflate', 'inflate'],
          ['deflateRaw', 'inflateRaw']]) {
        const packed = zlib[compress + 'Sync'](original, {level: 9});
        if (!Buffer.isBuffer(packed) ||
            zlib[decompress + 'Sync'](packed).toString('hex') !== original.toString('hex')) return false;
      }
      let returned = false;
      const input = Buffer.from(original);
      const pending = new Promise((resolve, reject) => {
        zlib.gzip(input, (error, packed) => {
          if (error) { reject(error); return; }
          resolve(returned && zlib.gunzipSync(packed).toString('hex') === original.toString('hex'));
        });
      });
      input.fill(0);
      returned = true;
      if (!await pending) return false;
      const failure = await new Promise(resolve => zlib.gunzip('bad', error => resolve(error.code)));
      return failure === 'Z_DATA_ERROR' && require('node:zlib') === zlib;
    });
  )JS";
  XenonIpcMainContainer container;
  ASSERT_TRUE(container.Initialize(std::move(module)))
      << container.startup_error();
  base::test::TestFuture<mojom::IpcResultPtr> future;
  container.Invoke("renderer", "test:zlib", base::Value(base::ListValue()),
                   future.GetCallback());
  auto result = future.Take();
  ASSERT_TRUE(result->success) << result->error;
  ASSERT_TRUE(result->value.is_bool());
  EXPECT_TRUE(result->value.GetBool());
}
}  // namespace
}  // namespace xenon::ipc
