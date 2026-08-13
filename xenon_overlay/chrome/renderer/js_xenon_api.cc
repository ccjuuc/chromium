// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/renderer/js_xenon_api.h"

#include "base/check.h"
#include "base/functional/bind.h"
#include "content/public/common/isolated_world_ids.h"
#include "content/public/renderer/v8_value_converter.h"
#include "mojo/public/cpp/bindings/callback_helpers.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "v8/include/v8-json.h"
#include "gin/converter.h"
#include "gin/object_template_builder.h"
#include "gin/persistent.h"
#include "third_party/blink/public/platform/browser_interface_broker_proxy.h"
#include "third_party/blink/public/platform/scheduler/web_agent_group_scheduler.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "xenon_overlay/blink/renderer/core/frame/data_mask_applier.h"
#include "third_party/blink/renderer/core/frame/web_local_frame_impl.h"
#include "v8/include/cppgc/allocation.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-function.h"
#include "v8/include/v8-microtask-queue.h"
#include "v8/include/v8-primitive.h"
#include "v8/include/v8-promise.h"
#include "xenon_overlay/chrome/renderer/xenon_data_mask_js_conversions.h"

namespace xenon {

namespace {

constexpr char kGlobalApiName[] = "xenon";

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

struct PromiseResolverContext {
  v8::Global<v8::Context> context;
  mojom::XenonToolExecutor::ExecuteCallback callback;
};

void OnPromiseResolved(const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Isolate* isolate = info.GetIsolate();
  void* data = info.Data().As<v8::External>()->Value(
      v8::kExternalPointerTypeTagDefault);
  auto* resolver_ctx = static_cast<PromiseResolverContext*>(data);

  std::string result_str;
  if (info.Length() > 0 && gin::ConvertFromV8(isolate, info[0], &result_str)) {
    std::move(resolver_ctx->callback).Run(result_str);
  } else {
    std::move(resolver_ctx->callback).Run(std::nullopt);
  }
  delete resolver_ctx;
}

void OnPromiseRejected(const v8::FunctionCallbackInfo<v8::Value>& info) {
  void* data = info.Data().As<v8::External>()->Value(
      v8::kExternalPointerTypeTagDefault);
  auto* resolver_ctx = static_cast<PromiseResolverContext*>(data);
  std::move(resolver_ctx->callback).Run(std::nullopt);
  delete resolver_ctx;
}

class XenonToolExecutorImpl : public mojom::XenonToolExecutor {
 public:
  XenonToolExecutorImpl(v8::Isolate* isolate,
                        v8::Local<v8::Context> context,
                        v8::Local<v8::Function> callback)
      : isolate_(isolate),
        context_(isolate, context),
        callback_(isolate, callback) {}
  ~XenonToolExecutorImpl() override = default;

  void Execute(const std::string& input_json, ExecuteCallback callback) override {
    callback = mojo::WrapCallbackWithDefaultInvokeIfNotRun(std::move(callback),
                                                           std::nullopt);

    if (context_.IsEmpty() || callback_.IsEmpty()) {
      std::move(callback).Run(std::nullopt);
      return;
    }

    v8::HandleScope handle_scope(isolate_);
    v8::Local<v8::Context> context = context_.Get(isolate_);
    v8::Context::Scope context_scope(context);
    v8::MicrotasksScope microtasks(isolate_, context->GetMicrotaskQueue(),
                                   v8::MicrotasksScope::kDoNotRunMicrotasks);

    v8::Local<v8::Function> js_callback = callback_.Get(isolate_);
    v8::Local<v8::Value> argv[1];
    argv[0] = gin::StringToV8(isolate_, input_json);

    v8::TryCatch try_catch(isolate_);
    v8::MaybeLocal<v8::Value> maybe_result =
        js_callback->Call(context, context->Global(), 1, argv);

    if (try_catch.HasCaught() || maybe_result.IsEmpty()) {
      std::move(callback).Run(std::nullopt);
      return;
    }

    v8::Local<v8::Value> result = maybe_result.ToLocalChecked();
    if (result->IsPromise()) {
      v8::Local<v8::Promise> promise = result.As<v8::Promise>();
      auto* resolver_ctx = new PromiseResolverContext{
          v8::Global<v8::Context>(isolate_, context), std::move(callback)};
      v8::Local<v8::External> data_ext = v8::External::New(
          isolate_, resolver_ctx, v8::kExternalPointerTypeTagDefault);
      v8::Local<v8::Function> resolved_fn;
      v8::Local<v8::Function> rejected_fn;
      if (!v8::Function::New(context, OnPromiseResolved, data_ext)
               .ToLocal(&resolved_fn) ||
          !v8::Function::New(context, OnPromiseRejected, data_ext)
               .ToLocal(&rejected_fn)) {
        std::move(resolver_ctx->callback).Run(std::nullopt);
        delete resolver_ctx;
        return;
      }
      if (promise->Then(context, resolved_fn, rejected_fn).IsEmpty()) {
        std::move(resolver_ctx->callback).Run(std::nullopt);
        delete resolver_ctx;
      }
    } else {
      std::string result_str;
      if (gin::ConvertFromV8(isolate_, result, &result_str)) {
        std::move(callback).Run(result_str);
      } else {
        std::move(callback).Run(std::nullopt);
      }
    }
  }

 private:
  raw_ptr<v8::Isolate> isolate_;
  v8::Global<v8::Context> context_;
  v8::Global<v8::Function> callback_;
};

}  // namespace

// static
void JSXenonApi::Install(content::RenderFrame* render_frame) {
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

  JSXenonApi* api = cppgc::MakeGarbageCollected<JSXenonApi>(
      isolate->GetCppHeap()->GetAllocationHandle(), render_frame);
  v8::Local<v8::Object> wrapper;
  if (!api->GetWrapper(isolate).ToLocal(&wrapper)) {
    return;
  }

  DefineGlobalReadOnly(isolate, context, global,
                       gin::StringToV8(isolate, kGlobalApiName), wrapper);
}

JSXenonApi::JSXenonApi(content::RenderFrame* render_frame)
    : content::RenderFrameObserver(render_frame) {}

JSXenonApi::~JSXenonApi() = default;

const gin::WrapperInfo* JSXenonApi::wrapper_info() const {
  return &kWrapperInfo;
}

void JSXenonApi::Trace(cppgc::Visitor* visitor) const {
  gin::Wrappable<JSXenonApi>::Trace(visitor);
  visitor->Trace(weak_factory_);
}

gin::ObjectTemplateBuilder JSXenonApi::GetObjectTemplateBuilder(
    v8::Isolate* isolate) {
  return gin::Wrappable<JSXenonApi>::GetObjectTemplateBuilder(isolate)
      .SetValue("name", std::string_view("Xenon"))
      .SetMethod("ping", &JSXenonApi::Ping)
      .SetMethod("getApiVersion", &JSXenonApi::GetApiVersion)
      .SetMethod("echoObject", &JSXenonApi::EchoObject)
      .SetMethod("wrapObjectWithBrowserMeta", &JSXenonApi::WrapObjectWithBrowserMeta)
      .SetMethod("registerTool", &JSXenonApi::RegisterTool)
      .SetMethod("sendDataMaskRules", &JSXenonApi::SendDataMaskRules)
      .SetMethod("sendDataMaskXPath", &JSXenonApi::SendDataMaskXPath)
      .SetMethod("sendDataMaskToMain", &JSXenonApi::SendDataMaskToMain);
}

void JSXenonApi::WillReleaseScriptContext(v8::Local<v8::Context>,
                                          int32_t world_id) {
  if (world_id != content::ISOLATED_WORLD_ID_GLOBAL) {
    return;
  }
  weak_factory_.Invalidate();
  xenon_host_.reset();
  blink_data_mask_to_main_.reset();
}

void JSXenonApi::OnDestruct() {
  weak_factory_.Invalidate();
  xenon_host_.reset();
  blink_data_mask_to_main_.reset();
}

bool JSXenonApi::EnsureConnected() {
  if (!render_frame()) {
    return false;
  }
  if (!xenon_host_.is_bound()) {
    render_frame()->GetBrowserInterfaceBroker().GetInterface(
        xenon_host_.BindNewPipeAndPassReceiver());
  }
  return xenon_host_.is_bound();
}

bool JSXenonApi::EnsureBlinkDataMaskToMainConnected() {
  if (!render_frame()) {
    return false;
  }
  if (!blink_data_mask_to_main_.is_bound()) {
    render_frame()->GetBrowserInterfaceBroker().GetInterface(
        blink_data_mask_to_main_.BindNewPipeAndPassReceiver());
  }
  return blink_data_mask_to_main_.is_bound();
}

void JSXenonApi::Ping(gin::Arguments* args) {
  v8::Isolate* isolate = args->isolate();
  v8::Local<v8::Context> context = args->GetHolderCreationContext();
  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(context).ToLocal(&resolver)) {
    return;
  }

  if (!EnsureConnected() || !render_frame()) {
    resolver->Reject(context, gin::StringToV8(isolate, "Xenon: not connected"))
        .Check();
    args->Return(resolver->GetPromise());
    return;
  }

  v8::Global<v8::Promise::Resolver> resolver_global(isolate, resolver);
  v8::Global<v8::Context> global_context(isolate, context);

  // Frame-scoped BrowserInterfaceBroker remotes dispatch replies on this frame's
  // runner (same as Brave `JSEthereumProvider` — no BindPostTask).
  xenon_host_->Ping(base::BindOnce(
      &JSXenonApi::OnPing,
      gin::WrapPersistent(weak_factory_.GetWeakCell(
          isolate->GetCppHeap()->GetAllocationHandle())),
      std::move(global_context), std::move(resolver_global), isolate));

  args->Return(resolver->GetPromise());
}

void JSXenonApi::GetApiVersion(gin::Arguments* args) {
  v8::Isolate* isolate = args->isolate();
  v8::Local<v8::Context> context = args->GetHolderCreationContext();
  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(context).ToLocal(&resolver)) {
    return;
  }

  if (!EnsureConnected() || !render_frame()) {
    resolver->Reject(context, gin::StringToV8(isolate, "Xenon: not connected"))
        .Check();
    args->Return(resolver->GetPromise());
    return;
  }

  v8::Global<v8::Promise::Resolver> resolver_global(isolate, resolver);
  v8::Global<v8::Context> global_context(isolate, context);

  xenon_host_->GetApiVersion(base::BindOnce(
      &JSXenonApi::OnGetApiVersion,
      gin::WrapPersistent(weak_factory_.GetWeakCell(
          isolate->GetCppHeap()->GetAllocationHandle())),
      std::move(global_context), std::move(resolver_global), isolate));

  args->Return(resolver->GetPromise());
}

void JSXenonApi::EchoObject(gin::Arguments* args) {
  v8::Isolate* isolate = args->isolate();
  v8::Local<v8::Context> context = args->GetHolderCreationContext();
  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(context).ToLocal(&resolver)) {
    return;
  }

  v8::Local<v8::Value> v8_arg;
  if (!args->GetNext(&v8_arg)) {
    resolver->Reject(context, gin::StringToV8(isolate, "Xenon: echoObject requires an argument"))
        .Check();
    args->Return(resolver->GetPromise());
    return;
  }

  if (!EnsureConnected() || !render_frame()) {
    resolver->Reject(context, gin::StringToV8(isolate, "Xenon: not connected")).Check();
    args->Return(resolver->GetPromise());
    return;
  }

  std::unique_ptr<base::Value> input =
      content::V8ValueConverter::Create()->FromV8Value(v8_arg, context);
  if (!input) {
    resolver->Reject(context,
                     gin::StringToV8(isolate, "Xenon: could not convert argument to Value"))
        .Check();
    args->Return(resolver->GetPromise());
    return;
  }

  v8::Global<v8::Promise::Resolver> resolver_global(isolate, resolver);
  v8::Global<v8::Context> global_context(isolate, context);

  xenon_host_->EchoObject(
      std::move(*input),
      base::BindOnce(
          &JSXenonApi::OnEchoObject,
          gin::WrapPersistent(weak_factory_.GetWeakCell(
              isolate->GetCppHeap()->GetAllocationHandle())),
          std::move(global_context), std::move(resolver_global), isolate));

  args->Return(resolver->GetPromise());
}

void JSXenonApi::WrapObjectWithBrowserMeta(gin::Arguments* args) {
  v8::Isolate* isolate = args->isolate();
  v8::Local<v8::Context> context = args->GetHolderCreationContext();
  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(context).ToLocal(&resolver)) {
    return;
  }

  v8::Local<v8::Value> v8_arg;
  if (!args->GetNext(&v8_arg)) {
    resolver
        ->Reject(context,
                 gin::StringToV8(isolate,
                                  "Xenon: wrapObjectWithBrowserMeta requires an argument"))
        .Check();
    args->Return(resolver->GetPromise());
    return;
  }

  if (!EnsureConnected() || !render_frame()) {
    resolver->Reject(context, gin::StringToV8(isolate, "Xenon: not connected")).Check();
    args->Return(resolver->GetPromise());
    return;
  }

  std::unique_ptr<base::Value> input =
      content::V8ValueConverter::Create()->FromV8Value(v8_arg, context);
  if (!input) {
    resolver->Reject(context,
                     gin::StringToV8(isolate, "Xenon: could not convert argument to Value"))
        .Check();
    args->Return(resolver->GetPromise());
    return;
  }

  v8::Global<v8::Promise::Resolver> resolver_global(isolate, resolver);
  v8::Global<v8::Context> global_context(isolate, context);

  xenon_host_->WrapObjectWithBrowserMeta(
      std::move(*input),
      base::BindOnce(
          &JSXenonApi::OnWrapObjectWithBrowserMeta,
          gin::WrapPersistent(weak_factory_.GetWeakCell(
              isolate->GetCppHeap()->GetAllocationHandle())),
          std::move(global_context), std::move(resolver_global), isolate));

  args->Return(resolver->GetPromise());
}

void JSXenonApi::SendDataMaskRules(gin::Arguments* args) {
  v8::Isolate* isolate = args->isolate();
  v8::Local<v8::Context> context = args->GetHolderCreationContext();
  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(context).ToLocal(&resolver)) {
    return;
  }

  v8::Local<v8::Value> request_v8;
  v8::Local<v8::Value> rules_v8;
  if (!args->GetNext(&request_v8) || !args->GetNext(&rules_v8)) {
    resolver
        ->Reject(
            context,
            gin::StringToV8(isolate,
                             "Xenon: sendDataMaskRules(requestId, rulesObject)"))
        .Check();
    args->Return(resolver->GetPromise());
    return;
  }

  int32_t request_id = 0;
  if (!gin::ConvertFromV8(isolate, request_v8, &request_id)) {
    resolver
        ->Reject(context,
                 gin::StringToV8(isolate, "Xenon: requestId must be an integer"))
        .Check();
    args->Return(resolver->GetPromise());
    return;
  }

  if (!render_frame() || !render_frame()->GetWebFrame()) {
    resolver->Reject(context, gin::StringToV8(isolate, "Xenon: no local frame"))
        .Check();
    args->Return(resolver->GetPromise());
    return;
  }

  std::unique_ptr<base::Value> rules_value =
      content::V8ValueConverter::Create()->FromV8Value(rules_v8, context);
  if (!rules_value) {
    resolver->Reject(context,
                     gin::StringToV8(isolate, "Xenon: could not convert rules to Value"))
        .Check();
    args->Return(resolver->GetPromise());
    return;
  }

  blink::mojom::blink::DataMaskRulesPtr rules;
  if (!BuildDataMaskRulesFromValue(*rules_value, &rules)) {
    resolver
        ->Reject(context,
                 gin::StringToV8(
                     isolate,
                     "Xenon: rules must be { maskItems: [...] } or a bare array"))
        .Check();
    args->Return(resolver->GetPromise());
    return;
  }

  auto* web_impl =
      static_cast<blink::WebLocalFrameImpl*>(render_frame()->GetWebFrame());
  blink::LocalFrame* local_frame = web_impl->GetFrame();
  if (!local_frame) {
    resolver->Reject(context, gin::StringToV8(isolate, "Xenon: LocalFrame is null"))
        .Check();
    args->Return(resolver->GetPromise());
    return;
  }
  // Align with `blink::mojom::DataMask::SendData`; no JS acknowledgement yet.
  static_cast<void>(request_id);

  local_frame->SetDataMaskRules(std::move(rules));
  blink::ApplyDataMaskForLocalFrame(*local_frame);

  v8::MicrotasksScope microtasks(isolate, context->GetMicrotaskQueue(),
                                 v8::MicrotasksScope::kDoNotRunMicrotasks);
  resolver->Resolve(context, v8::Undefined(isolate)).Check();
  args->Return(resolver->GetPromise());
}

void JSXenonApi::SendDataMaskXPath(gin::Arguments* args) {
  v8::Isolate* isolate = args->isolate();
  v8::Local<v8::Context> context = args->GetHolderCreationContext();
  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(context).ToLocal(&resolver)) {
    return;
  }

  v8::Local<v8::Value> config_v8;
  if (!args->GetNext(&config_v8)) {
    resolver
        ->Reject(context,
                 gin::StringToV8(isolate, "Xenon: sendDataMaskXPath(configObject)"))
        .Check();
    args->Return(resolver->GetPromise());
    return;
  }

  if (!render_frame() || !render_frame()->GetWebFrame()) {
    resolver->Reject(context, gin::StringToV8(isolate, "Xenon: no local frame"))
        .Check();
    args->Return(resolver->GetPromise());
    return;
  }

  std::unique_ptr<base::Value> config_value =
      content::V8ValueConverter::Create()->FromV8Value(config_v8, context);
  if (!config_value) {
    resolver->Reject(context,
                     gin::StringToV8(isolate, "Xenon: could not convert config to Value"))
        .Check();
    args->Return(resolver->GetPromise());
    return;
  }

  blink::mojom::blink::XPathConfigPtr config;
  if (!BuildXPathConfigFromValue(*config_value, &config)) {
    resolver->Reject(context,
                     gin::StringToV8(isolate, "Xenon: xpath config must be an object"))
        .Check();
    args->Return(resolver->GetPromise());
    return;
  }

  auto* web_impl =
      static_cast<blink::WebLocalFrameImpl*>(render_frame()->GetWebFrame());
  blink::LocalFrame* local_frame = web_impl->GetFrame();
  if (!local_frame) {
    resolver->Reject(context, gin::StringToV8(isolate, "Xenon: LocalFrame is null"))
        .Check();
    args->Return(resolver->GetPromise());
    return;
  }
  local_frame->SetDataMaskXPathConfig(std::move(config));
  blink::ApplyDataMaskForLocalFrame(*local_frame);

  v8::MicrotasksScope microtasks(isolate, context->GetMicrotaskQueue(),
                                 v8::MicrotasksScope::kDoNotRunMicrotasks);
  resolver->Resolve(context, v8::Undefined(isolate)).Check();
  args->Return(resolver->GetPromise());
}

void JSXenonApi::SendDataMaskToMain(gin::Arguments* args) {
  v8::Isolate* isolate = args->isolate();
  v8::Local<v8::Context> context = args->GetHolderCreationContext();
  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(context).ToLocal(&resolver)) {
    return;
  }

  v8::Local<v8::Value> request_v8;
  v8::Local<v8::Value> data_v8;
  if (!args->GetNext(&request_v8) || !args->GetNext(&data_v8)) {
    resolver
        ->Reject(context,
                 gin::StringToV8(isolate,
                                 "Xenon: sendDataMaskToMain(requestId, dataString)"))
        .Check();
    args->Return(resolver->GetPromise());
    return;
  }

  int32_t request_id = 0;
  if (!gin::ConvertFromV8(isolate, request_v8, &request_id)) {
    resolver
        ->Reject(context,
                 gin::StringToV8(isolate, "Xenon: requestId must be an integer"))
        .Check();
    args->Return(resolver->GetPromise());
    return;
  }

  std::string data;
  if (!gin::ConvertFromV8(isolate, data_v8, &data)) {
    resolver->Reject(context,
                     gin::StringToV8(isolate, "Xenon: data must be a string"))
        .Check();
    args->Return(resolver->GetPromise());
    return;
  }

  if (!EnsureBlinkDataMaskToMainConnected()) {
    resolver->Reject(
            context, gin::StringToV8(isolate,
                                     "Xenon: blink::mojom::DataMaskToMain not connected"))
        .Check();
    args->Return(resolver->GetPromise());
    return;
  }

  blink_data_mask_to_main_->SendDataToMain(request_id, data);

  v8::MicrotasksScope microtasks(isolate, context->GetMicrotaskQueue(),
                                 v8::MicrotasksScope::kDoNotRunMicrotasks);
  resolver->Resolve(context, v8::Undefined(isolate)).Check();
  args->Return(resolver->GetPromise());
}

void JSXenonApi::OnPing(v8::Global<v8::Context> global_context,
                        v8::Global<v8::Promise::Resolver> resolver_global,
                        v8::Isolate* isolate,
                        bool ok,
                        const std::string& message) {
  v8::HandleScope handle_scope(isolate);
  v8::Local<v8::Context> context = global_context.Get(isolate);
  v8::Context::Scope context_scope(context);
  // Align with Brave wallet `JSEthereumProvider::SendResponse`: kScoped
  // isolates require an active MicrotasksScope; use kDoNotRunMicrotasks when
  // resolving/rejecting so microtasks flush with the embedder checkpoint.
  v8::MicrotasksScope microtasks(isolate, context->GetMicrotaskQueue(),
                                   v8::MicrotasksScope::kDoNotRunMicrotasks);
  v8::Local<v8::Promise::Resolver> resolver = resolver_global.Get(isolate);

  if (!render_frame()) {
    resolver->Reject(context, gin::StringToV8(isolate, "Xenon: frame detached"))
        .Check();
    return;
  }

  if (ok) {
    resolver->Resolve(context, gin::StringToV8(isolate, message)).Check();
  } else {
    resolver->Reject(context, gin::StringToV8(isolate, message)).Check();
  }
}

void JSXenonApi::OnGetApiVersion(v8::Global<v8::Context> global_context,
                                 v8::Global<v8::Promise::Resolver> resolver_global,
                                 v8::Isolate* isolate,
                                 const std::string& version) {
  v8::HandleScope handle_scope(isolate);
  v8::Local<v8::Context> context = global_context.Get(isolate);
  v8::Context::Scope context_scope(context);
  v8::MicrotasksScope microtasks(isolate, context->GetMicrotaskQueue(),
                                   v8::MicrotasksScope::kDoNotRunMicrotasks);
  v8::Local<v8::Promise::Resolver> resolver = resolver_global.Get(isolate);

  if (!render_frame()) {
    resolver->Reject(context, gin::StringToV8(isolate, "Xenon: frame detached"))
        .Check();
    return;
  }

  resolver->Resolve(context, gin::StringToV8(isolate, version)).Check();
}

void JSXenonApi::OnEchoObject(v8::Global<v8::Context> global_context,
                              v8::Global<v8::Promise::Resolver> resolver_global,
                              v8::Isolate* isolate,
                              base::Value result) {
  v8::HandleScope handle_scope(isolate);
  v8::Local<v8::Context> context = global_context.Get(isolate);
  v8::Context::Scope context_scope(context);
  v8::MicrotasksScope microtasks(isolate, context->GetMicrotaskQueue(),
                                 v8::MicrotasksScope::kDoNotRunMicrotasks);
  v8::Local<v8::Promise::Resolver> resolver = resolver_global.Get(isolate);

  if (!render_frame()) {
    resolver->Reject(context, gin::StringToV8(isolate, "Xenon: frame detached")).Check();
    return;
  }

  v8::Local<v8::Value> v8_result =
      content::V8ValueConverter::Create()->ToV8Value(result, context);
  resolver->Resolve(context, v8_result).Check();
}

void JSXenonApi::OnWrapObjectWithBrowserMeta(
    v8::Global<v8::Context> global_context,
    v8::Global<v8::Promise::Resolver> resolver_global,
    v8::Isolate* isolate,
    base::Value result) {
  OnEchoObject(std::move(global_context), std::move(resolver_global), isolate,
               std::move(result));
}

void JSXenonApi::RegisterTool(gin::Arguments* args) {
  v8::Isolate* isolate = args->isolate();
  v8::Local<v8::Context> context = args->GetHolderCreationContext();
  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(context).ToLocal(&resolver)) {
    return;
  }

  std::string name;
  std::string description;
  v8::Local<v8::Value> schema_val;
  v8::Local<v8::Function> callback_fn;

  if (!args->GetNext(&name) || !args->GetNext(&description) ||
      !args->GetNext(&schema_val) || !args->GetNext(&callback_fn)) {
    resolver->Reject(context, gin::StringToV8(isolate, "Xenon: Invalid arguments for registerTool"))
        .Check();
    args->Return(resolver->GetPromise());
    return;
  }

  if (!EnsureConnected() || !render_frame()) {
    resolver->Reject(context, gin::StringToV8(isolate, "Xenon: not connected")).Check();
    args->Return(resolver->GetPromise());
    return;
  }

  std::string schema_str;
  if (schema_val->IsObject()) {
    v8::TryCatch try_catch(isolate);
    v8::Local<v8::Object> schema_obj = schema_val.As<v8::Object>();
    v8::MaybeLocal<v8::String> maybe_json = v8::JSON::Stringify(context, schema_obj);
    if (try_catch.HasCaught() || maybe_json.IsEmpty() ||
        !gin::ConvertFromV8(isolate, maybe_json.ToLocalChecked(), &schema_str)) {
      resolver->Reject(context, gin::StringToV8(isolate, "Xenon: Failed to stringify schema")).Check();
      args->Return(resolver->GetPromise());
      return;
    }
  } else if (!gin::ConvertFromV8(isolate, schema_val, &schema_str)) {
    resolver->Reject(context, gin::StringToV8(isolate, "Xenon: schema must be an object or a string")).Check();
    args->Return(resolver->GetPromise());
    return;
  }

  mojo::PendingRemote<mojom::XenonToolExecutor> pending_remote;
  mojo::MakeSelfOwnedReceiver(
      std::make_unique<XenonToolExecutorImpl>(isolate, context, callback_fn),
      pending_remote.InitWithNewPipeAndPassReceiver());

  v8::Global<v8::Promise::Resolver> resolver_global(isolate, resolver);
  v8::Global<v8::Context> global_context(isolate, context);

  xenon_host_->RegisterTool(
      name, description, schema_str, std::move(pending_remote),
      base::BindOnce(
          &JSXenonApi::OnRegisterTool,
          gin::WrapPersistent(weak_factory_.GetWeakCell(
              isolate->GetCppHeap()->GetAllocationHandle())),
          std::move(global_context), std::move(resolver_global), isolate));

  args->Return(resolver->GetPromise());
}

void JSXenonApi::OnRegisterTool(v8::Global<v8::Context> global_context,
                                v8::Global<v8::Promise::Resolver> resolver_global,
                                v8::Isolate* isolate,
                                bool success) {
  v8::HandleScope handle_scope(isolate);
  v8::Local<v8::Context> context = global_context.Get(isolate);
  v8::Context::Scope context_scope(context);
  v8::MicrotasksScope microtasks(isolate, context->GetMicrotaskQueue(),
                                 v8::MicrotasksScope::kDoNotRunMicrotasks);
  v8::Local<v8::Promise::Resolver> resolver = resolver_global.Get(isolate);

  if (!render_frame()) {
    resolver->Reject(context, gin::StringToV8(isolate, "Xenon: frame detached")).Check();
    return;
  }

  resolver->Resolve(context, v8::Boolean::New(isolate, success)).Check();
}

}  // namespace xenon
