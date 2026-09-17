// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/services/xenon_net_pipe_bridge.h"

#include <string>
#include <vector>

#include "base/base64.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/test/bind.h"
#include "base/test/run_until.h"
#include "base/test/task_environment.h"
#include "base/uuid.h"
#include "build/build_config.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace xenon {
namespace {

class XenonNetTcpTest : public testing::Test {
 protected:
  struct Event {
    std::string channel;
    base::Value payload;
  };
  XenonNetTcpTest()
      : bridge_(base::BindRepeating(&XenonNetTcpTest::OnEvent,
                                    base::Unretained(this))) {}

  void OnEvent(const std::string&,
               const std::string&,
               const std::string& channel,
               base::Value payload) {
    events_.push_back({channel, payload.Clone()});
    if (event_hook_) {
      event_hook_.Run(channel, payload.GetDict());
    }
  }

  const base::DictValue* Find(const std::string& channel,
                              const std::string& to_id = "") const {
    for (const auto& event : events_) {
      const auto& payload = event.payload.GetDict();
      const auto* id = payload.FindString("toId");
      if (event.channel == "__xenon:net:" + channel &&
          (to_id.empty() || (id && *id == to_id))) {
        return &payload;
      }
    }
    return nullptr;
  }

  std::string Bytes(const std::string& id) const {
    std::string result;
    for (const auto& event : events_) {
      const auto& payload = event.payload.GetDict();
      const auto* target = payload.FindString("toId");
      if (event.channel != "__xenon:net:data" || !target || *target != id) {
        continue;
      }
      const auto* wire = payload.FindDict("wire");
      std::string decoded;
      EXPECT_TRUE(wire && base::Base64Decode(*wire->FindString("d"), &decoded));
      result += decoded;
    }
    return result;
  }

  int Listen(const std::string& host = "127.0.0.1") {
    std::string error;
    EXPECT_TRUE(
        bridge_.ListenTcp("container", "server", "listener", host, 0, &error))
        << error;
    auto address = bridge_.ServerAddress("listener");
    return address.is_dict() ? address.GetDict().FindInt("port").value_or(0)
                             : 0;
  }

  std::string PipePath(const std::string& name) {
    if (!pipe_directory_.IsValid() && !pipe_directory_.CreateUniqueTempDir()) {
      return "";
    }
#if BUILDFLAG(IS_WIN)
    return "\\\\.\\pipe\\xenon-" + name + "-" +
           base::Uuid::GenerateRandomV4().AsLowercaseString();
#else
    return pipe_directory_.GetPath().AppendASCII(name).AsUTF8Unsafe();
#endif
  }

  base::test::TaskEnvironment task_environment_;
  base::ScopedTempDir pipe_directory_;
  std::vector<Event> events_;
  base::RepeatingCallback<void(const std::string&, const base::DictValue&)>
      event_hook_;
  XenonNetPipeBridge bridge_;
};

TEST_F(XenonNetTcpTest, PipePermissionsSupportReadWriteAndCombinedModes) {
  const int permission_flags[] = {UV_READABLE, UV_WRITABLE,
                                  UV_READABLE | UV_WRITABLE};
  for (const int flags : permission_flags) {
    SCOPED_TRACE(flags);
    events_.clear();
    const std::string path = PipePath("permissions-" + std::to_string(flags));
    ASSERT_FALSE(path.empty());
    std::string error;
    ASSERT_TRUE(bridge_.Listen("container", "server", "listener", path, &error,
                              flags))
        << error;
    ASSERT_TRUE(bridge_.HasListenerForPath(path));
    ASSERT_TRUE(bridge_.Connect("container", "client", "client-js", path,
                               &error))
        << error;
    ASSERT_TRUE(base::test::RunUntil(
        [&] { return Find("connected", "client-js") && Find("connection"); }));
    const std::string client = *Find("connected")->FindString("peerId");
    const std::string server = *Find("connection")->FindString("socketId");
    ASSERT_TRUE(bridge_.Write(
        client, base::DictValue().Set("t", "s").Set("d", "permission fixture"),
        &error));
    ASSERT_TRUE(base::test::RunUntil(
        [&] { return Bytes(server) == "permission fixture"; }));
    EXPECT_FALSE(bridge_.CloseServer("listener", "container", "other-owner"));
    ASSERT_TRUE(bridge_.CloseServer("listener", "container", "server"));
    ASSERT_TRUE(base::test::RunUntil([&] {
      return !bridge_.HasServer("listener") && !bridge_.HasSocket(client);
    }));
    EXPECT_FALSE(Find("error"));
  }
}

TEST_F(XenonNetTcpTest, PipePermissionFailureDoesNotRegisterOrRetainTheBind) {
  const std::string path = PipePath("permission-failure");
  ASSERT_FALSE(path.empty());
  std::string error;
  EXPECT_FALSE(bridge_.Listen("container", "server", "listener", path, &error,
                              UV_READABLE | UV_WRITABLE | 0x8000));
  EXPECT_EQ(error, "EINVAL");
  EXPECT_FALSE(bridge_.HasServer("listener"));
  EXPECT_FALSE(bridge_.HasListenerForPath(path));
  EXPECT_TRUE(events_.empty());
  error.clear();
  EXPECT_TRUE(bridge_.Listen("container", "server", "listener", path, &error,
                             UV_READABLE | UV_WRITABLE))
      << error;
}

TEST_F(XenonNetTcpTest, EphemeralPortIsReallyBoundAndCanBeReboundAfterClose) {
  const int port = Listen();
  ASSERT_GT(port, 0);
  const auto address = bridge_.ServerAddress("listener");
  EXPECT_EQ(*address.GetDict().FindString("address"), "127.0.0.1");
  EXPECT_EQ(*address.GetDict().FindString("family"), "IPv4");
  std::string error;
  EXPECT_FALSE(bridge_.ListenTcp("container", "server", "duplicate",
                                 "127.0.0.1", port, &error));
  EXPECT_EQ(error, "EADDRINUSE");
  EXPECT_TRUE(bridge_.CloseServer("listener"));
  ASSERT_TRUE(base::test::RunUntil([&] { return Find("server-closed"); }));
  EXPECT_TRUE(bridge_.ListenTcp("container", "server", "replacement",
                                "127.0.0.1", port, &error))
      << error;
}

TEST_F(XenonNetTcpTest, EndFlushesBinaryWritesAndKeepsReadingTheReply) {
  const int port = Listen();
  ASSERT_GT(port, 0);
  std::string error;
  ASSERT_TRUE(bridge_.ConnectTcp("container", "client", "client-js",
                                 "127.0.0.1", port, &error))
      << error;
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return Find("connected", "client-js") && Find("connection"); }));
  const std::string client = *Find("connected")->FindString("peerId");
  const std::string server = *Find("connection")->FindString("socketId");
  EXPECT_TRUE(bridge_.OwnsSocket(client, "container", "client"));
  EXPECT_FALSE(bridge_.OwnsSocket(client, "container", "server"));
  EXPECT_FALSE(bridge_.OwnsSocket(client, "other-container", "client"));
  ASSERT_TRUE(Find("connection")->FindDict("remote"));
  ASSERT_TRUE(Find("connected")->FindDict("local"));
  // Stop accepting while the established connection is still in use.
  ASSERT_TRUE(bridge_.CloseServer("listener"));
  EXPECT_FALSE(Find("server-closed"));
  std::string expected(1024 * 1024, '\0');
  for (size_t i = 0; i < expected.size(); ++i) {
    expected[i] = static_cast<char>(i % 256);
  }
  auto wire =
      base::DictValue().Set("t", "b64").Set("d", base::Base64Encode(expected));
  ASSERT_TRUE(bridge_.Write(client, wire, &error, 7));
  ASSERT_TRUE(error.empty()) << error;
  ASSERT_TRUE(bridge_.EndSocket(client));
  ASSERT_TRUE(base::test::RunUntil([&] { return Find("end", server); }));
  EXPECT_EQ(Bytes(server), expected);
  ASSERT_TRUE(Find("written", "client-js"));
  EXPECT_EQ(Find("written", "client-js")->FindInt("writeId"), 7);
  EXPECT_TRUE(Find("finish", "client-js"));
  EXPECT_FALSE(Find("close", "client-js"));
  auto response = base::DictValue().Set("t", "s").Set("d", "reply after EOF");
  ASSERT_TRUE(bridge_.Write(server, response, &error, 8));
  ASSERT_TRUE(bridge_.EndSocket(server));
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return Find("close", "client-js") && Find("server-closed"); }));
  EXPECT_EQ(Bytes("client-js"), "reply after EOF");
  EXPECT_FALSE(Find("error"));
  std::vector<std::string> completion_order;
  for (const auto& event : events_) {
    const std::string* target = event.payload.GetDict().FindString("toId");
    if (target && *target == "client-js" &&
        (event.channel == "__xenon:net:written" ||
         event.channel == "__xenon:net:finish" ||
         event.channel == "__xenon:net:close")) {
      completion_order.push_back(event.channel);
    }
  }
  EXPECT_EQ(completion_order, (std::vector<std::string>{"__xenon:net:written",
                                                        "__xenon:net:finish",
                                                        "__xenon:net:close"}));
}

TEST_F(XenonNetTcpTest, WriteCompletionCanRemoveItsEndpoint) {
  const int port = Listen();
  ASSERT_GT(port, 0);
  std::string error;
  ASSERT_TRUE(bridge_.ConnectTcp("container", "client", "client-js",
                                 "127.0.0.1", port, &error));
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return Find("connected", "client-js") && Find("connection"); }));
  const std::string client = *Find("connected")->FindString("peerId");
  int completions = 0;
  event_hook_ = base::BindLambdaForTesting([&](const std::string& channel,
                                               const base::DictValue& payload) {
    if (channel == "__xenon:net:written" && payload.FindInt("writeId") == 44) {
      ++completions;
      bridge_.RemoveEndpoint("container", "client");
    }
  });
  ASSERT_TRUE(bridge_.Write(
      client, base::DictValue().Set("t", "s").Set("d", "completion teardown"),
      &error, 44));
  ASSERT_TRUE(error.empty()) << error;
  ASSERT_TRUE(base::test::RunUntil([&] { return completions != 0; }));
  EXPECT_EQ(completions, 1);
  EXPECT_FALSE(bridge_.HasSocket(client));
  EXPECT_FALSE(Find("error"));
  EXPECT_FALSE(Find("close", "client-js"));
}

TEST_F(XenonNetTcpTest, PausedSocketDefersBytesAndEofUntilResume) {
  const int port = Listen();
  ASSERT_GT(port, 0);
  std::string error;
  ASSERT_TRUE(bridge_.ConnectTcp("container", "client", "client-js",
                                 "127.0.0.1", port, &error));
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return Find("connected", "client-js") && Find("connection"); }));
  const std::string client = *Find("connected")->FindString("peerId");
  const std::string server = *Find("connection")->FindString("socketId");
  ASSERT_TRUE(bridge_.SetSocketReadPaused(server, true));
  const std::string expected(64 * 1024, 'a');
  const auto wire =
      base::DictValue().Set("t", "b64").Set("d", base::Base64Encode(expected));
  ASSERT_TRUE(bridge_.Write(client, wire, &error, 1));
  ASSERT_TRUE(bridge_.EndSocket(client));
  ASSERT_TRUE(
      base::test::RunUntil([&] { return Find("written", "client-js"); }));
  EXPECT_TRUE(Bytes(server).empty());
  EXPECT_FALSE(Find("end", server));
  ASSERT_TRUE(bridge_.SetSocketReadPaused(server, false));
  ASSERT_TRUE(base::test::RunUntil([&] { return Find("end", server); }));
  EXPECT_EQ(Bytes(server), expected);
  EXPECT_FALSE(Find("error"));
}

TEST_F(XenonNetTcpTest, EndpointRemovalReleasesTcpListener) {
  const int port = Listen();
  ASSERT_GT(port, 0);
  bridge_.RemoveEndpoint("container", "server");
  ASSERT_TRUE(
      base::test::RunUntil([&] { return !bridge_.HasServer("listener"); }));
  std::string error;
  EXPECT_TRUE(
      bridge_.ListenTcp("other", "server", "next", "127.0.0.1", port, &error))
      << error;
}

TEST_F(XenonNetTcpTest, ReentrantPauseResumeDuringConnectionKeepsReading) {
  const int port = Listen();
  ASSERT_GT(port, 0);
  int callbacks = 0;
  event_hook_ = base::BindLambdaForTesting(
      [&](const std::string& channel, const base::DictValue& payload) {
        const auto* id = channel == "__xenon:net:connection"
                             ? payload.FindString("socketId")
                             : channel == "__xenon:net:connected"
                                   ? payload.FindString("peerId")
                                   : nullptr;
        if (!id) {
          return;
        }
        ++callbacks;
        EXPECT_TRUE(bridge_.SetSocketReadPaused(*id, true));
        EXPECT_TRUE(bridge_.SetSocketReadPaused(*id, false));
      });
  std::string error;
  ASSERT_TRUE(bridge_.ConnectTcp("container", "client", "client-js",
                                 "127.0.0.1", port, &error));
  ASSERT_TRUE(base::test::RunUntil([&] { return callbacks == 2; }));
  const std::string client = *Find("connected")->FindString("peerId");
  const std::string server = *Find("connection")->FindString("socketId");
  ASSERT_TRUE(bridge_.HasSocket(client));
  ASSERT_TRUE(bridge_.HasSocket(server));
  ASSERT_TRUE(bridge_.Write(client,
                            base::DictValue().Set("t", "s").Set("d", "after-resume"),
                            &error));
  ASSERT_TRUE(bridge_.EndSocket(client));
  ASSERT_TRUE(base::test::RunUntil([&] { return Find("end", server); }));
  EXPECT_EQ("after-resume", Bytes(server));
  EXPECT_FALSE(Find("error"));
  EXPECT_FALSE(Find("close", server));
}

TEST_F(XenonNetTcpTest, ClosingDuringConnectionDoesNotRestartReads) {
  const int port = Listen();
  ASSERT_GT(port, 0);
  int callbacks = 0;
  event_hook_ = base::BindLambdaForTesting(
      [&](const std::string& channel, const base::DictValue& payload) {
        const auto* id = channel == "__xenon:net:connection"
                             ? payload.FindString("socketId")
                             : channel == "__xenon:net:connected"
                                   ? payload.FindString("peerId")
                                   : nullptr;
        if (!id) {
          return;
        }
        ++callbacks;
        EXPECT_TRUE(bridge_.CloseSocket(*id));
        EXPECT_FALSE(bridge_.SetSocketReadPaused(*id, false));
      });
  std::string error;
  ASSERT_TRUE(bridge_.ConnectTcp("container", "client", "client-js",
                                 "127.0.0.1", port, &error));
  ASSERT_TRUE(base::test::RunUntil([&] { return callbacks == 2; }));
  EXPECT_FALSE(bridge_.HasSocket(*Find("connected")->FindString("peerId")));
  EXPECT_FALSE(bridge_.HasSocket(*Find("connection")->FindString("socketId")));
  EXPECT_FALSE(Find("error"));
}

TEST_F(XenonNetTcpTest, RepeatedJavascriptServerIdsAreScopedToTheirEndpoint) {
  ASSERT_GT(Listen(), 0);
  std::string error;
  ASSERT_TRUE(bridge_.ListenTcp("container", "another-page", "listener",
                                "127.0.0.1", 0, &error))
      << error;
  const auto first = bridge_.ServerAddress("listener", "container", "server");
  const auto second =
      bridge_.ServerAddress("listener", "container", "another-page");
  EXPECT_NE(first.GetDict().FindInt("port"), second.GetDict().FindInt("port"));
  EXPECT_FALSE(bridge_.CloseServer("listener", "container", "unrelated-page"));
  EXPECT_TRUE(bridge_.CloseServer("listener", "container", "another-page"));
  ASSERT_TRUE(base::test::RunUntil([&] { return Find("server-closed"); }));
  EXPECT_TRUE(
      bridge_.ServerAddress("listener", "container", "server").is_dict());
}

TEST_F(XenonNetTcpTest, Ipv6LoopbackPublishesTheActualFamily) {
  std::string error;
  if (!bridge_.ListenTcp("container", "server", "listener", "::1", 0, &error)) {
    GTEST_SKIP() << "IPv6 loopback unavailable: " << error;
  }
  auto address = bridge_.ServerAddress("listener");
  EXPECT_EQ(*address.GetDict().FindString("family"), "IPv6");
  ASSERT_TRUE(bridge_.ConnectTcp("container", "client", "ipv6-client", "::1",
                                 *address.GetDict().FindInt("port"), &error));
  ASSERT_TRUE(
      base::test::RunUntil([&] { return Find("connected", "ipv6-client"); }));
}

TEST_F(XenonNetTcpTest,
       UnimplementedHostnameResolutionDoesNotBindAnotherAddress) {
  std::string error;
  EXPECT_FALSE(bridge_.ListenTcp("container", "server", "listener",
                                 "unresolved.invalid", 0, &error));
  EXPECT_EQ(error, "ERR_NOT_SUPPORTED");
  EXPECT_FALSE(bridge_.HasServer("listener"));
}

}  // namespace
}  // namespace xenon
