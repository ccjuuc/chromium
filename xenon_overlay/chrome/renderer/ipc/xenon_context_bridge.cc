// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/renderer/ipc/xenon_context_bridge.h"

#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "gin/converter.h"
#include "gin/function_template.h"
#include "v8/include/v8.h"
#include "xenon_overlay/common/ipc/xenon_ipc_value_codec.h"

namespace xenon::ipc {
namespace {
std::string ExceptionText(v8::Isolate* isolate, v8::TryCatch& caught) {
  v8::String::Utf8Value message(isolate, caught.Exception());
  return *message ? std::string(*message, message.length())
                  : "Context bridge call failed";
}
}  // namespace

scoped_refptr<XenonContextBridge> XenonContextBridge::Create(
    v8::Local<v8::Context> preload_context,
    v8::Local<v8::Context> main_context) {
  return base::MakeRefCounted<XenonContextBridge>(preload_context,
                                                  main_context);
}

XenonContextBridge::XenonContextBridge(v8::Local<v8::Context> preload_context,
                                       v8::Local<v8::Context> main_context)
    : isolate_(v8::Isolate::GetCurrent()),
      main_context_(isolate_, main_context),
      preload_context_(isolate_, preload_context),
      original_function_key_(isolate_, v8::Private::New(isolate_)) {}

XenonContextBridge::~XenonContextBridge() = default;

v8::Local<v8::Function> XenonContextBridge::BindFunction(auto method,
                                                         auto... bound_args) {
  // Gin's FunctionTemplate factory uses V8's per-context template cache, which
  // would retain every transient proxy despite its weak handle. Function::New
  // disables that cache. Reuse Gin's holder so callbacks still have its normal
  // weak cleanup and isolate-disposal handling.
  using Signature = void(gin::Arguments*);
  auto* holder = new gin::internal::CallbackHolder<Signature>(
      isolate_,
      base::BindRepeating(method, scoped_refptr<XenonContextBridge>(this),
                          bound_args...),
      gin::InvokerOptions{});
  return v8::Function::New(
             isolate_->GetCurrentContext(),
             &gin::internal::Dispatcher<Signature>::DispatchToCallback,
             holder->GetHandle(isolate_), 0, v8::ConstructorBehavior::kThrow)
      .ToLocalChecked();
}

v8::Local<v8::Object> XenonContextBridge::GetApi() {
  auto context = preload_context_.Get(isolate_);
  v8::Context::Scope scope(context);
  auto api = v8::Object::New(isolate_);
  api->CreateDataProperty(context,
                          gin::StringToV8(isolate_, "exposeInMainWorld"),
                          BindFunction(&XenonContextBridge::ExposeInMainWorld))
      .Check();
  api->CreateDataProperty(context,
                          gin::StringToV8(isolate_, "executeInMainWorld"),
                          BindFunction(&XenonContextBridge::ExecuteInMainWorld))
      .Check();
  api->SetIntegrityLevel(context, v8::IntegrityLevel::kFrozen).Check();
  return api;
}

void XenonContextBridge::Shutdown() {
  if (closed_) {
    return;
  }
  closed_ = true;
  v8::HandleScope handles(isolate_);
  // Pin the remaining proxies while deleting their private edges. A GC during
  // cleanup must not run a weak callback that mutates the records below.
  for (const auto& [id, function] : functions_) {
    function->proxy.ClearWeak();
  }
  for (const auto& [id, function] : functions_) {
    auto target = (function->source_is_main ? preload_context_ : main_context_)
                      .Get(isolate_);
    v8::Context::Scope scope(target);
    // A retained but revoked proxy must not keep its preload closure alive.
    function->proxy.Get(isolate_)
        ->DeletePrivate(target, original_function_key_.Get(isolate_))
        .Check();
  }
  promises_.clear();
  functions_.clear();
  original_function_key_.Reset();
  preload_context_.Reset();
  main_context_.Reset();
}

// static
void XenonContextBridge::FirstFunctionWeakCallback(
    const v8::WeakCallbackInfo<BridgedFunction>& data) {
  auto* function = data.GetParameter();
  function->proxy.Reset();
  // First-pass callbacks cannot make other V8 API calls. Transfer ownership
  // without destroying the original handle; the second pass releases it. The
  // record's owner reference also survives Gin's independently ordered cleanup.
  auto entry = function->owner->functions_.extract(function->id);
  CHECK(!entry.empty() && entry.mapped().get() == function);
  entry.mapped().release();
  data.SetSecondPassCallback(SecondFunctionWeakCallback);
}

// static
void XenonContextBridge::SecondFunctionWeakCallback(
    const v8::WeakCallbackInfo<BridgedFunction>& data) {
  delete data.GetParameter();
}

void XenonContextBridge::ThrowError(const std::string& message) {
  isolate_->ThrowException(v8::Exception::Error(
      gin::StringToV8(isolate_, message).As<v8::String>()));
}

v8::MaybeLocal<v8::Value> XenonContextBridge::CopyValue(
    v8::Local<v8::Context> source,
    v8::Local<v8::Context> target,
    v8::Local<v8::Value> value,
    int depth,
    bool freeze,
    std::vector<std::pair<v8::Local<v8::Object>, v8::Local<v8::Object>>>*
        copies) {
  if (closed_ || depth > 64) {
    ThrowError("Context bridge value is too deeply nested or unavailable");
    return {};
  }
  if (value->IsSymbol()) {
    ThrowError("Symbols cannot cross contextBridge");
    return {};
  }
  if (!value->IsObject()) {
    return value;
  }
  std::vector<std::pair<v8::Local<v8::Object>, v8::Local<v8::Object>>>
      local_copies;
  if (!copies) {
    copies = &local_copies;
  }
  for (const auto& [original, copy] : *copies) {
    if (original->StrictEquals(value)) {
      return copy;
    }
  }
  const bool source_is_main = source == main_context_.Get(isolate_);
  if (value->IsFunction()) {
    for (const auto& [id, function] : functions_) {
      if (function->source_is_main == source_is_main &&
          function->original.Get(isolate_)->StrictEquals(value)) {
        return function->proxy.Get(isolate_);
      }
      // Returning a proxy to its original realm restores function identity.
      if (function->source_is_main != source_is_main &&
          function->proxy.Get(isolate_)->StrictEquals(value)) {
        return function->original.Get(isolate_);
      }
    }
    const size_t id = next_function_id_++;
    v8::Context::Scope target_scope(target);
    auto proxy = BindFunction(&XenonContextBridge::CallBridgedFunction, id);
    // Keep the source alive through a V8-visible edge, rather than a strong C++
    // root. This also lets V8 collect cycles involving callback closures. The
    // private property is inaccessible to JavaScript in either realm.
    if (!proxy->SetPrivate(target, original_function_key_.Get(isolate_), value)
             .FromMaybe(false)) {
      return {};
    }
    auto function = std::make_unique<BridgedFunction>();
    function->owner = scoped_refptr<XenonContextBridge>(this);
    function->id = id;
    function->source_is_main = source_is_main;
    function->original.Reset(isolate_, value.As<v8::Function>());
    function->original.SetWeak();
    function->proxy.Reset(isolate_, proxy);
    function->proxy.SetWeak(function.get(), FirstFunctionWeakCallback,
                            v8::WeakCallbackType::kParameter);
    functions_.emplace(id, std::move(function));
    return proxy;
  }
  if (value->IsPromise()) {
    v8::Context::Scope target_scope(target);
    auto resolver = v8::Promise::Resolver::New(target).ToLocalChecked();
    const uint64_t id = next_id_++;
    promises_.emplace(
        id, BridgedPromise{!source_is_main, v8::Global<v8::Promise::Resolver>(
                                                isolate_, resolver)});
    v8::Context::Scope source_scope(source);
    v8::Local<v8::Promise> chained;
    if (!value.As<v8::Promise>()
             ->Then(source,
                    BindFunction(&XenonContextBridge::SettleBridgedPromise, id,
                                 false),
                    BindFunction(&XenonContextBridge::SettleBridgedPromise, id,
                                 true))
             .ToLocal(&chained)) {
      promises_.erase(id);
      return {};
    }
    return resolver->GetPromise();
  }
  if (value->IsArrayBuffer() || value->IsArrayBufferView() || value->IsDate() ||
      value->IsRegExp() || value->IsMap() || value->IsSet() ||
      value->IsNativeError()) {
    base::Value serialized;
    std::string error;
    if (!SerializeIpcValue(isolate_, source, value, true, &serialized,
                           &error)) {
      return {};
    }
    return DeserializeIpcValue(isolate_, target, serialized, &error);
  }
  v8::Context::Scope target_scope(target);
  auto copy = value->IsArray()
                  ? v8::Array::New(isolate_, value.As<v8::Array>()->Length())
                        .As<v8::Object>()
                  : v8::Object::New(isolate_);
  v8::Local<v8::Array> keys;
  auto object = value.As<v8::Object>();
  copies->emplace_back(object, copy);
  if (!object
           ->GetOwnPropertyNames(source,
                                 static_cast<v8::PropertyFilter>(
                                     v8::ONLY_ENUMERABLE | v8::SKIP_SYMBOLS),
                                 v8::KeyConversionMode::kConvertToString)
           .ToLocal(&keys)) {
    return {};
  }
  for (uint32_t index = 0; index < keys->Length(); ++index) {
    v8::Local<v8::Value> key, member, copied;
    if (!keys->Get(source, index).ToLocal(&key) ||
        !object->Get(source, key).ToLocal(&member) ||
        !CopyValue(source, target, member, depth + 1, freeze, copies)
             .ToLocal(&copied) ||
        !copy->CreateDataProperty(target, key.As<v8::Name>(), copied)
             .FromMaybe(false)) {
      return {};
    }
  }
  if (freeze && !copy->SetIntegrityLevel(target, v8::IntegrityLevel::kFrozen)
                     .FromMaybe(false)) {
    return {};
  }
  return copy;
}

void XenonContextBridge::CallBridgedFunction(size_t id, gin::Arguments* args) {
  auto found = functions_.find(id);
  if (closed_ || found == functions_.end()) {
    ThrowError("Preload context has been destroyed");
    return;
  }
  const bool source_is_main = found->second->source_is_main;
  auto source =
      (source_is_main ? main_context_ : preload_context_).Get(isolate_);
  auto target =
      (source_is_main ? preload_context_ : main_context_).Get(isolate_);
  auto function = found->second->original.Get(isolate_);
  auto incoming = args->GetAll();
  v8::LocalVector<v8::Value> arguments(isolate_);
  v8::TryCatch caught(isolate_);
  for (const auto& value : incoming) {
    v8::Local<v8::Value> copied;
    if (!CopyValue(target, source, value).ToLocal(&copied)) {
      caught.Reset();
      ThrowError("Argument cannot cross contextBridge");
      caught.ReThrow();
      return;
    }
    arguments.push_back(copied);
  }
  v8::Local<v8::Value> result, copied;
  bool success;
  {
    v8::Context::Scope source_scope(source);
    success = function
                  ->Call(source, v8::Undefined(isolate_), arguments.size(),
                         arguments.data())
                  .ToLocal(&result) &&
              CopyValue(source, target, result).ToLocal(&copied);
  }
  if (!success) {
    const std::string error = caught.HasCaught()
                                  ? ExceptionText(isolate_, caught)
                                  : "Return value cannot cross contextBridge";
    caught.Reset();
    ThrowError(error);
    caught.ReThrow();
    return;
  }
  args->Return(copied);
}

void XenonContextBridge::SettleBridgedPromise(uint64_t id,
                                              bool reject,
                                              gin::Arguments* args) {
  auto found = promises_.find(id);
  if (closed_ || found == promises_.end()) {
    return;
  }
  auto pending = std::move(found->second);
  promises_.erase(found);
  auto target =
      (pending.target_is_main ? main_context_ : preload_context_).Get(isolate_);
  auto source =
      (pending.target_is_main ? preload_context_ : main_context_).Get(isolate_);
  v8::Local<v8::Value> value, copy;
  if (!args->GetNext(&value)) {
    value = v8::Undefined(isolate_);
  }
  v8::TryCatch caught(isolate_);
  v8::Context::Scope scope(target);
  if (!CopyValue(source, target, value).ToLocal(&copy)) {
    caught.Reset();
    copy = v8::Exception::Error(
        gin::StringToV8(isolate_, "Promise value cannot cross contextBridge")
            .As<v8::String>());
    reject = true;
  }
  auto resolver = pending.resolver.Get(isolate_);
  if (reject) {
    resolver->Reject(target, copy).Check();
  } else {
    resolver->Resolve(target, copy).Check();
  }
}

void XenonContextBridge::ExposeInMainWorld(gin::Arguments* args) {
  std::string key;
  v8::Local<v8::Value> value, copied;
  if (closed_ || !args->GetNext(&key) || !args->GetNext(&value)) {
    args->ThrowTypeError("Expected an API name and value");
    return;
  }
  auto main = main_context_.Get(isolate_);
  if (main->Global()
          ->HasOwnProperty(main, gin::StringToV8(isolate_, key).As<v8::Name>())
          .FromMaybe(true)) {
    ThrowError("Cannot replace an existing global through contextBridge");
    return;
  }
  if (!CopyValue(preload_context_.Get(isolate_), main, value, 0, true)
           .ToLocal(&copied)) {
    return;
  }
  main->Global()
      ->DefineOwnProperty(
          main, gin::StringToV8(isolate_, key).As<v8::Name>(), copied,
          static_cast<v8::PropertyAttribute>(v8::ReadOnly | v8::DontDelete))
      .Check();
}

void XenonContextBridge::ExecuteInMainWorld(gin::Arguments* args) {
  v8::Local<v8::Object> script;
  if (closed_ || !args->GetNext(&script)) {
    args->ThrowTypeError("Expected a script with a func property");
    return;
  }
  auto preload = preload_context_.Get(isolate_);
  auto main = main_context_.Get(isolate_);
  v8::Local<v8::Value> function, arguments;
  if (!script->Get(preload, gin::StringToV8(isolate_, "func"))
           .ToLocal(&function) ||
      !function->IsFunction() ||
      !script->Get(preload, gin::StringToV8(isolate_, "args"))
           .ToLocal(&arguments)) {
    args->ThrowTypeError("script.func must be a function");
    return;
  }
  if (arguments->IsUndefined()) {
    arguments = v8::Array::New(isolate_);
  }
  if (!arguments->IsArray()) {
    args->ThrowTypeError("script.args must be an array");
    return;
  }
  v8::Local<v8::String> source;
  if (!function.As<v8::Function>()->ToString(preload).ToLocal(&source)) {
    return;
  }
  v8::String::Utf8Value function_source(isolate_, source);
  v8::TryCatch caught(isolate_);
  v8::Local<v8::Value> result, copied;
  bool success = false;
  {
    v8::Context::Scope scope(main);
    v8::Local<v8::Script> compiled;
    v8::Local<v8::Value> executable, cloned_args;
    if (v8::Script::Compile(
            main,
            gin::StringToV8(
                isolate_,
                "(" + std::string(*function_source, function_source.length()) +
                    ")")
                .As<v8::String>())
            .ToLocal(&compiled) &&
        compiled->Run(main).ToLocal(&executable) && executable->IsFunction() &&
        CopyValue(preload, main, arguments).ToLocal(&cloned_args)) {
      auto array = cloned_args.As<v8::Array>();
      v8::LocalVector<v8::Value> values(isolate_);
      for (uint32_t i = 0; i < array->Length(); ++i) {
        v8::Local<v8::Value> value;
        if (!array->Get(main, i).ToLocal(&value)) {
          return;
        }
        values.push_back(value);
      }
      success = executable.As<v8::Function>()
                    ->Call(main, v8::Undefined(isolate_), values.size(),
                           values.data())
                    .ToLocal(&result) &&
                CopyValue(main, preload, result).ToLocal(&copied);
    }
  }
  if (!success) {
    const auto error = caught.HasCaught() ? ExceptionText(isolate_, caught)
                                          : "Unable to execute in main world";
    caught.Reset();
    ThrowError(error);
    caught.ReThrow();
    return;
  }
  args->Return(copied);
}

}  // namespace xenon::ipc
