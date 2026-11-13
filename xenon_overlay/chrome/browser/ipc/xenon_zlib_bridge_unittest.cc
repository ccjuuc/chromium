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
#include "mojo/public/cpp/bindings/receiver.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_ipc_main_container.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_zlib_stream.h"

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

class XenonZlibContainerTest : public gin::V8Test, public mojom::IpcRenderer {
 protected:
  static void SetUpTestSuite() { mojo::core::Init(); }

  void TearDown() override {
    renderer_receiver_.reset();
    gin::V8Test::TearDown();
  }

  void Dispatch(const std::string& channel, base::Value arguments) override {}
  void DispatchPostMessage(const std::string& channel,
                           blink::TransferableMessage message) override {
    ADD_FAILURE() << "Unexpected MessagePort IPC: " << channel;
  }

  mojo::Receiver<mojom::IpcRenderer> renderer_receiver_{this};
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
      if (failure !== 'Z_DATA_ERROR' || require('node:zlib') !== zlib) return false;
      const dictionary = Buffer.from('shared compression dictionary');
      const withDictionary = zlib.deflateSync('shared compression dictionary payload', {dictionary});
      if (zlib.inflateSync(withDictionary, {dictionary}).toString() !==
          'shared compression dictionary payload') return false;
      for (const [compress, decompress] of [['Gzip', 'Gunzip'], ['Deflate', 'Inflate'],
          ['DeflateRaw', 'InflateRaw'], ['BrotliCompress', 'BrotliDecompress'],
          ['ZstdCompress', 'ZstdDecompress']]) {
        const decoded = await new Promise((resolve, reject) => {
          const encoder = zlib['create' + compress]({chunkSize: 1024});
          const decoder = zlib['create' + decompress]({chunkSize: 2048});
          const chunks = [];
          encoder.on('error', reject);
          decoder.on('error', reject);
          decoder.on('data', data => chunks.push(data));
          decoder.on('end', () => resolve(Buffer.concat(chunks)));
          encoder.pipe(decoder);
          encoder.write(Buffer.alloc(70000, 65));
          encoder.end(Buffer.alloc(80000, 66));
        });
        if (decoded.length !== 150000 || decoded[0] !== 65 || decoded[69999] !== 65 ||
            decoded[70000] !== 66 || decoded[149999] !== 66) return false;
      }
      return true;
    });
  )JS";
  XenonIpcMainContainer container;
  ASSERT_TRUE(container.Initialize(std::move(module)))
      << container.startup_error();
  base::test::TestFuture<mojom::IpcResultPtr> future;
  container.AddRenderer("renderer",
                        renderer_receiver_.BindNewPipeAndPassRemote(), 1, 1, 0,
                        "about:blank", true);
  container.Invoke("renderer", "test:zlib", base::Value(base::ListValue()),
                   future.GetCallback());
  auto result = future.Take();
  ASSERT_TRUE(result->success) << result->error;
  ASSERT_TRUE(result->value.is_bool());
  EXPECT_TRUE(result->value.GetBool());
}
}  // namespace
}  // namespace xenon::ipc

// Streaming tests use deliberately small output buffers and split every input
// frame. This exercises actual codec state rather than convenience collection.
namespace xenon::ipc {
namespace {
using Bytes = base::Value::BlobStorage;
mojom::IpcResultPtr PumpCodec(scoped_refptr<ZlibStream> stream,
                              Bytes input,
                              int flush,
                              int capacity = 127) {
  Bytes output;
  size_t offset = 0;
  for (size_t attempt = 0; attempt < 100000; ++attempt) {
    if (!stream->TryBegin()) {
      auto failure = mojom::IpcResult::New();
      failure->error = "busy";
      return failure;
    }
    auto step = stream->Process(Bytes(input.begin() + offset, input.end()),
                                flush, capacity);
    if (!step->success) {
      return step;
    }
    const auto& value = step->value.GetDict();
    const auto& bytes = *value.FindBlob("output");
    EXPECT_LE(bytes.size(), static_cast<size_t>(capacity));
    offset += *value.FindInt("consumed");
    output.insert(output.end(), bytes.begin(), bytes.end());
    if (value.FindBool("done").value_or(false)) {
      EXPECT_EQ(offset, input.size());
      step->value = base::Value(std::move(output));
      return step;
    }
    EXPECT_TRUE(!bytes.empty() || *value.FindInt("consumed"));
  }
  ADD_FAILURE() << "Codec did not terminate";
  return mojom::IpcResult::New();
}

TEST(XenonZlibStreamTest, EveryCodecRetainsStateAcrossWritesAndBoundedReads) {
  for (const auto& [compress, decompress, finish] :
       {std::tuple{"gzip", "gunzip", 4}, std::tuple{"deflate", "inflate", 4},
        std::tuple{"deflateRaw", "inflateRaw", 4},
        std::tuple{"brotliCompress", "brotliDecompress", 2},
        std::tuple{"zstdCompress", "zstdDecompress", 2}}) {
    SCOPED_TRACE(compress);
    std::string error;
    auto encoder = ZlibStream::Create(compress, {}, &error);
    ASSERT_TRUE(encoder) << error;
    Bytes source, encoded;
    for (int piece = 0; piece < 5; ++piece) {
      Bytes input(32000, static_cast<uint8_t>(65 + piece));
      source.insert(source.end(), input.begin(), input.end());
      auto step = PumpCodec(encoder, std::move(input), 0);
      ASSERT_TRUE(step->success) << step->error;
      const auto& output = step->value.GetBlob();
      encoded.insert(encoded.end(), output.begin(), output.end());
    }
    auto final = PumpCodec(encoder, {}, finish);
    ASSERT_TRUE(final->success) << final->error;
    encoded.insert(encoded.end(), final->value.GetBlob().begin(),
                   final->value.GetBlob().end());
    auto decoder = ZlibStream::Create(decompress, {}, &error);
    ASSERT_TRUE(decoder) << error;
    Bytes decoded;
    for (size_t offset = 0; offset < encoded.size(); ++offset) {
      auto step = PumpCodec(decoder, {encoded[offset]}, 0);
      ASSERT_TRUE(step->success) << step->error;
      const auto& output = step->value.GetBlob();
      decoded.insert(decoded.end(), output.begin(), output.end());
    }
    auto end = PumpCodec(decoder, {}, finish);
    ASSERT_TRUE(end->success) << end->error;
    decoded.insert(decoded.end(), end->value.GetBlob().begin(),
                   end->value.GetBlob().end());
    EXPECT_EQ(decoded, source);
  }
}

TEST(XenonZlibStreamTest, FlushProducesOutputBeforeStreamEnd) {
  for (const auto& [compress, decompress, flush, finish] :
       {std::tuple{"gzip", "gunzip", 2, 4},
        std::tuple{"brotliCompress", "brotliDecompress", 1, 2},
        std::tuple{"zstdCompress", "zstdDecompress", 1, 2}}) {
    SCOPED_TRACE(compress);
    std::string error;
    auto encoder = ZlibStream::Create(compress, {}, &error);
    auto decoder = ZlibStream::Create(decompress, {}, &error);
    ASSERT_TRUE(encoder);
    ASSERT_TRUE(decoder);
    const Bytes input(4096, 73);
    auto first = PumpCodec(encoder, input, flush);
    ASSERT_TRUE(first->success) << first->error;
    ASSERT_FALSE(first->value.GetBlob().empty());
    auto visible = PumpCodec(decoder, first->value.GetBlob(), 0);
    ASSERT_TRUE(visible->success) << visible->error;
    EXPECT_EQ(visible->value.GetBlob(), input);
    auto final = PumpCodec(encoder, {}, finish);
    ASSERT_TRUE(final->success) << final->error;
    auto end = PumpCodec(decoder, final->value.GetBlob(), finish);
    ASSERT_TRUE(end->success) << end->error;
  }
}

TEST(XenonZlibStreamTest, SplitAndConcatenatedFramesDecodeWithoutLosingBytes) {
  for (const auto& [compress, decompress, finish] :
       {std::tuple{"gzip", "gunzip", 4}, std::tuple{"gzip", "unzip", 4},
        std::tuple{"zstdCompress", "zstdDecompress", 2}}) {
    std::string error;
    auto packed = PerformZlibCall(compress, Bytes(20000, 12), {});
    ASSERT_TRUE(packed->success) << packed->error;
    auto decoder = ZlibStream::Create(decompress, {}, &error);
    ASSERT_TRUE(decoder);
    Bytes output;
    for (int member = 0; member < 2; ++member) {
      for (uint8_t byte : packed->value.GetBlob()) {
        auto chunk = PumpCodec(decoder, {byte}, 0);
        ASSERT_TRUE(chunk->success) << chunk->error;
        output.insert(output.end(), chunk->value.GetBlob().begin(),
                      chunk->value.GetBlob().end());
      }
    }
    auto final = PumpCodec(decoder, {}, finish);
    ASSERT_TRUE(final->success) << final->error;
    EXPECT_EQ(output, Bytes(40000, 12));
  }
}

TEST(XenonZlibStreamTest, TruncationInvalidParametersAndCancellationAreErrors) {
  for (const auto& [compress, decompress, finish] :
       {std::tuple{"gzip", "gunzip", 4},
        std::tuple{"brotliCompress", "brotliDecompress", 2},
        std::tuple{"zstdCompress", "zstdDecompress", 2}}) {
    std::string error;
    auto packed = PerformZlibCall(compress, Bytes(400, 17), {});
    ASSERT_TRUE(packed->success);
    packed->value.GetBlob().pop_back();
    auto decoder = ZlibStream::Create(decompress, {}, &error);
    auto result = PumpCodec(decoder, packed->value.GetBlob(), finish);
    EXPECT_FALSE(result->success);
    EXPECT_TRUE(result->error.starts_with("Z_BUF_ERROR:")) << result->error;
    auto cancel = ZlibStream::Create(compress, {}, &error);
    ASSERT_TRUE(cancel->TryBegin());
    EXPECT_FALSE(cancel->TryBegin());
    cancel->Cancel();
    auto canceled = cancel->Process({}, 0, 128);
    EXPECT_FALSE(canceled->success);
    EXPECT_TRUE(canceled->error.starts_with("ERR_STREAM_DESTROYED:"));
    EXPECT_FALSE(cancel->TryBegin());
  }
  std::string error;
  EXPECT_FALSE(ZlibStream::Create(
      "zstdCompress",
      base::DictValue().Set("params", base::DictValue().Set("1006", 1)),
      &error));
  EXPECT_TRUE(error.starts_with("ERR_NOT_SUPPORTED:"));
  EXPECT_FALSE(ZlibStream::Create(
      "zstdDecompress",
      base::DictValue().Set("params", base::DictValue().Set("100", 30)),
      &error));
}

TEST(XenonZlibStreamTest, DictionariesAndExtendedConvenienceOutputLimits) {
  const Bytes dictionary{65, 66, 67, 68, 69, 70};
  const Bytes input(80000, 65);
  for (const auto& [compress, decompress] :
       {std::tuple{"deflate", "inflate"},
        std::tuple{"deflateRaw", "inflateRaw"}}) {
    auto packed =
        PerformZlibCall(compress, input,
                        base::DictValue().Set("dictionary", Bytes(dictionary)));
    ASSERT_TRUE(packed->success) << packed->error;
    auto decoded =
        PerformZlibCall(decompress, packed->value.GetBlob(),
                        base::DictValue().Set("dictionary", Bytes(dictionary)));
    ASSERT_TRUE(decoded->success) << decoded->error;
    EXPECT_EQ(decoded->value.GetBlob(), input);
  }
  for (const auto& [compress, decompress] :
       {std::tuple{"brotliCompress", "brotliDecompress"},
        std::tuple{"zstdCompress", "zstdDecompress"}}) {
    auto packed = PerformZlibCall(compress, input, {});
    ASSERT_TRUE(packed->success) << packed->error;
    auto limited =
        PerformZlibCall(decompress, packed->value.GetBlob(),
                        base::DictValue().Set("maxOutputLength", 79999));
    EXPECT_FALSE(limited->success);
    EXPECT_TRUE(limited->error.starts_with("ERR_BUFFER_TOO_LARGE:"));
    auto decoded = PerformZlibCall(decompress, packed->value.GetBlob(), {});
    ASSERT_TRUE(decoded->success) << decoded->error;
    EXPECT_EQ(decoded->value.GetBlob(), input);
  }
}
}  // namespace
}  // namespace xenon::ipc

namespace xenon::ipc {
namespace {
TEST_F(XenonZlibContainerTest,
       DocumentOwnershipAndTeardownReleaseCodecHandles) {
  base::ScopedTempDir directory;
  ASSERT_TRUE(directory.CreateUniqueTempDir());
  base::FilePath canonical;
  ASSERT_TRUE(base::NormalizeFilePath(directory.GetPath(), &canonical));
  XenonIpcMainContainer::EmbeddedMainModule module;
  module.app_path = canonical;
  module.virtual_path = canonical.AppendASCII("main.js");
  module.source = R"JS(
    const {ipcMain} = require('electron');
    let id;
    ipcMain.handle('create', () => {
      id = __xenonZlibStreamCall({action: 'create', operation: 'gzip', options: {}}, false, 'renderer');
      try {
        __xenonZlibStreamCall({action: 'process', id, data: Buffer.from('x'), flush: 0, outputSize: 64}, false, '');
        return false;
      } catch (error) { return error.message.includes('ERR_STREAM_DESTROYED'); }
    });
    ipcMain.handle('removed', () => {
      try {
        __xenonZlibStreamCall({action: 'process', id, data: Buffer.from('x'), flush: 0, outputSize: 64}, false, 'renderer');
        return false;
      } catch (error) { return error.message.includes('ERR_STREAM_DESTROYED'); }
    });
  )JS";
  XenonIpcMainContainer container;
  ASSERT_TRUE(container.Initialize(std::move(module)))
      << container.startup_error();
  container.AddRenderer("renderer",
                        renderer_receiver_.BindNewPipeAndPassRemote(), 1, 1, 0,
                        "about:blank", true);
  base::test::TestFuture<mojom::IpcResultPtr> created;
  container.Invoke("renderer", "create", base::Value(base::ListValue()),
                   created.GetCallback());
  auto result = created.Take();
  ASSERT_TRUE(result->success) << result->error;
  EXPECT_TRUE(result->value.GetBool());
  container.RemoveRenderer("renderer");
  renderer_receiver_.reset();
  container.AddRenderer("renderer",
                        renderer_receiver_.BindNewPipeAndPassRemote(), 1, 1, 0,
                        "about:blank", true);
  base::test::TestFuture<mojom::IpcResultPtr> removed;
  container.Invoke("renderer", "removed", base::Value(base::ListValue()),
                   removed.GetCallback());
  result = removed.Take();
  ASSERT_TRUE(result->success) << result->error;
  EXPECT_TRUE(result->value.GetBool());
}
}  // namespace
}  // namespace xenon::ipc
