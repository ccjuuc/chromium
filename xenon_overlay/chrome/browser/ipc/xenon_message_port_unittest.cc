// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
#include "xenon_overlay/chrome/browser/ipc/xenon_message_port.h"

#include "base/functional/bind.h"
#include "base/run_loop.h"
#include "gin/converter.h"
#include "gin/function_template.h"
#include "gin/test/v8_test.h"
#include "mojo/core/embedder/embedder.h"
#include "third_party/blink/public/common/messaging/message_port_descriptor.h"
#include "v8/include/v8.h"

namespace xenon::ipc {
namespace {
class MessagePortRuntimeTest : public gin::V8Test {
 protected:
  static void SetUpTestSuite() { mojo::core::Init(); }
  gin::IsolateHolder::AccessMode AccessMode() const override {
    return gin::IsolateHolder::AccessMode::kUseLocker;
  }
  void SetUp() override {
    gin::V8Test::SetUp();
    auto* isolate = instance_->isolate();
    v8::HandleScope handles(isolate);
    auto context = context_.Get(isolate);
    v8::Context::Scope scope(context);
    runtime_ = std::make_unique<MessagePortRuntime>(
        isolate, context,
        base::BindRepeating([](const std::string&) { return false; }),
        MessagePortRuntime::Post());
    ASSERT_TRUE(runtime_->Install());
    auto shutdown =
        gin::CreateFunctionTemplate(
            isolate, base::BindRepeating(&MessagePortRuntime::Shutdown,
                                         base::Unretained(runtime_.get())))
            ->GetFunction(context)
            .ToLocalChecked();
    context->Global()
        ->Set(context, gin::StringToV8(isolate, "shutdownRuntime"), shutdown)
        .Check();
    Run(R"JS(
      globalThis.events = [];
      globalThis.__xenonAdoptMessagePorts = ids => ids.map(id => {
        const port = {id}; __xenonMessagePortBind(id, port); return port;
      });
      globalThis.__xenonDetachMessagePort = port => { port.detached = true; };
      globalThis.pair = () => __xenonAdoptMessagePorts(__xenonMessagePortCreate());
      globalThis.__xenonDispatchMessagePort = (id, type, data, ports) =>
        events.push({id, type, data, ports});
      globalThis.__xenonDispatchPortMessage = (sender, channel, data, ports) =>
        events.push({sender, channel, data, ports});
    )JS");
  }
  void TearDown() override {
    runtime_.reset();
    base::RunLoop().RunUntilIdle();
    gin::V8Test::TearDown();
  }
  void Run(const char* source) {
    auto* isolate = instance_->isolate();
    v8::HandleScope handles(isolate);
    auto context = context_.Get(isolate);
    v8::Context::Scope scope(context);
    v8::TryCatch caught(isolate);
    v8::Local<v8::Script> script;
    v8::Local<v8::Value> result;
    ASSERT_TRUE(v8::Script::Compile(
                    context, gin::StringToV8(isolate, source).As<v8::String>())
                    .ToLocal(&script));
    ASSERT_TRUE(script->Run(context).ToLocal(&result))
        << (caught.HasCaught()
                ? *v8::String::Utf8Value(isolate, caught.Exception())
                : "unknown error");
  }
  void Expect(const char* source) {
    auto* isolate = instance_->isolate();
    v8::HandleScope handles(isolate);
    auto context = context_.Get(isolate);
    v8::Context::Scope scope(context);
    auto result =
        v8::Script::Compile(context,
                            gin::StringToV8(isolate, source).As<v8::String>())
            .ToLocalChecked()
            ->Run(context)
            .ToLocalChecked();
    EXPECT_TRUE(result->IsTrue()) << source;
  }
  std::unique_ptr<MessagePortRuntime> runtime_;
};

TEST_F(MessagePortRuntimeTest,
       QueuesUntilStartAndClonesGraphsInBothDirections) {
  Run(R"JS(
    const [a, b] = pair();
    const graph = {map: new Map([['key', 42n]]), bytes: new Uint8Array([1, 2])};
    graph.self = graph;
    __xenonMessagePortPost(a.id, graph, []);
    graph.bytes[0] = 99;
  )JS");
  base::RunLoop().RunUntilIdle();
  Expect("events.length === 0");
  Run("__xenonMessagePortStart(b.id);");
  base::RunLoop().RunUntilIdle();
  Expect(
      "events.length === 1 && events[0].id === b.id && events[0].data.self === "
      "events[0].data && events[0].data.map.get('key') === 42n && "
      "events[0].data.bytes[0] === 1");
  Run("__xenonMessagePortStart(a.id); __xenonMessagePortPost(b.id, {reply: "
      "true}, []);");
  base::RunLoop().RunUntilIdle();
  Expect(
      "events.length === 2 && events[1].id === a.id && events[1].data.reply");
}

TEST_F(MessagePortRuntimeTest, HostBootstrapTransfersARealEndpointOnce) {
  Run("globalThis.hostPair = pair(); __xenonMessagePortPost(hostPair[0].id, "
      "'queued', []);");
  auto* isolate = instance_->isolate();
  v8::HandleScope handles(isolate);
  auto context = context_.Get(isolate);
  v8::Context::Scope scope(context);
  auto pair = context->Global()
                  ->Get(context, gin::StringToV8(isolate, "hostPair"))
                  .ToLocalChecked()
                  .As<v8::Array>();
  auto old_port = pair->Get(context, 1u).ToLocalChecked().As<v8::Object>();
  blink::MessagePortDescriptor descriptor;
  ASSERT_TRUE(runtime_->TakePort(old_port, &descriptor));
  Expect("hostPair[1].detached === true");
  ASSERT_TRUE(descriptor.IsValid());
  {
    v8::TryCatch caught(isolate);
    blink::MessagePortDescriptor duplicate;
    EXPECT_FALSE(runtime_->TakePort(old_port, &duplicate));
    EXPECT_TRUE(caught.HasCaught());
    EXPECT_FALSE(duplicate.IsValid());
  }
  v8::Local<v8::Object> adopted;
  ASSERT_TRUE(runtime_->AdoptPort(std::move(descriptor)).ToLocal(&adopted));
  context->Global()
      ->Set(context, gin::StringToV8(isolate, "adopted"), adopted)
      .Check();
  Run("__xenonMessagePortStart(adopted.id)");
  base::RunLoop().RunUntilIdle();
  Expect(
      "events.length === 1 && events[0].id === adopted.id && events[0].data "
      "=== 'queued'");
}

TEST_F(MessagePortRuntimeTest, TransfersStartedPortAndPreservesQueuedMessages) {
  Run(R"JS(
    const [a, b] = pair();
    const [c, d] = pair();
    __xenonMessagePortStart(b.id);
    __xenonMessagePortPost(a.id, 'queued-before-transfer', []);
    __xenonMessagePortStart(d.id);
    __xenonMessagePortPost(c.id, {nested: b}, [b]);
  )JS");
  base::RunLoop().RunUntilIdle();
  Expect(
      "events.length === 1 && events[0].id === d.id && events[0].data.nested "
      "=== events[0].ports[0]");
  Run("const moved = events[0].ports[0]; __xenonMessagePortStart(moved.id);");
  base::RunLoop().RunUntilIdle();
  Expect(
      "events.length === 2 && events[1].id === moved.id && events[1].data === "
      "'queued-before-transfer'");
  Expect("__xenonMessagePortPost(b.id, 'stale', []) === false");
}

TEST_F(MessagePortRuntimeTest, TransfersReceivingPortFromItsMessageListener) {
  Run(R"JS(
    const [a, b] = pair();
    const [c, d] = pair();
    let moved;
    __xenonDispatchMessagePort = (id, type, data, ports) => {
      events.push({id, type, data, ports});
      if (id === b.id && type === 'message') {
        __xenonMessagePortPost(c.id, 'moved-during-dispatch', [b]);
      } else if (id === d.id && type === 'message') {
        moved = ports[0];
        __xenonMessagePortStart(moved.id);
      }
    };
    __xenonMessagePortPost(a.id, 'first', []);
    __xenonMessagePortPost(a.id, 'second', []);
    __xenonMessagePortPost(a.id, 'third', []);
    __xenonMessagePortStart(b.id);
    __xenonMessagePortStart(d.id);
  )JS");
  base::RunLoop().RunUntilIdle();
  Expect(
      "events.length === 4 && events[0].id === b.id && "
      "events[0].data === 'first' && events[1].id === d.id && "
      "events[1].data === 'moved-during-dispatch' && "
      "events[2].id === moved.id && events[2].data === 'second' && "
      "events[3].id === moved.id && events[3].data === 'third'");
  Expect("__xenonMessagePortPost(b.id, 'stale', []) === false");
}

TEST_F(MessagePortRuntimeTest,
       FailedCloneAndUnavailableRendererKeepPortsOwned) {
  Run(R"JS(
    const [a, b] = pair();
    const [c, d] = pair();
    const failures = [];
    for (const action of [
      () => __xenonMessagePortPost(a.id, () => {}, [c]),
      () => __xenonMessagePortPost(a.id, 1, [c, c]),
      () => __xenonMessagePortPost(a.id, 1, [a]),
      () => __xenonMessagePortPost(a.id, {port: c}, []),
      () => __xenonPostPortMessage('missing', 'channel', 1, [c]),
    ]) { try { action(); failures.push(false); } catch { failures.push(true); } }
    __xenonMessagePortStart(d.id);
    __xenonMessagePortPost(c.id, 'still-owned', []);
  )JS");
  base::RunLoop().RunUntilIdle();
  Expect(
      "failures.every(Boolean) && events.length === 1 && events[0].data === "
      "'still-owned'");
}

TEST_F(MessagePortRuntimeTest, SerializationGetterCannotReuseTransferredPort) {
  Run(R"JS(
    const [a, b] = pair();
    const [c, d] = pair();
    const [e, f] = pair();
    __xenonMessagePortStart(f.id);
    let rejected = false;
    try {
      __xenonMessagePortPost(a.id, {get effect() {
        __xenonMessagePortPost(e.id, 'inner', [c]); return true;
      }}, [c]);
    } catch { rejected = true; }
  )JS");
  base::RunLoop().RunUntilIdle();
  Expect(
      "rejected && events.length === 1 && events[0].data === 'inner' && "
      "events[0].ports.length === 1");
}

TEST_F(MessagePortRuntimeTest, CloseNotifiesBothSidesEvenBeforeStart) {
  Run("const [a, b] = pair(); __xenonMessagePortClose(a.id, true); "
      "__xenonMessagePortClose(a.id, true);");
  Expect("events.length === 0");
  base::RunLoop().RunUntilIdle();
  Expect(
      "events.length === 2 && events.every(event => event.type === 'close') && "
      "new Set(events.map(event => event.id)).size === 2");
}

TEST_F(MessagePortRuntimeTest, SerializationGetterMayShutDownRuntime) {
  Run(R"JS(
    const [a, b] = pair();
    const [c, d] = pair();
    let rejected = false;
    try {
      __xenonMessagePortPost(a.id, {get stop() {
        shutdownRuntime(); return c;
      }}, [c]);
    } catch { rejected = true; }
  )JS");
  base::RunLoop().RunUntilIdle();
  Expect("rejected && events.length === 0");
}

}  // namespace
}  // namespace xenon::ipc

namespace xenon::ipc {
namespace {
TEST_F(MessagePortRuntimeTest,
       HostTransferHookFailurePreservesNativeOwnership) {
  Run(R"JS(
    globalThis.hostPair = pair();
    __xenonMessagePortStart(hostPair[0].id);
    globalThis.savedDetach = __xenonDetachMessagePort;
    __xenonDetachMessagePort = () => { throw new Error('detachment failed'); };
  )JS");
  auto* isolate = instance_->isolate();
  v8::HandleScope handles(isolate);
  auto context = context_.Get(isolate);
  v8::Context::Scope scope(context);
  auto pair = context->Global()
                  ->Get(context, gin::StringToV8(isolate, "hostPair"))
                  .ToLocalChecked()
                  .As<v8::Array>();
  auto port = pair->Get(context, 1u).ToLocalChecked().As<v8::Object>();
  blink::MessagePortDescriptor descriptor;
  {
    v8::TryCatch caught(isolate);
    EXPECT_FALSE(runtime_->TakePort(port, &descriptor));
    EXPECT_TRUE(caught.HasCaught());
  }
  EXPECT_FALSE(descriptor.IsValid());
  Run("__xenonMessagePortPost(hostPair[1].id, 'still-owned', []);");
  base::RunLoop().RunUntilIdle();
  Expect("events.length === 1 && events[0].data === 'still-owned'");
  Run("__xenonDetachMessagePort = savedDetach;");
  EXPECT_TRUE(runtime_->TakePort(port, &descriptor));
  EXPECT_TRUE(descriptor.IsValid());
}

TEST_F(MessagePortRuntimeTest, FailedHostAdoptionClosesCreatedStartedWrapper) {
  Run(R"JS(
    globalThis.hostPair = pair();
    globalThis.originalAdopt = __xenonAdoptMessagePorts;
  )JS");
  auto* isolate = instance_->isolate();
  v8::HandleScope handles(isolate);
  auto context = context_.Get(isolate);
  v8::Context::Scope scope(context);
  auto pair = context->Global()
                  ->Get(context, gin::StringToV8(isolate, "hostPair"))
                  .ToLocalChecked()
                  .As<v8::Array>();
  auto port = pair->Get(context, 1u).ToLocalChecked().As<v8::Object>();
  blink::MessagePortDescriptor descriptor;
  ASSERT_TRUE(runtime_->TakePort(port, &descriptor));
  Run(R"JS(
    __xenonAdoptMessagePorts = ids => {
      globalThis.failedAdopted = originalAdopt(ids)[0];
      __xenonMessagePortStart(failedAdopted.id);
      throw new Error('factory failed after starting');
    };
  )JS");
  {
    v8::TryCatch caught(isolate);
    EXPECT_TRUE(runtime_->AdoptPort(std::move(descriptor)).IsEmpty());
    EXPECT_TRUE(caught.HasCaught());
  }
  base::RunLoop().RunUntilIdle();
  Expect(
      "events.some(event => event.id === failedAdopted.id && event.type === "
      "'close')");
  Expect("__xenonMessagePortPost(failedAdopted.id, 'closed', []) === false");
  Run(R"JS(
    __xenonAdoptMessagePorts = originalAdopt;
    globalThis.closedPair = pair();
  )JS");
  auto closed_pair = context->Global()
                         ->Get(context, gin::StringToV8(isolate, "closedPair"))
                         .ToLocalChecked()
                         .As<v8::Array>();
  ASSERT_TRUE(runtime_->TakePort(
      closed_pair->Get(context, 1u).ToLocalChecked().As<v8::Object>(),
      &descriptor));
  Run(R"JS(
    __xenonAdoptMessagePorts = ids => {
      const ports = originalAdopt(ids);
      __xenonMessagePortClose(ids[0], true);
      return ports;
    };
  )JS");
  EXPECT_TRUE(runtime_->AdoptPort(std::move(descriptor)).IsEmpty());
}
}  // namespace
}  // namespace xenon::ipc

namespace xenon::ipc {
namespace {
TEST_F(MessagePortRuntimeTest,
       PausingDuringDispatchPreservesQueueAndObservesPeerClose) {
  Run(R"JS(
    const [pauseSender, pauseReceiver] = pair();
    __xenonDispatchMessagePort = (id, type, data, ports) => {
      events.push({id, type, data, ports});
      if (id === pauseReceiver.id && type === 'message' && data === 'first')
        __xenonMessagePortPause(id);
    };
    __xenonMessagePortPost(pauseSender.id, 'first', []);
    __xenonMessagePortPost(pauseSender.id, 'second', []);
    __xenonMessagePortPost(pauseSender.id, 'third', []);
    __xenonMessagePortStart(pauseReceiver.id);
  )JS");
  base::RunLoop().RunUntilIdle();
  Expect("events.length === 1 && events[0].data === 'first'");
  Run("__xenonMessagePortStart(pauseReceiver.id);");
  base::RunLoop().RunUntilIdle();
  Expect(
      "events.length === 3 && events[1].data === 'second' && events[2].data "
      "=== 'third'");
  Run("__xenonMessagePortPause(pauseReceiver.id); "
      "__xenonMessagePortClose(pauseSender.id, true);");
  base::RunLoop().RunUntilIdle();
  Expect(
      "events.some(event => event.id === pauseReceiver.id && event.type === "
      "'close')");
}
}  // namespace
}  // namespace xenon::ipc

namespace xenon::ipc {
namespace {
TEST_F(MessagePortRuntimeTest, ReportsListenerExceptionToTheOwningRuntime) {
  int calls = 0;
  std::string message;
  runtime_->SetExceptionHandler(base::BindRepeating(
      [](int* count, std::string* text, v8::Local<v8::Value> exception) {
        ++*count;
        EXPECT_TRUE(exception->IsNativeError());
        *text = *v8::String::Utf8Value(v8::Isolate::GetCurrent(), exception);
      },
      &calls, &message));
  Run(R"JS(
    const [throwingSender, throwingReceiver] = pair();
    __xenonDispatchMessagePort = () => { throw new Error('listener failure'); };
    __xenonMessagePortStart(throwingReceiver.id);
    __xenonMessagePortPost(throwingSender.id, 'message', []);
  )JS");
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(calls, 1);
  EXPECT_NE(message.find("listener failure"), std::string::npos);
  runtime_->Shutdown();
}
}  // namespace
}  // namespace xenon::ipc
