// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license.
#include "xenon_overlay/chrome/browser/ipc/xenon_udp_socket_bridge.h"

#include <memory>
#include <vector>

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/test/scoped_run_loop_timeout.h"
#include "base/test/test_future.h"
#include "base/threading/platform_thread.h"
#include "base/time/time.h"
#include "gin/test/v8_test.h"
#include "mojo/core/embedder/embedder.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_ipc_main_container.h"

namespace xenon::ipc {
namespace {
class XenonUdpSocketBridgeTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(uv_loop_init(&loop_), 0);
    bridge_ = std::make_unique<XenonUdpSocketBridge>(
        &loop_, base::BindRepeating(&XenonUdpSocketBridgeTest::Event,
                                    base::Unretained(this)));
  }
  void TearDown() override {
    bridge_->Shutdown();
    EXPECT_TRUE(PumpUntil([&] { return bridge_->IsDrained(); }));
    bridge_.reset();
    EXPECT_EQ(uv_loop_close(&loop_), 0);
  }
  void Event(base::Value value) { events_.push_back(std::move(value)); }
  template <typename Predicate>
  bool PumpUntil(Predicate done) {
    const auto deadline = base::TimeTicks::Now() + base::Seconds(3);
    while (!done() && base::TimeTicks::Now() < deadline) {
      uv_run(&loop_, UV_RUN_NOWAIT);
      base::PlatformThread::Sleep(base::Milliseconds(1));
    }
    return done();
  }
  int Create(int family = 4) {
    auto result = bridge_->Call(
        base::DictValue().Set("op", "create").Set("family", family));
    EXPECT_TRUE(result.has_value()) << result.error();
    return result.has_value() ? result->GetInt() : 0;
  }
  void Bind(int id, std::string host = "127.0.0.1", int port = 0) {
    auto result = bridge_->Call(base::DictValue()
                                    .Set("op", "bind")
                                    .Set("id", id)
                                    .Set("address", host)
                                    .Set("port", port));
    ASSERT_TRUE(result.has_value()) << result.error();
  }
  int Port(int id) {
    auto result =
        bridge_->Call(base::DictValue().Set("op", "address").Set("id", id));
    EXPECT_TRUE(result.has_value()) << result.error();
    return result.has_value() ? result->GetDict().FindInt("port").value_or(0)
                              : 0;
  }
  const base::DictValue* FindEvent(int id, std::string_view type) {
    for (const auto& event : events_) {
      const auto& dict = event.GetDict();
      if (dict.FindInt("id") == id && dict.FindString("type") &&
          *dict.FindString("type") == type) {
        return &dict;
      }
    }
    return nullptr;
  }
  uv_loop_t loop_;
  std::unique_ptr<XenonUdpSocketBridge> bridge_;
  std::vector<base::Value> events_;
};

TEST_F(XenonUdpSocketBridgeTest,
       LoopbackPreservesDatagramBytesAndSourceAddress) {
  const int receiver = Create(), sender = Create();
  Bind(receiver);
  Bind(sender);
  const base::Value::BlobStorage bytes{0, 255, 31, 128};
  auto result = bridge_->Call(base::DictValue()
                                  .Set("op", "send")
                                  .Set("id", sender)
                                  .Set("address", "127.0.0.1")
                                  .Set("port", Port(receiver))
                                  .Set("sendId", 1)
                                  .Set("bytes", base::Value(bytes)));
  ASSERT_TRUE(result.has_value()) << result.error();
  ASSERT_TRUE(PumpUntil([&] {
    return FindEvent(receiver, "message") && FindEvent(sender, "sent");
  }));
  const auto* message = FindEvent(receiver, "message");
  ASSERT_TRUE(message->FindBlob("bytes"));
  EXPECT_EQ(*message->FindBlob("bytes"), bytes);
  const auto* info = message->FindDict("rinfo");
  ASSERT_TRUE(info);
  EXPECT_EQ(info->FindInt("port"), Port(sender));
  EXPECT_EQ(*info->FindString("address"), "127.0.0.1");
  EXPECT_EQ(*info->FindString("family"), "IPv4");
  EXPECT_EQ(info->FindInt("size"), 4);
  EXPECT_EQ(FindEvent(sender, "sent")->FindInt("bytes"), 4);
}

TEST_F(XenonUdpSocketBridgeTest, EmptyDatagramsAndConnectedPeerAreReal) {
  const int receiver = Create(6), sender = Create(6);
  Bind(receiver, "::1");
  Bind(sender, "::1");
  auto connected = bridge_->Call(base::DictValue()
                                     .Set("op", "connect")
                                     .Set("id", sender)
                                     .Set("address", "::1")
                                     .Set("port", Port(receiver)));
  ASSERT_TRUE(connected.has_value()) << connected.error();
  auto peer = bridge_->Call(
      base::DictValue().Set("op", "remoteAddress").Set("id", sender));
  ASSERT_TRUE(peer.has_value()) << peer.error();
  EXPECT_EQ(peer->GetDict().FindInt("port"), Port(receiver));
  auto sent =
      bridge_->Call(base::DictValue()
                        .Set("op", "send")
                        .Set("id", sender)
                        .Set("sendId", 2)
                        .Set("bytes", base::Value(base::Value::BlobStorage())));
  ASSERT_TRUE(sent.has_value()) << sent.error();
  ASSERT_TRUE(PumpUntil([&] {
    return FindEvent(receiver, "message") && FindEvent(sender, "sent");
  }));
  EXPECT_TRUE(FindEvent(receiver, "message")->FindBlob("bytes")->empty());
  auto disconnected = bridge_->Call(
      base::DictValue().Set("op", "disconnect").Set("id", sender));
  EXPECT_TRUE(disconnected.has_value()) << disconnected.error();
  EXPECT_FALSE(
      bridge_
          ->Call(base::DictValue().Set("op", "remoteAddress").Set("id", sender))
          .has_value());
}

TEST_F(XenonUdpSocketBridgeTest,
       HostnameLookupAndShutdownDrainPendingRequests) {
  const int id = Create();
  auto lookup = bridge_->Call(base::DictValue()
                                  .Set("op", "lookup")
                                  .Set("id", id)
                                  .Set("lookupId", 1)
                                  .Set("hostname", "localhost"));
  ASSERT_TRUE(lookup.has_value()) << lookup.error();
  ASSERT_TRUE(PumpUntil([&] { return FindEvent(id, "lookup"); }));
  ASSERT_FALSE(FindEvent(id, "lookup")->FindString("error"));
  EXPECT_EQ(*FindEvent(id, "lookup")->FindDict("result")->FindString("address"),
            "127.0.0.1");
  lookup = bridge_->Call(base::DictValue()
                             .Set("op", "lookup")
                             .Set("id", id)
                             .Set("lookupId", 2)
                             .Set("hostname", "localhost"));
  ASSERT_TRUE(lookup.has_value()) << lookup.error();
  bridge_->Shutdown();
  ASSERT_TRUE(PumpUntil([&] { return bridge_->IsDrained(); }));
  EXPECT_FALSE(
      bridge_->Call(base::DictValue().Set("op", "create").Set("family", 4))
          .has_value());
}

TEST_F(XenonUdpSocketBridgeTest, CloseReleasesPortAndRejectsOldSocketIdentity) {
  const int id = Create();
  Bind(id);
  const int port = Port(id);
  auto close =
      bridge_->Call(base::DictValue().Set("op", "close").Set("id", id));
  ASSERT_TRUE(close.has_value()) << close.error();
  ASSERT_TRUE(PumpUntil([&] { return FindEvent(id, "close"); }));
  EXPECT_FALSE(
      bridge_->Call(base::DictValue().Set("op", "address").Set("id", id))
          .has_value());
  const int replacement = Create();
  EXPECT_NE(id, replacement);
  Bind(replacement, "127.0.0.1", port);
  EXPECT_EQ(Port(replacement), port);
}

TEST_F(XenonUdpSocketBridgeTest,
       MulticastMembershipAndSocketOptionsReachTheOS) {
  const int id = Create();
  Bind(id, "0.0.0.0");
  auto control = [&](std::string op, base::DictValue options) {
    options.Set("op", op);
    options.Set("id", id);
    auto result = bridge_->Call(options);
    EXPECT_TRUE(result.has_value()) << op << ": " << result.error();
    return result.has_value() ? std::move(*result) : base::Value();
  };
  control("setMulticastInterface",
          base::DictValue().Set("interface", "127.0.0.1"));
  control("setMulticastTTL", base::DictValue().Set("value", 1));
  control("setMulticastLoopback", base::DictValue().Set("value", 1));
  control("addMembership", base::DictValue()
                               .Set("group", "239.255.0.42")
                               .Set("interface", "127.0.0.1"));
  control("dropMembership", base::DictValue()
                                .Set("group", "239.255.0.42")
                                .Set("interface", "127.0.0.1"));
  control("setBroadcast", base::DictValue().Set("value", 1));
  control("setTTL", base::DictValue().Set("value", 64));
  control("setRecvBufferSize", base::DictValue().Set("value", 65536));
  auto size = control("getRecvBufferSize", {});
  ASSERT_TRUE(size.is_int());
  EXPECT_GE(size.GetInt(), 65536);
  control("unref", {});
  control("ref", {});
}

TEST_F(XenonUdpSocketBridgeTest, InvalidOptionsAndRuntimeIdsFailExplicitly) {
  EXPECT_FALSE(
      bridge_->Call(base::DictValue().Set("op", "create").Set("family", 7))
          .has_value());
  const int id = Create();
  EXPECT_FALSE(
      bridge_->Call(base::DictValue().Set("op", "close").Set("id", id + 1))
          .has_value());
  auto invalid = bridge_->Call(base::DictValue()
                                   .Set("op", "bind")
                                   .Set("id", id)
                                   .Set("address", "127.0.0.1")
                                   .Set("port", -1));
  EXPECT_FALSE(invalid.has_value());
  Bind(id);
  EXPECT_FALSE(bridge_
                   ->Call(base::DictValue()
                              .Set("op", "setTTL")
                              .Set("id", id)
                              .Set("value", 300))
                   .has_value());
  EXPECT_FALSE(bridge_
                   ->Call(base::DictValue()
                              .Set("op", "nonexistent")
                              .Set("id", id)
                              .Set("value", 1))
                   .has_value());
}

#if BUILDFLAG(ENABLE_XENON_NODE_UV_COMPAT)
class XenonMainDgramTest : public gin::V8Test, public mojom::IpcRenderer {
 protected:
  static void SetUpTestSuite() { mojo::core::Init(); }
  void Dispatch(const std::string&, base::Value) override {}
  void DispatchPostMessage(const std::string&,
                           blink::TransferableMessage) override {}
  mojo::Receiver<mojom::IpcRenderer> receiver_{this};
};

TEST_F(XenonMainDgramTest,
       BuiltinSocketUsesNativeLoopAndClosesBeforeRuntimeExit) {
  base::test::ScopedRunLoopTimeout timeout(FROM_HERE, base::Seconds(5));
  base::ScopedTempDir directory;
  ASSERT_TRUE(directory.CreateUniqueTempDir());
  base::FilePath canonical;
  ASSERT_TRUE(base::NormalizeFilePath(directory.GetPath(), &canonical));
  XenonIpcMainContainer::EmbeddedMainModule module;
  module.app_path = canonical;
  module.virtual_path = canonical.AppendASCII("udp-main.js");
  module.source = R"JS(
    const dgram = require('node:dgram');
    if (dgram !== require('dgram')) throw Error('builtin identity');
    const {ipcMain} = require('electron');
    ipcMain.handle('test:udp', async () => {
      const receiver = dgram.createSocket('udp4');
      const sender = dgram.createSocket('udp4');
      const close = socket => new Promise(resolve => socket.close(resolve));
      try {
        await new Promise((resolve, reject) => {
          receiver.once('error', reject); receiver.bind(0, '127.0.0.1', resolve);
        });
        await new Promise((resolve, reject) => {
          sender.once('error', reject); sender.bind(0, '127.0.0.1', resolve);
        });
        receiver.unref(); sender.unref();
        const message = new Promise((resolve, reject) => {
          receiver.once('error', reject);
          receiver.once('message', (bytes, info) => resolve({hex: bytes.toString('hex'), info}));
        });
        const sent = new Promise((resolve, reject) => sender.send(
          Buffer.from([0, 255, 128]), receiver.address().port, 'localhost',
          (error, count) => error ? reject(error) : resolve(count)));
        const [received, count] = await Promise.all([message, sent]);
        return received.hex === '00ff80' && count === 3 &&
          received.info.port === sender.address().port && received.info.size === 3;
      } finally { await Promise.all([close(receiver), close(sender)]); }
    });
  )JS";
  XenonIpcMainContainer container;
  ASSERT_TRUE(container.Initialize(std::move(module)))
      << container.startup_error();
  container.AddRenderer("renderer", receiver_.BindNewPipeAndPassRemote(), 1, 1,
                        0, "about:blank", true);
  base::test::TestFuture<mojom::IpcResultPtr> future;
  container.Invoke("renderer", "test:udp", base::Value(base::ListValue()),
                   future.GetCallback());
  auto result = future.Take();
  ASSERT_TRUE(result->success) << result->error;
  ASSERT_TRUE(result->value.is_bool());
  EXPECT_TRUE(result->value.GetBool());
  container.BeginNativeShutdown();
  EXPECT_TRUE(container.PollNativeShutdown())
      << container.DescribeNativeShutdown();
}
#endif
}  // namespace
}  // namespace xenon::ipc
