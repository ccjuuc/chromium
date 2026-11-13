// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
#include "xenon_overlay/chrome/browser/ipc/xenon_message_port.h"

#include <limits>
#include <set>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/task/single_thread_task_runner.h"
#include "gin/arguments.h"
#include "gin/converter.h"
#include "gin/function_template.h"
#include "mojo/public/cpp/bindings/connector.h"
#include "mojo/public/cpp/system/simple_watcher.h"
#include "third_party/blink/public/common/messaging/message_port_descriptor.h"
#include "third_party/blink/public/common/messaging/transferable_message_mojom_traits.h"
#include "third_party/blink/public/mojom/blob/blob.mojom.h"
#include "third_party/blink/public/mojom/messaging/transferable_message.mojom.h"
#include "v8/include/v8.h"
#include "xenon_overlay/common/ipc/xenon_message_port_codec.h"

namespace xenon::ipc {
namespace {
class Realm {
 public:
  Realm(v8::Isolate* isolate, const v8::Global<v8::Context>& context)
      : locker_(isolate),
        isolate_(isolate),
        handles_(isolate),
        context_(context.Get(isolate)),
        entered_(context_) {}
  v8::Local<v8::Context> context() { return context_; }

 private:
  v8::Locker locker_;
  v8::Isolate::Scope isolate_;
  v8::HandleScope handles_;
  v8::Local<v8::Context> context_;
  v8::Context::Scope entered_;
};
v8::Local<v8::String> String(v8::Isolate* isolate, const char* text) {
  return v8::String::NewFromUtf8(isolate, text).ToLocalChecked();
}
}  // namespace

class MessagePortRuntime::Endpoint final : public mojo::MessageReceiver {
 public:
  Endpoint(MessagePortRuntime* owner, int id, blink::MessagePortDescriptor port)
      : owner_(owner),
        id_(id),
        descriptor_(std::move(port)),
        peer_watcher_(FROM_HERE, mojo::SimpleWatcher::ArmingPolicy::MANUAL) {
    connector_ = std::make_unique<mojo::Connector>(
        descriptor_.TakeHandleToEntangleWithEmbedder(),
        mojo::Connector::SINGLE_THREADED_SEND);
    connector_->set_incoming_receiver(this);
    // Dispatch directly in the connector task so taking its pipe preserves
    // all not-yet-delivered messages when an already-started port is moved.
    connector_->set_force_immediate_dispatch(true);
    connector_->set_connection_error_handler(
        base::BindOnce(&MessagePortRuntime::Close,
                       owner_->weak_factory_.GetWeakPtr(), id_, true));
    WatchPeerClosed();
  }
  ~Endpoint() override { Take(); }
  bool Accept(mojo::Message* incoming) override {
    blink::TransferableMessage message;
    if (!blink::mojom::TransferableMessage::DeserializeFromMessage(
            std::move(*incoming), &message)) {
      return false;
    }
    owner_->OnMessage(id_, std::move(message));
    return true;
  }
  void Start() {
    if (started_) {
      return;
    }
    started_ = true;
    peer_watcher_.Cancel();
    if (receiving_) {
      connector_->ResumeIncomingMethodCallProcessing();
    } else {
      receiving_ = true;
      connector_->StartReceiving(
          base::SingleThreadTaskRunner::GetCurrentDefault());
    }
  }
  void Pause() {
    if (!started_) {
      return;
    }
    started_ = false;
    connector_->PauseIncomingMethodCallProcessing();
    WatchPeerClosed();
  }
  bool Send(blink::TransferableMessage message) {
    auto wire = blink::mojom::TransferableMessage::SerializeAsMessage(&message);
    return connector_->Accept(&wire);
  }
  blink::MessagePortDescriptor Take() {
    peer_watcher_.Cancel();
    if (connector_) {
      descriptor_.GiveDisentangledHandle(connector_->PassMessagePipe());
      connector_.reset();
    }
    return std::move(descriptor_);
  }

 private:
  void WatchPeerClosed() {
    peer_watcher_.Watch(
        connector_->handle(), MOJO_HANDLE_SIGNAL_PEER_CLOSED,
        MOJO_TRIGGER_CONDITION_SIGNALS_SATISFIED,
        base::BindRepeating(&Endpoint::OnPeerClosed, base::Unretained(this)));
    peer_watcher_.ArmOrNotify();
  }
  void OnPeerClosed(MojoResult result, const mojo::HandleSignalsState& state) {
    // Preserve unread messages until start(). The connector reports closure
    // after draining them. An empty, never-started port still emits close.
    peer_watcher_.Cancel();
    if (result == MOJO_RESULT_OK && !state.readable()) {
      owner_->Close(id_, true);
    }
  }
  raw_ptr<MessagePortRuntime> owner_;
  int id_;
  blink::MessagePortDescriptor descriptor_;
  std::unique_ptr<mojo::Connector> connector_;
  mojo::SimpleWatcher peer_watcher_;
  bool started_ = false;
  bool receiving_ = false;
};

MessagePortRuntime::MessagePortRuntime(v8::Isolate* isolate,
                                       v8::Local<v8::Context> context,
                                       CanPost can_post,
                                       Post post)
    : isolate_(isolate),
      context_(isolate, context),
      brand_(isolate, v8::Private::New(isolate)),
      can_post_(std::move(can_post)),
      post_(std::move(post)) {}
MessagePortRuntime::~MessagePortRuntime() {
  Shutdown();
  brand_.Reset();
  context_.Reset();
}

bool MessagePortRuntime::Install() {
  auto context = context_.Get(isolate_);
  const struct {
    const char* name;
    void (MessagePortRuntime::*callback)(gin::Arguments*);
  } bindings[] = {
      {"__xenonMessagePortCreate", &MessagePortRuntime::Create},
      {"__xenonMessagePortBind", &MessagePortRuntime::Bind},
      {"__xenonMessagePortPost", &MessagePortRuntime::Send},
      {"__xenonMessagePortStart", &MessagePortRuntime::Start},
      {"__xenonMessagePortPause", &MessagePortRuntime::Pause},
      {"__xenonMessagePortClose", &MessagePortRuntime::CloseBinding},
      {"__xenonPostPortMessage", &MessagePortRuntime::SendToRenderer}};
  for (const auto& binding : bindings) {
    v8::Local<v8::Function> callback;
    if (!gin::CreateFunctionTemplate(
             isolate_,
             base::BindRepeating(binding.callback, weak_factory_.GetWeakPtr()))
             ->GetFunction(context)
             .ToLocal(&callback) ||
        !context->Global()
             ->Set(context, String(isolate_, binding.name), callback)
             .FromMaybe(false)) {
      return false;
    }
  }
  return true;
}

void MessagePortRuntime::SetExceptionHandler(
    base::RepeatingCallback<void(v8::Local<v8::Value>)> handler) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  exception_handler_ = std::move(handler);
}

int MessagePortRuntime::Add(blink::MessagePortDescriptor descriptor) {
  CHECK_LT(next_id_, std::numeric_limits<int>::max());
  const int id = next_id_++;
  ports_.emplace(id,
                 std::make_unique<Endpoint>(this, id, std::move(descriptor)));
  return id;
}

v8::MaybeLocal<v8::Object> MessagePortRuntime::AdoptPort(
    blink::MessagePortDescriptor descriptor) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (stopped_ || !descriptor.IsValid() || descriptor.IsEntangled()) {
    return {};
  }
  auto context = context_.Get(isolate_);
  const int id = Add(std::move(descriptor));
  auto ids = v8::Array::New(isolate_, 1);
  if (!ids->CreateDataProperty(context, 0u, v8::Integer::New(isolate_, id))
           .FromMaybe(false)) {
    Close(id, true);
    return {};
  }
  v8::Local<v8::Value> factory;
  v8::Local<v8::Value> result;
  v8::Local<v8::Value> wrapper;
  v8::Local<v8::Value> argument = ids;
  if (context->Global()
          ->Get(context, String(isolate_, "__xenonAdoptMessagePorts"))
          .ToLocal(&factory) &&
      factory->IsFunction() &&
      factory.As<v8::Function>()
          ->Call(context, context->Global(), 1, &argument)
          .ToLocal(&result) &&
      result->IsArray() && result.As<v8::Array>()->Length() == 1 &&
      result.As<v8::Array>()->Get(context, 0u).ToLocal(&wrapper) &&
      wrapper->IsObject() && PortId(wrapper.As<v8::Object>()) == id &&
      ports_.contains(id)) {
    return wrapper.As<v8::Object>();
  }
  // A factory can have created/started a wrapper before throwing. Normal
  // asynchronous close dispatch also releases its JS keepalive references.
  Close(id, true);
  return {};
}

bool MessagePortRuntime::TakePort(v8::Local<v8::Object> port,
                                  blink::MessagePortDescriptor* descriptor) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  const int id = PortId(port);
  if (stopped_ || id == 0 || !ports_.contains(id)) {
    isolate_->ThrowException(v8::Exception::TypeError(
        String(isolate_, "MessagePort is not owned by this runtime")));
    return false;
  }
  auto context = context_.Get(isolate_);
  v8::Local<v8::Value> detach, ignored;
  v8::Local<v8::Value> argument = port;
  if (!context->Global()
           ->Get(context, String(isolate_, "__xenonDetachMessagePort"))
           .ToLocal(&detach)) {
    return false;
  }
  if (!detach->IsFunction()) {
    isolate_->ThrowException(v8::Exception::TypeError(
        String(isolate_, "MessagePort detachment callback is unavailable")));
    return false;
  }
  if (!detach.As<v8::Function>()
           ->Call(context, context->Global(), 1, &argument)
           .ToLocal(&ignored)) {
    return false;
  }
  // Invoking JS can synchronously close the port or shut down the runtime.
  if (stopped_ || !ports_.contains(id)) {
    isolate_->ThrowException(v8::Exception::TypeError(
        String(isolate_, "MessagePort was closed during detachment")));
    return false;
  }
  *descriptor = ports_.at(id)->Take();
  ports_.erase(id);
  return true;
}
int MessagePortRuntime::PortId(v8::Local<v8::Object> object) const {
  v8::Local<v8::Value> id;
  return object->GetPrivate(context_.Get(isolate_), brand_.Get(isolate_))
                     .ToLocal(&id) &&
                 id->IsInt32()
             ? id.As<v8::Int32>()->Value()
             : 0;
}
bool MessagePortRuntime::IsPort(v8::Local<v8::Object> object) const {
  return PortId(object) != 0;
}
bool MessagePortRuntime::ValidateTransfers(const std::vector<int>& ids,
                                           int sender) {
  std::set<int> seen;
  for (int id : ids) {
    if (!id || id == sender || !ports_.contains(id) ||
        !seen.insert(id).second) {
      ThrowMessagePortCloneError(isolate_,
                                 "Transfer list contains an invalid, detached, "
                                 "duplicate, or source port");
      return false;
    }
  }
  return true;
}
bool MessagePortRuntime::Transfers(
    v8::Local<v8::Value> input,
    int sender,
    std::vector<int>* ids,
    std::vector<v8::Local<v8::Object>>* objects) {
  if (input->IsUndefined()) {
    return true;
  }
  if (!input->IsArray() || input.As<v8::Array>()->Length() > 1024) {
    ThrowMessagePortCloneError(isolate_,
                               "Transfer list must be an array of at most 1024 "
                               "MessagePortMain objects");
    return false;
  }
  auto context = context_.Get(isolate_);
  auto array = input.As<v8::Array>();
  const uint32_t length = array->Length();
  for (uint32_t i = 0; i < length; ++i) {
    v8::Local<v8::Value> value;
    if (!array->Get(context, i).ToLocal(&value)) {
      return false;
    }
    if (!value->IsObject()) {
      ThrowMessagePortCloneError(isolate_,
                                 "Transfer entry must be a MessagePortMain");
      return false;
    }
    objects->push_back(value.As<v8::Object>());
    ids->push_back(PortId(value.As<v8::Object>()));
  }
  return ValidateTransfers(*ids, sender);
}
void MessagePortRuntime::CommitTransfers(const std::vector<int>& ids,
                                         blink::TransferableMessage* message) {
  for (int id : ids) {
    auto it = ports_.find(id);
    message->ports.emplace_back(it->second->Take());
    ports_.erase(it);
  }
}

void MessagePortRuntime::Create(gin::Arguments* args) {
  if (stopped_) {
    isolate_->ThrowException(v8::Exception::Error(
        String(isolate_, "Message port runtime is shut down")));
    return;
  }
  blink::MessagePortDescriptorPair pair;
  auto array = v8::Array::New(isolate_, 2);
  auto context = context_.Get(isolate_);
  array->Set(context, 0, v8::Integer::New(isolate_, Add(pair.TakePort0())))
      .Check();
  array->Set(context, 1, v8::Integer::New(isolate_, Add(pair.TakePort1())))
      .Check();
  args->Return(array.As<v8::Value>());
}
void MessagePortRuntime::Bind(gin::Arguments* args) {
  int id;
  v8::Local<v8::Object> object;
  if (!args->GetNext(&id) || !args->GetNext(&object) || !ports_.contains(id) ||
      IsPort(object)) {
    args->ThrowTypeError("Invalid MessagePortMain wrapper");
    return;
  }
  object
      ->SetPrivate(context_.Get(isolate_), brand_.Get(isolate_),
                   v8::Integer::New(isolate_, id))
      .Check();
}
void MessagePortRuntime::Send(gin::Arguments* args) {
  int id;
  v8::Local<v8::Value> value, transfer = v8::Undefined(isolate_);
  if (!args->GetNext(&id) || !args->GetNext(&value)) {
    args->ThrowTypeError("Invalid port message");
    return;
  }
  if (args->Length() > 2 && !args->GetNext(&transfer)) {
    return;
  }
  if (stopped_ || !ports_.contains(id)) {
    args->Return(false);
    return;
  }
  std::vector<int> ids;
  std::vector<v8::Local<v8::Object>> objects;
  blink::TransferableMessage message;
  if (!Transfers(transfer, id, &ids, &objects) ||
      !EncodeMessagePortValue(context_.Get(isolate_), value, objects,
                              base::BindRepeating(&MessagePortRuntime::IsPort,
                                                  base::Unretained(this)),
                              &message) ||
      !ValidateTransfers(ids, id)) {
    return;
  }
  // Serializing getters may close or transfer the source port.
  auto source = ports_.find(id);
  if (source == ports_.end()) {
    args->Return(false);
    return;
  }
  CommitTransfers(ids, &message);
  // As with DOM ports, a peer disappearing after acceptance does not undo
  // a transfer. The transferred handles are closed by the discarded message.
  source->second->Send(std::move(message));
  args->Return(true);
}
void MessagePortRuntime::Start(gin::Arguments* args) {
  int id;
  if (!args->GetNext(&id)) {
    args->ThrowTypeError("Invalid port identity");
    return;
  }
  if (auto it = ports_.find(id); it != ports_.end()) {
    it->second->Start();
  }
}
void MessagePortRuntime::CloseBinding(gin::Arguments* args) {
  int id;
  bool notify = true;
  if (!args->GetNext(&id)) {
    args->ThrowTypeError("Invalid port identity");
    return;
  }
  if (args->Length() > 1 && !args->GetNext(&notify)) {
    return;
  }
  Close(id, notify);
}

void MessagePortRuntime::Pause(gin::Arguments* args) {
  int id;
  if (!args->GetNext(&id)) {
    return;
  }
  auto found = ports_.find(id);
  if (!stopped_ && found != ports_.end()) {
    found->second->Pause();
  }
}
void MessagePortRuntime::Close(int id, bool notify) {
  if (!ports_.erase(id) || !notify || stopped_) {
    return;
  }
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&MessagePortRuntime::EmitClose,
                                weak_factory_.GetWeakPtr(), id));
}
void MessagePortRuntime::EmitClose(int id) {
  if (stopped_) {
    return;
  }
  Realm scope(isolate_, context_);
  Dispatch(id, "close", v8::Undefined(isolate_), v8::Array::New(isolate_));
}
void MessagePortRuntime::SendToRenderer(gin::Arguments* args) {
  std::string endpoint, channel;
  v8::Local<v8::Value> value, transfer = v8::Undefined(isolate_);
  if (!args->GetNext(&endpoint) || !args->GetNext(&channel) ||
      !args->GetNext(&value)) {
    args->ThrowTypeError(
        "postMessage requires an endpoint, channel and message");
    return;
  }
  if (args->Length() > 3 && !args->GetNext(&transfer)) {
    return;
  }
  std::vector<int> ids;
  std::vector<v8::Local<v8::Object>> objects;
  blink::TransferableMessage message;
  if (!Transfers(transfer, 0, &ids, &objects) ||
      !EncodeMessagePortValue(context_.Get(isolate_), value, objects,
                              base::BindRepeating(&MessagePortRuntime::IsPort,
                                                  base::Unretained(this)),
                              &message) ||
      !ValidateTransfers(ids, 0)) {
    return;
  }
  if (stopped_ || !can_post_ || !post_ || !can_post_.Run(endpoint)) {
    isolate_->ThrowException(v8::Exception::Error(
        String(isolate_, "Target renderer document is unavailable")));
    return;
  }
  CommitTransfers(ids, &message);
  post_.Run(endpoint, channel, std::move(message));
  args->Return(true);
}

bool MessagePortRuntime::Decode(blink::TransferableMessage* message,
                                v8::Local<v8::Value>* value,
                                v8::Local<v8::Array>* ports) {
  if (!ValidateMessagePortMessage(*message)) {
    return false;
  }
  auto context = context_.Get(isolate_);
  auto ids = v8::Array::New(isolate_, message->ports.size());
  std::vector<int> adopted;
  for (size_t i = 0; i < message->ports.size(); ++i) {
    auto descriptor = message->ports[i].ReleaseHandle();
    if (!descriptor.IsValid()) {
      for (int id : adopted) {
        Close(id, false);
      }
      return false;
    }
    int id = Add(std::move(descriptor));
    adopted.push_back(id);
    ids->Set(context, i, v8::Integer::New(isolate_, id)).Check();
  }
  message->ports.clear();
  v8::Local<v8::Value> factory, result;
  bool success =
      context->Global()
          ->Get(context, String(isolate_, "__xenonAdoptMessagePorts"))
          .ToLocal(&factory) &&
      factory->IsFunction();
  v8::Local<v8::Value> argument = ids;
  success = success &&
            factory.As<v8::Function>()
                ->Call(context, context->Global(), 1, &argument)
                .ToLocal(&result) &&
            result->IsArray();
  std::vector<v8::Local<v8::Object>> wrappers;
  if (success) {
    *ports = result.As<v8::Array>();
    success = (*ports)->Length() == adopted.size();
    for (size_t i = 0; success && i < adopted.size(); ++i) {
      v8::Local<v8::Value> wrapper;
      success = (*ports)->Get(context, i).ToLocal(&wrapper) &&
                wrapper->IsObject() &&
                PortId(wrapper.As<v8::Object>()) == adopted[i];
      if (success) {
        wrappers.push_back(wrapper.As<v8::Object>());
      }
    }
  }
  if (success) {
    success =
        DecodeMessagePortValue(context, *message, wrappers).ToLocal(value);
  }
  if (!success) {
    for (int id : adopted) {
      Close(id, true);
    }
  }
  return success;
}
void MessagePortRuntime::Dispatch(int id,
                                  const char* type,
                                  v8::Local<v8::Value> data,
                                  v8::Local<v8::Array> ports) {
  auto context = context_.Get(isolate_);
  v8::TryCatch caught(isolate_);
  v8::Local<v8::Value> callback;
  if (!context->Global()
           ->Get(context, String(isolate_, "__xenonDispatchMessagePort"))
           .ToLocal(&callback) ||
      !callback->IsFunction()) {
    return;
  }
  v8::Local<v8::Value> arguments[] = {v8::Integer::New(isolate_, id),
                                      String(isolate_, type), data, ports};
  std::ignore = callback.As<v8::Function>()->Call(context, context->Global(), 4,
                                                  arguments);
  if (caught.HasCaught() && !caught.HasTerminated()) {
    if (exception_handler_) {
      auto handler = exception_handler_;
      handler.Run(caught.Exception());
    } else {
      LOG(ERROR) << "Uncaught MessagePortMain listener: "
                 << *v8::String::Utf8Value(isolate_, caught.Exception());
    }
  }
  isolate_->PerformMicrotaskCheckpoint();
}
void MessagePortRuntime::OnMessage(int id, blink::TransferableMessage message) {
  if (stopped_ || !ports_.contains(id)) {
    return;
  }
  Realm scope(isolate_, context_);
  v8::Local<v8::Value> value;
  v8::Local<v8::Array> ports;
  bool decoded;
  {
    v8::TryCatch caught(isolate_);
    decoded = Decode(&message, &value, &ports);
  }
  Dispatch(id, decoded ? "message" : "messageerror",
           decoded ? value : v8::Undefined(isolate_).As<v8::Value>(),
           decoded ? ports : v8::Array::New(isolate_));
}
void MessagePortRuntime::Receive(v8::Local<v8::Value> sender,
                                 const std::string& channel,
                                 blink::TransferableMessage message) {
  if (stopped_) {
    return;
  }
  auto context = context_.Get(isolate_);
  v8::TryCatch caught(isolate_);
  v8::Local<v8::Value> value, callback;
  v8::Local<v8::Array> ports;
  if (!Decode(&message, &value, &ports)) {
    LOG(ERROR) << "Uncloneable renderer port message";
    return;
  }
  if (!context->Global()
           ->Get(context, String(isolate_, "__xenonDispatchPortMessage"))
           .ToLocal(&callback) ||
      !callback->IsFunction()) {
    return;
  }
  v8::Local<v8::Value> arguments[] = {
      sender, gin::StringToV8(isolate_, channel), value, ports};
  std::ignore = callback.As<v8::Function>()->Call(context, context->Global(), 4,
                                                  arguments);
  if (caught.HasCaught() && !caught.HasTerminated()) {
    if (exception_handler_) {
      auto handler = exception_handler_;
      handler.Run(caught.Exception());
    } else {
      LOG(ERROR) << "Uncaught ipcMain postMessage listener: "
                 << *v8::String::Utf8Value(isolate_, caught.Exception());
    }
  }
  isolate_->PerformMicrotaskCheckpoint();
}
void MessagePortRuntime::Shutdown() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (stopped_) {
    return;
  }
  stopped_ = true;
  weak_factory_.InvalidateWeakPtrs();
  ports_.clear();
  post_.Reset();
  exception_handler_.Reset();
  can_post_.Reset();
  // An application getter currently being serialized may request shutdown.
  // Preserve this realm/brand until that native stack has unwound. Container
  // ownership retains this object through final V8 teardown.
}
}  // namespace xenon::ipc
