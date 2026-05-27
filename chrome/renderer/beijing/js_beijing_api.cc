// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/renderer/beijing/js_beijing_api.h"

#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "content/public/common/isolated_world_ids.h"
#include "content/public/renderer/v8_value_converter.h"
#include "gin/converter.h"
#include "gin/object_template_builder.h"
#include "mojo/public/cpp/bindings/associated_receiver.h"
#include "third_party/blink/public/platform/browser_interface_broker_proxy.h"
#include "third_party/blink/public/platform/scheduler/web_agent_group_scheduler.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "gin/persistent.h"
#include "v8/include/cppgc/allocation.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-function.h"
#include "v8/include/v8-microtask-queue.h"
#include "v8/include/v8-primitive.h"
#include "v8/include/v8-promise.h"

namespace beijing {

namespace {

constexpr char kGlobalApiName[] = "beijing";

void DefineGlobalReadOnly(v8::Isolate* isolate,
                          v8::Local<v8::Context> context,
                          v8::Local<v8::Object> global,
                          v8::Local<v8::String> name,
                          v8::Local<v8::Value> value) {
  v8::PropertyDescriptor desc(value, false);
  desc.set_configurable(false);
  desc.set_enumerable(true);
  global->DefineProperty(context, name, desc).Check();
}

// Encapsulates V8 Promise resolver boilerplates (holding persistent context, resolver handles,
// scopes, converter translations, and automatically freeing them when destroyed).
class BeijingPromiseResolver {
 public:
  BeijingPromiseResolver(v8::Isolate* isolate,
                         v8::Local<v8::Context> context,
                         v8::Local<v8::Promise::Resolver> resolver)
      : isolate_(isolate),
        context_(isolate, context),
        resolver_(isolate, resolver) {}

  ~BeijingPromiseResolver() {
    resolver_.Reset();
    context_.Reset();
  }

  BeijingPromiseResolver(const BeijingPromiseResolver&) = delete;
  BeijingPromiseResolver& operator=(const BeijingPromiseResolver&) = delete;

  BeijingPromiseResolver(BeijingPromiseResolver&&) = default;
  BeijingPromiseResolver& operator=(BeijingPromiseResolver&&) = default;

  v8::Isolate* isolate() const { return isolate_; }

  template <typename T>
  void Resolve(T value) {
    v8::HandleScope handle_scope(isolate_);
    v8::Local<v8::Context> context = context_.Get(isolate_);
    v8::Context::Scope context_scope(context);
    v8::MicrotasksScope microtasks(isolate_, context->GetMicrotaskQueue(),
                                   v8::MicrotasksScope::kDoNotRunMicrotasks);
    v8::Local<v8::Promise::Resolver> resolver = resolver_.Get(isolate_);
    resolver->Resolve(context, gin::Converter<T>::ToV8(isolate_, value)).Check();
  }

  void ResolveValue(content::V8ValueConverter* converter, const base::Value& value) {
    v8::HandleScope handle_scope(isolate_);
    v8::Local<v8::Context> context = context_.Get(isolate_);
    v8::Context::Scope context_scope(context);
    v8::MicrotasksScope microtasks(isolate_, context->GetMicrotaskQueue(),
                                   v8::MicrotasksScope::kDoNotRunMicrotasks);
    v8::Local<v8::Promise::Resolver> resolver = resolver_.Get(isolate_);
    v8::Local<v8::Value> v8_value = converter->ToV8Value(value, context);
    resolver->Resolve(context, v8_value).Check();
  }

  void Reject(const std::string& error_message) {
    v8::HandleScope handle_scope(isolate_);
    v8::Local<v8::Context> context = context_.Get(isolate_);
    v8::Context::Scope context_scope(context);
    v8::MicrotasksScope microtasks(isolate_, context->GetMicrotaskQueue(),
                                   v8::MicrotasksScope::kDoNotRunMicrotasks);
    v8::Local<v8::Promise::Resolver> resolver = resolver_.Get(isolate_);
    resolver->Reject(context, gin::StringToV8(isolate_, error_message)).Check();
  }

 private:
  raw_ptr<v8::Isolate> isolate_;
  v8::Global<v8::Context> context_;
  v8::Global<v8::Promise::Resolver> resolver_;
};

}  // namespace

// 管理单个主题订阅的辅助类
class JSBeijingApi::BeijingSubscription : public cppgc::GarbageCollected<BeijingSubscription>,
                                         public mojom::BeijingPageClient {
 public:
  BeijingSubscription(v8::Isolate* isolate,
                      content::V8ValueConverter* converter,
                      v8::Local<v8::Function> callback,
                      v8::Local<v8::Context> context)
      : isolate_(isolate),
        converter_(converter),
        callback_(isolate, callback),
        context_(isolate, context) {}

  ~BeijingSubscription() override {
    Disconnect();
  }

  void Trace(cppgc::Visitor* visitor) const {}

  mojo::PendingAssociatedRemote<mojom::BeijingPageClient> BindAndGetRemote() {
    return receiver_.BindNewEndpointAndPassRemote();
  }

  void Disconnect() {
    receiver_.reset();
    callback_.Reset();
    context_.Reset();
  }

  // mojom::BeijingPageClient 实现:
  void OnEvent(const std::string& topic, base::Value payload) override {
    if (context_.IsEmpty() || callback_.IsEmpty()) {
      return;
    }
    v8::HandleScope handle_scope(isolate_);
    v8::Local<v8::Context> context = context_.Get(isolate_);
    v8::Context::Scope context_scope(context);
    v8::MicrotasksScope microtasks(isolate_, context->GetMicrotaskQueue(),
                                   v8::MicrotasksScope::kDoNotRunMicrotasks);

    v8::Local<v8::Value> v8_payload =
        converter_->ToV8Value(payload, context);
    v8::Local<v8::Function> callback = callback_.Get(isolate_);

    v8::Local<v8::Value> argv[] = {v8_payload};
    v8::MaybeLocal<v8::Value> result =
        callback->Call(context, context->Global(), 1, argv);
    std::ignore = result;
  }

 private:
  raw_ptr<v8::Isolate> isolate_;
  raw_ptr<content::V8ValueConverter> converter_;
  v8::Global<v8::Function> callback_;
  v8::Global<v8::Context> context_;
  mojo::AssociatedReceiver<mojom::BeijingPageClient> receiver_{this};
};

// static
void JSBeijingApi::Install(content::RenderFrame* render_frame) {
  CHECK(render_frame);
  blink::WebLocalFrame* web_frame = render_frame->GetWebFrame();
  v8::Isolate* isolate = web_frame->GetAgentGroupScheduler()->Isolate();
  v8::HandleScope handle_scope(isolate);
  v8::Local<v8::Context> context = web_frame->MainWorldScriptContext();
  if (context.IsEmpty()) {
    return;
  }

  v8::MicrotasksScope microtasks(
      isolate, context->GetMicrotaskQueue(),
      v8::MicrotasksScope::kDoNotRunMicrotasks);
  v8::Context::Scope context_scope(context);

  v8::Local<v8::Object> global = context->Global();
  v8::Local<v8::Value> existing =
      global->Get(context, gin::StringToV8(isolate, kGlobalApiName))
          .ToLocalChecked();
  if (!existing->IsUndefined()) {
    return;
  }

  JSBeijingApi* api = cppgc::MakeGarbageCollected<JSBeijingApi>(
      isolate->GetCppHeap()->GetAllocationHandle(), render_frame);
  v8::Local<v8::Object> wrapper;
  if (!api->GetWrapper(isolate).ToLocal(&wrapper)) {
    return;
  }

  DefineGlobalReadOnly(isolate, context, global,
                       gin::StringToV8(isolate, kGlobalApiName), wrapper);
}

JSBeijingApi::JSBeijingApi(content::RenderFrame* render_frame)
    : content::RenderFrameObserver(render_frame),
      v8_value_converter_(content::V8ValueConverter::Create()) {}

JSBeijingApi::~JSBeijingApi() = default;

const gin::WrapperInfo* JSBeijingApi::wrapper_info() const {
  return &kWrapperInfo;
}

void JSBeijingApi::Trace(cppgc::Visitor* visitor) const {
  gin::Wrappable<JSBeijingApi>::Trace(visitor);
  visitor->Trace(weak_factory_);
  for (const auto& pair : subscriptions_) {
    for (const auto& sub : pair.second) {
      visitor->Trace(sub);
    }
  }
}

gin::ObjectTemplateBuilder JSBeijingApi::GetObjectTemplateBuilder(
    v8::Isolate* isolate) {
  return gin::Wrappable<JSBeijingApi>::GetObjectTemplateBuilder(isolate)
      .SetMethod("ping", &JSBeijingApi::Ping)
      .SetMethod("getApiVersion", &JSBeijingApi::GetApiVersion)
      .SetMethod("echoObject", &JSBeijingApi::EchoObject)
      .SetMethod("wrapObjectWithBrowserMeta", &JSBeijingApi::WrapObjectWithBrowserMeta)
      .SetMethod("subscribe", &JSBeijingApi::Subscribe)
      .SetMethod("unsubscribe", &JSBeijingApi::Unsubscribe)
      .SetMethod("publish", &JSBeijingApi::Publish);
}

void JSBeijingApi::WillReleaseScriptContext(v8::Local<v8::Context>,
                                          int32_t world_id) {
  if (world_id != content::ISOLATED_WORLD_ID_GLOBAL) {
    return;
  }
  for (auto& pair : subscriptions_) {
    for (auto& sub : pair.second) {
      if (sub) {
        sub->Disconnect();
      }
    }
  }
  subscriptions_.clear();
  weak_factory_.Invalidate();
  beijing_host_.reset();
}

void JSBeijingApi::OnDestruct() {
  for (auto& pair : subscriptions_) {
    for (auto& sub : pair.second) {
      if (sub) {
        sub->Disconnect();
      }
    }
  }
  subscriptions_.clear();
  weak_factory_.Invalidate();
  beijing_host_.reset();
}

bool JSBeijingApi::EnsureConnected() {
  if (!render_frame()) {
    return false;
  }
  if (!beijing_host_.is_bound() || !beijing_host_.is_connected()) {
    beijing_host_.reset();
    render_frame()->GetBrowserInterfaceBroker().GetInterface(
        beijing_host_.BindNewPipeAndPassReceiver());
  }
  return beijing_host_.is_bound();
}

void JSBeijingApi::Ping(gin::Arguments* args) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  v8::Isolate* isolate = args->isolate();
  v8::Local<v8::Context> context = args->GetHolderCreationContext();
  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(context).ToLocal(&resolver)) {
    return;
  }
  args->Return(resolver->GetPromise());

  if (!EnsureConnected() || !render_frame()) {
    resolver->Reject(context, gin::StringToV8(isolate, "Beijing: not connected")).Check();
    return;
  }

  auto promise_resolver = std::make_unique<BeijingPromiseResolver>(isolate, context, resolver);
  beijing_host_->Ping(base::BindOnce(
      [](JSBeijingApi* weak_this,
         std::unique_ptr<BeijingPromiseResolver> resolver,
         bool ok, const std::string& message) {
        if (!weak_this) return;
        if (ok) {
          resolver->Resolve(message);
        } else {
          resolver->Reject(message);
        }
      },
      gin::WrapPersistent(weak_factory_.GetWeakCell(isolate->GetCppHeap()->GetAllocationHandle())),
      std::move(promise_resolver)));
}

void JSBeijingApi::GetApiVersion(gin::Arguments* args) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  v8::Isolate* isolate = args->isolate();
  v8::Local<v8::Context> context = args->GetHolderCreationContext();
  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(context).ToLocal(&resolver)) {
    return;
  }
  args->Return(resolver->GetPromise());

  if (!EnsureConnected() || !render_frame()) {
    resolver->Reject(context, gin::StringToV8(isolate, "Beijing: not connected")).Check();
    return;
  }

  auto promise_resolver = std::make_unique<BeijingPromiseResolver>(isolate, context, resolver);
  beijing_host_->GetApiVersion(base::BindOnce(
      [](JSBeijingApi* weak_this,
         std::unique_ptr<BeijingPromiseResolver> resolver,
         const std::string& version) {
        if (!weak_this) return;
        resolver->Resolve(version);
      },
      gin::WrapPersistent(weak_factory_.GetWeakCell(isolate->GetCppHeap()->GetAllocationHandle())),
      std::move(promise_resolver)));
}

void JSBeijingApi::EchoObject(gin::Arguments* args) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  v8::Isolate* isolate = args->isolate();
  v8::Local<v8::Context> context = args->GetHolderCreationContext();
  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(context).ToLocal(&resolver)) {
    return;
  }
  args->Return(resolver->GetPromise());

  v8::Local<v8::Value> v8_arg;
  if (!args->GetNext(&v8_arg)) {
    resolver->Reject(context, gin::StringToV8(isolate, "Beijing: echoObject requires an argument")).Check();
    return;
  }

  if (!EnsureConnected() || !render_frame()) {
    resolver->Reject(context, gin::StringToV8(isolate, "Beijing: not connected")).Check();
    return;
  }

  std::unique_ptr<base::Value> input =
      v8_value_converter_->FromV8Value(v8_arg, context);
  if (!input) {
    resolver->Reject(context, gin::StringToV8(isolate, "Beijing: could not convert argument to Value")).Check();
    return;
  }

  auto promise_resolver = std::make_unique<BeijingPromiseResolver>(isolate, context, resolver);
  beijing_host_->EchoObject(
      std::move(*input),
      base::BindOnce(
          [](JSBeijingApi* weak_this,
             std::unique_ptr<BeijingPromiseResolver> resolver,
             base::Value result) {
            if (!weak_this) return;
            resolver->ResolveValue(weak_this->v8_value_converter_.get(), result);
          },
          gin::WrapPersistent(weak_factory_.GetWeakCell(isolate->GetCppHeap()->GetAllocationHandle())),
          std::move(promise_resolver)));
}

void JSBeijingApi::WrapObjectWithBrowserMeta(gin::Arguments* args) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  v8::Isolate* isolate = args->isolate();
  v8::Local<v8::Context> context = args->GetHolderCreationContext();
  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(context).ToLocal(&resolver)) {
    return;
  }
  args->Return(resolver->GetPromise());

  v8::Local<v8::Value> v8_arg;
  if (!args->GetNext(&v8_arg)) {
    resolver->Reject(context, gin::StringToV8(isolate, "Beijing: wrapObjectWithBrowserMeta requires an argument")).Check();
    return;
  }

  if (!EnsureConnected() || !render_frame()) {
    resolver->Reject(context, gin::StringToV8(isolate, "Beijing: not connected")).Check();
    return;
  }

  std::unique_ptr<base::Value> input =
      v8_value_converter_->FromV8Value(v8_arg, context);
  if (!input) {
    resolver->Reject(context, gin::StringToV8(isolate, "Beijing: could not convert argument to Value")).Check();
    return;
  }

  auto promise_resolver = std::make_unique<BeijingPromiseResolver>(isolate, context, resolver);
  beijing_host_->WrapObjectWithBrowserMeta(
      std::move(*input),
      base::BindOnce(
          [](JSBeijingApi* weak_this,
             std::unique_ptr<BeijingPromiseResolver> resolver,
             base::Value result) {
            if (!weak_this) return;
            resolver->ResolveValue(weak_this->v8_value_converter_.get(), result);
          },
          gin::WrapPersistent(weak_factory_.GetWeakCell(isolate->GetCppHeap()->GetAllocationHandle())),
          std::move(promise_resolver)));
}

void JSBeijingApi::Subscribe(gin::Arguments* args) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  v8::Isolate* isolate = args->isolate();
  v8::Local<v8::Context> context = args->GetHolderCreationContext();

  std::string topic;
  if (!args->GetNext(&topic)) {
    args->ThrowTypeError("Beijing: subscribe requires a string topic");
    return;
  }

  v8::Local<v8::Value> val;
  if (!args->GetNext(&val) || !val->IsFunction()) {
    args->ThrowTypeError("Beijing: subscribe requires a callback function");
    return;
  }
  v8::Local<v8::Function> callback = val.As<v8::Function>();

  if (!EnsureConnected() || !render_frame()) {
    args->ThrowTypeError("Beijing: not connected");
    return;
  }

  BeijingSubscription* subscription =
      cppgc::MakeGarbageCollected<BeijingSubscription>(
          isolate->GetCppHeap()->GetAllocationHandle(), isolate,
          v8_value_converter_.get(), callback, context);
  mojo::PendingAssociatedRemote<mojom::BeijingPageClient> client_remote =
      subscription->BindAndGetRemote();

  beijing_host_->Subscribe(topic, std::move(client_remote));
  subscriptions_[topic].push_back(subscription);
}

void JSBeijingApi::Unsubscribe(gin::Arguments* args) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  std::string topic;
  if (!args->GetNext(&topic)) {
    args->ThrowTypeError("Beijing: unsubscribe requires a string topic");
    return;
  }

  subscriptions_.erase(topic);
}

void JSBeijingApi::Publish(gin::Arguments* args) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  v8::Local<v8::Context> context = args->GetHolderCreationContext();

  std::string topic;
  if (!args->GetNext(&topic)) {
    args->ThrowTypeError("Beijing: publish requires a string topic");
    return;
  }

  v8::Local<v8::Value> v8_payload;
  if (!args->GetNext(&v8_payload)) {
    args->ThrowTypeError("Beijing: publish requires a payload");
    return;
  }

  if (!EnsureConnected() || !render_frame()) {
    args->ThrowTypeError("Beijing: not connected");
    return;
  }

  std::unique_ptr<base::Value> payload =
      v8_value_converter_->FromV8Value(v8_payload, context);
  if (!payload) {
    args->ThrowTypeError("Beijing: failed to convert payload to Value");
    return;
  }

  beijing_host_->Publish(topic, std::move(*payload));
}

}  // namespace beijing
