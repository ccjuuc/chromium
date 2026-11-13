// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/renderer/ipc/xenon_script_executor.h"

#include <utility>

#include "base/functional/bind.h"
#include "gin/converter.h"
#include "gin/function_template.h"
#include "v8/include/v8.h"
#include "xenon_overlay/common/ipc/xenon_ipc_value_codec.h"

namespace xenon::ipc {

XenonScriptExecutor::XenonScriptExecutor(v8::Isolate* isolate)
    : isolate_(isolate) {}

XenonScriptExecutor::~XenonScriptExecutor() = default;

v8::MaybeLocal<v8::Function> XenonScriptExecutor::CreateSettlementCallback(
    v8::Local<v8::Context> context,
    uint64_t id,
    uint64_t generation,
    bool rejected) {
  // Avoid Gin's per-context FunctionTemplate cache: each pending request must
  // release its callback and executor reference when the JS promise is GC'd.
  using Signature = void(gin::Arguments*);
  auto* holder = new gin::internal::CallbackHolder<Signature>(
      isolate_,
      base::BindRepeating(&XenonScriptExecutor::Settle,
                          scoped_refptr<XenonScriptExecutor>(this), id,
                          generation, rejected),
      gin::InvokerOptions{});
  return v8::Function::New(
      context, &gin::internal::Dispatcher<Signature>::DispatchToCallback,
      holder->GetHandle(isolate_), 0, v8::ConstructorBehavior::kThrow);
}

v8::MaybeLocal<v8::Promise> XenonScriptExecutor::Execute(
    uint64_t request_id,
    v8::Local<v8::Context> main_context,
    v8::Local<v8::Context> caller_context,
    const std::string& source) {
  if (closed_ || pending_.contains(request_id) || main_context.IsEmpty() ||
      caller_context.IsEmpty()) {
    return {};
  }
  v8::EscapableHandleScope handles(isolate_);
  v8::Context::Scope caller_scope(caller_context);
  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(caller_context).ToLocal(&resolver)) {
    return {};
  }
  const uint64_t id = request_id;
  const uint64_t generation = next_generation_++;
  pending_.emplace(
      id, Pending{generation, v8::Global<v8::Context>(isolate_, caller_context),
                  v8::Global<v8::Promise::Resolver>(isolate_, resolver)});
  v8::Local<v8::Promise> result = resolver->GetPromise();
  {
    v8::Context::Scope main_scope(main_context);
    v8::TryCatch caught(isolate_);
    v8::Local<v8::Script> script;
    v8::Local<v8::Value> value;
    // This is an embedder-authorized script, not page eval(). Compiling it in
    // the selected main context preserves global bindings and works under a
    // page CSP that disables eval, without changing the page's CSP policy.
    if (!v8::Script::Compile(main_context,
                             gin::StringToV8(isolate_, source).As<v8::String>())
             .ToLocal(&script) ||
        !script->Run(main_context).ToLocal(&value)) {
      value = caught.HasCaught()
                  ? caught.Exception()
                  : v8::Exception::Error(
                        gin::StringToV8(isolate_, "Script execution failed")
                            .As<v8::String>());
      caught.Reset();
      Complete(id, generation, true, main_context, value);
    } else if (!closed_ && pending_.contains(id)) {
      // Native Promises attach directly through V8, without consulting a
      // replaced Promise.prototype.then. Other values use normal resolution
      // so user-provided thenables are awaited as well.
      v8::Local<v8::Promise::Resolver> assimilator;
      v8::Local<v8::Promise> source_promise;
      v8::Local<v8::Function> fulfilled, rejected;
      v8::Local<v8::Promise> chained;
      if (value->IsPromise()) {
        source_promise = value.As<v8::Promise>();
      } else if (v8::Promise::Resolver::New(main_context)
                     .ToLocal(&assimilator)) {
        source_promise = assimilator->GetPromise();
      }
      if (source_promise.IsEmpty() ||
          !CreateSettlementCallback(main_context, id, generation, false)
               .ToLocal(&fulfilled) ||
          !CreateSettlementCallback(main_context, id, generation, true)
               .ToLocal(&rejected) ||
          !source_promise->Then(main_context, fulfilled, rejected)
               .ToLocal(&chained) ||
          (!assimilator.IsEmpty() &&
           !assimilator->Resolve(main_context, value).FromMaybe(false))) {
        value =
            caught.HasCaught()
                ? caught.Exception()
                : v8::Exception::Error(
                      gin::StringToV8(isolate_, "Unable to await script result")
                          .As<v8::String>());
        caught.Reset();
        Complete(id, generation, true, main_context, value);
      }
    }
  }
  return handles.Escape(result);
}

void XenonScriptExecutor::Settle(uint64_t id,
                                 uint64_t generation,
                                 bool rejected,
                                 gin::Arguments* args) {
  if (closed_ || !pending_.contains(id)) {
    return;
  }
  v8::Local<v8::Value> value = args->PeekNext();
  if (value.IsEmpty()) {
    value = v8::Undefined(isolate_);
  }
  Complete(id, generation, rejected, args->GetHolderCreationContext(), value);
}

void XenonScriptExecutor::Complete(uint64_t id,
                                   uint64_t generation,
                                   bool rejected,
                                   v8::Local<v8::Context> source_context,
                                   v8::Local<v8::Value> value) {
  auto found = pending_.find(id);
  if (closed_ || found == pending_.end() ||
      found->second.generation != generation) {
    return;
  }
  auto target = found->second.context.Get(isolate_);
  auto resolver = found->second.resolver.Get(isolate_);
  // Serialization can invoke application getters, including navigation. Keep
  // the pending entry until after serialization so Shutdown can reject it.
  base::Value serialized;
  std::string error;
  const bool cloned = SerializeIpcValue(isolate_, source_context, value, false,
                                        &serialized, &error);
  if (closed_ || !pending_.contains(id) ||
      pending_.at(id).generation != generation) {
    return;
  }
  pending_.erase(id);
  v8::Context::Scope target_scope(target);
  v8::Local<v8::Value> copied;
  if (!cloned || !DeserializeIpcValue(isolate_, target, serialized, &error)
                      .ToLocal(&copied)) {
    rejected = true;
    copied = v8::Exception::Error(
        gin::StringToV8(isolate_, error.empty()
                                      ? "Script result could not be cloned"
                                      : error)
            .As<v8::String>());
  }
  if (rejected) {
    resolver->Reject(target, copied).FromMaybe(false);
  } else {
    resolver->Resolve(target, copied).FromMaybe(false);
  }
}

void XenonScriptExecutor::Shutdown() {
  if (closed_) {
    return;
  }
  closed_ = true;
  auto pending = std::move(pending_);
  pending_.clear();
  v8::HandleScope handles(isolate_);
  for (auto& [id, item] : pending) {
    auto context = item.context.Get(isolate_);
    v8::Context::Scope scope(context);
    item.resolver.Get(isolate_)
        ->Reject(context,
                 v8::Exception::Error(
                     gin::StringToV8(isolate_,
                                     "Script execution context was destroyed")
                         .As<v8::String>()))
        .FromMaybe(false);
  }
}

void XenonScriptExecutor::Cancel(uint64_t request_id) {
  auto node = pending_.extract(request_id);
  if (node.empty()) {
    return;
  }
  v8::HandleScope handles(isolate_);
  auto context = node.mapped().context.Get(isolate_);
  v8::Context::Scope scope(context);
  node.mapped()
      .resolver.Get(isolate_)
      ->Reject(context,
               v8::Exception::Error(
                   gin::StringToV8(isolate_, "Script execution was cancelled")
                       .As<v8::String>()))
      .FromMaybe(false);
}

}  // namespace xenon::ipc
