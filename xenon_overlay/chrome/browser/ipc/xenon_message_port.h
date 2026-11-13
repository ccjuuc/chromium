// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
#ifndef XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_MESSAGE_PORT_H_
#define XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_MESSAGE_PORT_H_

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "third_party/blink/public/common/messaging/transferable_message.h"
#include "v8/include/v8-forward.h"
#include "v8/include/v8-persistent-handle.h"

namespace gin {
class Arguments;
}
namespace xenon::ipc {

// One owner sequence and V8 realm. Every endpoint is a real Blink-compatible
// Mojo message pipe, transferable to DOM MessagePort without a JS relay.
class MessagePortRuntime {
 public:
  using CanPost = base::RepeatingCallback<bool(const std::string& endpoint)>;
  using Post = base::RepeatingCallback<void(const std::string& endpoint,
                                            const std::string& channel,
                                            blink::TransferableMessage)>;
  MessagePortRuntime(v8::Isolate* isolate,
                     v8::Local<v8::Context> context,
                     CanPost can_post,
                     Post post);
  ~MessagePortRuntime();
  bool Install();
  void SetExceptionHandler(
      base::RepeatingCallback<void(v8::Local<v8::Value>)> handler);
  // Trusted host bootstrap can move a dedicated endpoint between utility
  // processes without pretending it is a renderer document or JSON message.
  v8::MaybeLocal<v8::Object> AdoptPort(blink::MessagePortDescriptor descriptor);
  bool TakePort(v8::Local<v8::Object> port,
                blink::MessagePortDescriptor* descriptor);
  // Called with the owner realm entered. Sender is the container's normal
  // document-scoped IPC sender metadata, never supplied by renderer script.
  void Receive(v8::Local<v8::Value> sender,
               const std::string& channel,
               blink::TransferableMessage message);
  // Stops every port and invalidates pending events. Retain the runtime until
  // final V8 teardown if native callbacks can still invoke installed bindings.
  void Shutdown();

 private:
  class Endpoint;
  int Add(blink::MessagePortDescriptor descriptor);
  int PortId(v8::Local<v8::Object> object) const;
  bool IsPort(v8::Local<v8::Object> object) const;
  bool Transfers(v8::Local<v8::Value> input,
                 int sender,
                 std::vector<int>* ids,
                 std::vector<v8::Local<v8::Object>>* objects);
  bool ValidateTransfers(const std::vector<int>& ids, int sender);
  void CommitTransfers(const std::vector<int>& ids,
                       blink::TransferableMessage* message);
  bool Decode(blink::TransferableMessage* message,
              v8::Local<v8::Value>* value,
              v8::Local<v8::Array>* ports);
  void OnMessage(int id, blink::TransferableMessage message);
  void Close(int id, bool notify);
  void Dispatch(int id,
                const char* type,
                v8::Local<v8::Value> data,
                v8::Local<v8::Array> ports);
  void EmitClose(int id);
  void Create(gin::Arguments* args);
  void Bind(gin::Arguments* args);
  void Send(gin::Arguments* args);
  void Start(gin::Arguments* args);
  void Pause(gin::Arguments* args);
  void CloseBinding(gin::Arguments* args);
  void SendToRenderer(gin::Arguments* args);
  raw_ptr<v8::Isolate> isolate_;
  v8::Global<v8::Context> context_;
  v8::Global<v8::Private> brand_;
  std::map<int, std::unique_ptr<Endpoint>> ports_;
  CanPost can_post_;
  Post post_;
  base::RepeatingCallback<void(v8::Local<v8::Value>)> exception_handler_;
  int next_id_ = 1;
  bool stopped_ = false;
  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<MessagePortRuntime> weak_factory_{this};
};
}  // namespace xenon::ipc
#endif
