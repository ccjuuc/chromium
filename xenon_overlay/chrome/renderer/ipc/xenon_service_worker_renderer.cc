// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/renderer/ipc/xenon_service_worker_renderer.h"

#include <iterator>
#include <limits>
#include <string_view>
#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "gin/converter.h"
#include "gin/function_template.h"
#include "mojo/public/cpp/bindings/callback_helpers.h"
#include "third_party/blink/public/web/modules/service_worker/web_service_worker_context_proxy.h"
#include "ui/base/resource/resource_bundle.h"
#include "v8/include/v8.h"
#include "xenon_overlay/chrome/renderer/ipc/xenon_context_bridge.h"
#include "xenon_overlay/common/ipc/xenon_ipc_value_codec.h"
#include "xenon_overlay/resources/grit/xenon_resources.h"

namespace xenon::ipc {

namespace {

// Explicitly released by the worker-context destruction hook before V8 dies.
thread_local XenonServiceWorkerRenderer* worker_binding = nullptr;

// Blink callbacks must be created in a context with a ScriptState. The preload
// is a raw V8 context, so both the receiver and callbacks passed into Blink
// stay in the worker's main realm; ordinary JS calls enter the preload
// afterwards.
constexpr char kWorkerSchedulerSource[] = R"JS(
(() => {
  'use strict';
  const apply = Reflect.apply;
  const keys = Object.keys;
  const nativeSetTimeout = setTimeout.bind(globalThis);
  const nativeSetInterval = setInterval.bind(globalThis);
  const nativeClearTimeout = clearTimeout.bind(globalThis);
  const nativeClearInterval = clearInterval.bind(globalThis);
  const nativeQueueMicrotask = queueMicrotask.bind(globalThis);
  const timers = Object.create(null);
  let revoked = false;
  function validate(callback) {
    if (revoked) throw new Error('Service worker preload was destroyed');
    if (typeof callback !== 'function') throw new TypeError('Callback must be a function');
  }
  return preloadGlobal => {
    function schedule(repeat, callback, delay, args) {
      validate(callback);
      const id = (repeat ? nativeSetInterval : nativeSetTimeout)(() => {
        if (!repeat) delete timers[id];
        if (!revoked) apply(callback, preloadGlobal, args);
      }, delay);
      timers[id] = true;
      return id;
    }
    return {
      setTimeout(callback, delay, ...args) {
        return schedule(false, callback, delay, args);
      },
      setInterval(callback, delay, ...args) {
        return schedule(true, callback, delay, args);
      },
      clearTimeout(id) {
        // Never cancel unrelated timers installed by the worker main world.
        if (typeof id === 'number' && timers[id]) {
          delete timers[id];
          nativeClearTimeout(id);
        }
      },
      clearInterval(id) {
        if (typeof id === 'number' && timers[id]) {
          delete timers[id];
          nativeClearInterval(id);
        }
      },
      queueMicrotask(callback) {
        validate(callback);
        nativeQueueMicrotask(() => {
          if (!revoked) apply(callback, undefined, []);
        });
      },
      revoke() {
        revoked = true;
        const ids = keys(timers);
        for (let i = 0; i < ids.length; ++i) {
          nativeClearTimeout(ids[i]);
          delete timers[ids[i]];
        }
      },
    };
  };
})()
)JS";

mojom::IpcResultPtr DisconnectedResult() {
  auto result = mojom::IpcResult::New();
  result->success = false;
  result->error = "Service worker IPC was disconnected";
  return result;
}

std::string ExceptionText(v8::Isolate* isolate, v8::TryCatch& caught) {
  v8::String::Utf8Value message(isolate, caught.Exception());
  return *message ? std::string(*message, message.length())
                  : "Context bridge call failed";
}

}  // namespace

// static
void XenonServiceWorkerRenderer::Install(
    blink::WebServiceWorkerContextProxy* context_proxy,
    v8::Local<v8::Context> main_context) {
  if (worker_binding || !context_proxy || main_context.IsEmpty()) {
    return;
  }
  auto binding = base::MakeRefCounted<XenonServiceWorkerRenderer>(main_context);
  if (!binding->Initialize(context_proxy)) {
    binding->Shutdown();
    return;
  }
  worker_binding = binding.get();
  worker_binding->AddRef();
}

// static
void XenonServiceWorkerRenderer::Destroy() {
  if (worker_binding) {
    auto* binding = std::exchange(worker_binding, nullptr);
    binding->Shutdown();
    binding->Release();
  }
}

XenonServiceWorkerRenderer::XenonServiceWorkerRenderer(
    v8::Local<v8::Context> main_context)
    : isolate_(v8::Isolate::GetCurrent()),
      main_context_(isolate_, main_context) {}

XenonServiceWorkerRenderer::~XenonServiceWorkerRenderer() = default;

v8::Local<v8::Function> XenonServiceWorkerRenderer::BindFunction(
    auto method,
    auto... bound_args) {
  return gin::CreateFunctionTemplate(
             isolate_,
             base::BindRepeating(
                 method, scoped_refptr<XenonServiceWorkerRenderer>(this),
                 bound_args...))
      ->GetFunction(isolate_->GetCurrentContext())
      .ToLocalChecked();
}

bool XenonServiceWorkerRenderer::Initialize(
    blink::WebServiceWorkerContextProxy* context_proxy) {
  v8::HandleScope handles(isolate_);
  auto main = main_context_.Get(isolate_);
  v8::Context::Scope main_scope(main);
  // Bind through the public worker interface before its script runs. The
  // proxy is only used here; the associated pipe owns the transport lifetime.
  context_proxy->GetRemoteAssociatedInterface(
      host_.BindNewEndpointAndPassReceiver());
  mojom::IpcServiceWorkerConfigPtr config;
  if (!host_->Initialize(receiver_.BindNewPipeAndPassRemote(), &config) ||
      !config || config->preloads.empty()) {
    return false;
  }
  host_.set_disconnect_handler(base::BindOnce(
      &XenonServiceWorkerRenderer::OnDisconnected, weak_factory_.GetWeakPtr()));

  // Sharing the worker's microtask queue lets Blink perform checkpoints, while
  // separate globals and copied bridge values preserve the preload boundary.
  auto preload = v8::Context::New(isolate_, nullptr, {}, {},
                                  v8::DeserializeInternalFieldsCallback(),
                                  main->GetMicrotaskQueue());
  preload_context_.Reset(isolate_, preload);
  v8::Context::Scope preload_scope(preload);
  v8::MicrotasksScope microtasks(isolate_, main->GetMicrotaskQueue(),
                                 v8::MicrotasksScope::kDoNotRunMicrotasks);
  auto native = v8::Object::New(isolate_);
  auto method = [&](const char* name, auto function) {
    native
        ->CreateDataProperty(preload, gin::StringToV8(isolate_, name),
                             BindFunction(function))
        .Check();
  };
  method("send", &XenonServiceWorkerRenderer::Send);
  method("invoke", &XenonServiceWorkerRenderer::Invoke);
  method("setDispatchHandler", &XenonServiceWorkerRenderer::SetDispatchHandler);
  bridge_ = XenonContextBridge::Create(preload, main);
  native
      ->CreateDataProperty(preload, gin::StringToV8(isolate_, "contextBridge"),
                           bridge_->GetApi())
      .Check();
  preload->Global()
      ->CreateDataProperty(preload, gin::StringToV8(isolate_, "__xenonWorker"),
                           native)
      .Check();

  // These browser primitives are available only to trusted preload code. None
  // of its functions or objects are handed directly to the worker main world.
  for (const char* name :
       {"console", "URL", "URLSearchParams", "TextEncoder", "TextDecoder"}) {
    v8::Local<v8::Value> value;
    if (main->Global()
            ->Get(main, gin::StringToV8(isolate_, name))
            .ToLocal(&value)) {
      preload->Global()
          ->CreateDataProperty(preload, gin::StringToV8(isolate_, name), value)
          .Check();
    }
  }
  if (!InstallScheduler(main, preload)) {
    return false;
  }
  if (!ui::ResourceBundle::HasSharedInstance() ||
      !RunScript(preload,
                 ui::ResourceBundle::GetSharedInstance().LoadDataResourceString(
                     IDR_XENON_IPC_WORKER_BOOTSTRAP_JS),
                 "xenon-service-worker-preload-bootstrap.js")) {
    return false;
  }
  for (const auto& script : config->preloads) {
    // CommonJS lexical scope keeps one preload's local names out of the next.
    const std::string source =
        "(function(exports, require, module, __filename, __dirname) {\n" +
        script->source + "\n})";
    v8::TryCatch caught(isolate_);
    v8::ScriptOrigin origin(gin::StringToV8(isolate_, script->file_path));
    v8::Local<v8::Script> compiled;
    v8::Local<v8::Value> wrapper;
    auto module = v8::Object::New(isolate_);
    auto exports = v8::Object::New(isolate_);
    module
        ->CreateDataProperty(preload, gin::StringToV8(isolate_, "exports"),
                             exports)
        .Check();
    v8::Local<v8::Value> require;
    if (!preload->Global()
             ->Get(preload, gin::StringToV8(isolate_, "require"))
             .ToLocal(&require)) {
      return false;
    }
    const size_t separator = script->file_path.find_last_of("/\\");
    v8::Local<v8::Value> arguments[] = {
        exports, require, module, gin::StringToV8(isolate_, script->file_path),
        gin::StringToV8(isolate_,
                        separator == std::string::npos
                            ? std::string()
                            : script->file_path.substr(0, separator))};
    if (!v8::Script::Compile(preload,
                             gin::StringToV8(isolate_, source).As<v8::String>(),
                             &origin)
             .ToLocal(&compiled) ||
        !compiled->Run(preload).ToLocal(&wrapper) || !wrapper->IsFunction() ||
        wrapper.As<v8::Function>()
            ->Call(preload, exports, std::size(arguments), arguments)
            .IsEmpty()) {
      LOG(ERROR) << "Service worker preload failed: " << script->file_path
                 << " "
                 << (caught.HasCaught() ? ExceptionText(isolate_, caught)
                                        : "invalid module");
    }
  }
  return true;
}

bool XenonServiceWorkerRenderer::InstallScheduler(
    v8::Local<v8::Context> main,
    v8::Local<v8::Context> preload) {
  v8::Context::Scope scope(main);
  v8::TryCatch caught(isolate_);
  v8::Local<v8::Script> script;
  v8::Local<v8::Value> factory;
  v8::Local<v8::Value> scheduler;
  v8::Local<v8::Value> arguments[] = {preload->Global()};
  if (!v8::Script::Compile(
           main,
           gin::StringToV8(isolate_, kWorkerSchedulerSource).As<v8::String>())
           .ToLocal(&script) ||
      !script->Run(main).ToLocal(&factory) || !factory->IsFunction() ||
      !factory.As<v8::Function>()
           ->Call(main, v8::Undefined(isolate_), std::size(arguments),
                  arguments)
           .ToLocal(&scheduler) ||
      !scheduler->IsObject()) {
    LOG(ERROR) << "Service worker scheduler setup failed: "
               << (caught.HasCaught() ? ExceptionText(isolate_, caught) : "");
    return false;
  }
  for (const char* name : {"setTimeout", "clearTimeout", "setInterval",
                           "clearInterval", "queueMicrotask", "revoke"}) {
    auto key = gin::StringToV8(isolate_, name);
    v8::Local<v8::Value> function;
    if (!scheduler.As<v8::Object>()->Get(main, key).ToLocal(&function) ||
        !function->IsFunction()) {
      return false;
    }
    if (std::string_view(name) == "revoke") {
      revoke_scheduler_.Reset(isolate_, function.As<v8::Function>());
    } else if (!preload->Global()
                    ->CreateDataProperty(preload, key, function)
                    .FromMaybe(false)) {
      return false;
    }
  }
  return true;
}

bool XenonServiceWorkerRenderer::RunScript(v8::Local<v8::Context> context,
                                           const std::string& source,
                                           const std::string& path) {
  v8::TryCatch caught(isolate_);
  v8::ScriptOrigin origin(gin::StringToV8(isolate_, path));
  v8::Local<v8::Script> compiled;
  if (!v8::Script::Compile(
           context, gin::StringToV8(isolate_, source).As<v8::String>(), &origin)
           .ToLocal(&compiled) ||
      compiled->Run(context).IsEmpty()) {
    LOG(ERROR) << "Service worker bootstrap failed: " << path << " "
               << (caught.HasCaught() ? ExceptionText(isolate_, caught) : "");
    return false;
  }
  return true;
}

void XenonServiceWorkerRenderer::Shutdown() {
  if (closed_) {
    return;
  }
  closed_ = true;
  weak_factory_.InvalidateWeakPtrs();
  receiver_.reset();
  host_.reset();
  pending_invokes_.clear();
  if (!revoke_scheduler_.IsEmpty()) {
    v8::HandleScope handles(isolate_);
    auto main = main_context_.Get(isolate_);
    v8::Context::Scope scope(main);
    v8::MicrotasksScope microtasks(isolate_, main->GetMicrotaskQueue(),
                                   v8::MicrotasksScope::kDoNotRunMicrotasks);
    v8::TryCatch caught(isolate_);
    if (revoke_scheduler_.Get(isolate_)
            ->Call(main, v8::Undefined(isolate_), 0, nullptr)
            .IsEmpty()) {
      LOG(ERROR) << "Service worker scheduler cleanup failed: "
                 << ExceptionText(isolate_, caught);
    }
    revoke_scheduler_.Reset();
  }
  if (bridge_) {
    bridge_->Shutdown();
  }
  bridge_.reset();
  dispatch_handler_.Reset();
  preload_context_.Reset();
  main_context_.Reset();
}

void XenonServiceWorkerRenderer::OnDisconnected() {
  if (closed_ || preload_context_.IsEmpty()) {
    return;
  }
  v8::HandleScope handles(isolate_);
  auto context = preload_context_.Get(isolate_);
  v8::Context::Scope scope(context);
  v8::MicrotasksScope microtasks(isolate_, context->GetMicrotaskQueue(),
                                 v8::MicrotasksScope::kDoNotRunMicrotasks);
  host_.reset();
  for (auto& [id, resolver] : pending_invokes_) {
    resolver.Get(isolate_)
        ->Reject(
            context,
            v8::Exception::Error(
                gin::StringToV8(isolate_, "Service worker IPC was disconnected")
                    .As<v8::String>()))
        .Check();
  }
  pending_invokes_.clear();
}

void XenonServiceWorkerRenderer::ThrowError(const std::string& message) {
  isolate_->ThrowException(v8::Exception::Error(
      gin::StringToV8(isolate_, message).As<v8::String>()));
}

bool XenonServiceWorkerRenderer::ReadArguments(gin::Arguments* args,
                                               std::string* channel,
                                               base::Value* arguments) {
  auto values = args->GetAll();
  if (values.empty() || !gin::ConvertFromV8(isolate_, values[0], channel) ||
      channel->starts_with("__xenon:")) {
    args->ThrowTypeError("Invalid service worker IPC channel");
    return false;
  }
  if (closed_ || !host_.is_bound()) {
    ThrowError("Service worker IPC was disconnected");
    return false;
  }
  auto context = args->GetHolderCreationContext();
  if (values.size() - 1 >
      static_cast<size_t>(std::numeric_limits<int>::max())) {
    args->ThrowTypeError("Too many service worker IPC arguments");
    return false;
  }
  auto array = v8::Array::New(isolate_, static_cast<int>(values.size() - 1));
  for (size_t i = 1; i < values.size(); ++i) {
    array->Set(context, static_cast<uint32_t>(i - 1), values[i]).Check();
  }
  std::string error;
  return SerializeIpcValue(isolate_, context, array, true, arguments, &error);
}

void XenonServiceWorkerRenderer::Send(gin::Arguments* args) {
  std::string channel;
  base::Value arguments;
  if (ReadArguments(args, &channel, &arguments)) {
    host_->Send(channel, std::move(arguments));
  }
}

void XenonServiceWorkerRenderer::Invoke(gin::Arguments* args) {
  std::string channel;
  base::Value arguments;
  if (!ReadArguments(args, &channel, &arguments)) {
    return;
  }
  auto resolver = v8::Promise::Resolver::New(args->GetHolderCreationContext())
                      .ToLocalChecked();
  const uint64_t id = next_id_++;
  pending_invokes_.emplace(
      id, v8::Global<v8::Promise::Resolver>(isolate_, resolver));
  host_->Invoke(channel, std::move(arguments),
                mojo::WrapCallbackWithDefaultInvokeIfNotRun(
                    base::BindOnce(&XenonServiceWorkerRenderer::OnInvoke,
                                   weak_factory_.GetWeakPtr(), id),
                    DisconnectedResult()));
  args->Return(resolver->GetPromise());
}

void XenonServiceWorkerRenderer::OnInvoke(uint64_t id,
                                          mojom::IpcResultPtr result) {
  auto pending = pending_invokes_.find(id);
  if (closed_ || pending == pending_invokes_.end()) {
    return;
  }
  auto resolver_handle = std::move(pending->second);
  pending_invokes_.erase(pending);
  v8::HandleScope handles(isolate_);
  auto context = preload_context_.Get(isolate_);
  v8::Context::Scope scope(context);
  v8::MicrotasksScope microtasks(isolate_, context->GetMicrotaskQueue(),
                                 v8::MicrotasksScope::kDoNotRunMicrotasks);
  auto resolver = resolver_handle.Get(isolate_);
  v8::Local<v8::Value> value;
  std::string error;
  if (!result || !result->success ||
      !DeserializeIpcValue(isolate_, context, result->value, &error)
           .ToLocal(&value)) {
    if (result && !result->success) {
      error = result->error;
    }
    resolver
        ->Reject(
            context,
            v8::Exception::Error(
                gin::StringToV8(
                    isolate_,
                    error.empty() ? "Invalid service worker IPC reply" : error)
                    .As<v8::String>()))
        .Check();
    return;
  }
  resolver->Resolve(context, value).Check();
}

void XenonServiceWorkerRenderer::SetDispatchHandler(gin::Arguments* args) {
  v8::Local<v8::Function> callback;
  if (!args->GetNext(&callback)) {
    args->ThrowTypeError("Dispatch handler must be a function");
    return;
  }
  dispatch_handler_.Reset(isolate_, callback);
}

void XenonServiceWorkerRenderer::Dispatch(const std::string& channel,
                                          base::Value arguments) {
  if (closed_ || dispatch_handler_.IsEmpty()) {
    return;
  }
  v8::HandleScope handles(isolate_);
  auto context = preload_context_.Get(isolate_);
  v8::Context::Scope scope(context);
  v8::MicrotasksScope microtasks(isolate_, context->GetMicrotaskQueue(),
                                 v8::MicrotasksScope::kDoNotRunMicrotasks);
  v8::TryCatch caught(isolate_);
  v8::Local<v8::Value> decoded;
  std::string error;
  if (!DeserializeIpcValue(isolate_, context, arguments, &error)
           .ToLocal(&decoded)) {
    LOG(ERROR) << "Invalid service worker event: " << error;
    return;
  }
  v8::Local<v8::Value> values[] = {gin::StringToV8(isolate_, channel), decoded};
  if (dispatch_handler_.Get(isolate_)
          ->Call(context, v8::Undefined(isolate_), std::size(values), values)
          .IsEmpty()) {
    LOG(ERROR) << "Service worker IPC handler failed: "
               << ExceptionText(isolate_, caught);
  }
}

}  // namespace xenon::ipc
