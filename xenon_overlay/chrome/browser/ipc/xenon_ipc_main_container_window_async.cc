// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/functional/bind.h"
#include "base/task/bind_post_task.h"
#include "gin/arguments.h"
#include "gin/converter.h"
#include "v8/include/v8.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_ipc_main_container.h"

namespace xenon::ipc {

void XenonIpcMainContainer::NativeBrowserWindowCallAsync(gin::Arguments* args) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  int32_t window_id = 0;
  std::string command;
  v8::Local<v8::Value> input;
  base::Value arguments;
  std::string error;
  if (!args->GetNext(&window_id) || !args->GetNext(&command) ||
      !args->GetNext(&input) || !V8ToValue(input, &arguments, &error)) {
    args->ThrowTypeError(error.empty() ? "Invalid asynchronous host call"
                                       : error);
    return;
  }
  if (shutting_down_ || !window_hooks_.call_async) {
    args->ThrowTypeError("ERR_NOT_SUPPORTED: asynchronous host is unavailable");
    return;
  }
  if (!AddNativeRuntimeArguments(window_id, command, &arguments, &error)) {
    args->ThrowTypeError(error);
    return;
  }
  if (pending_window_calls_.size() >= 1024) {
    args->ThrowTypeError("ERR_RESOURCE_BUSY: asynchronous host queue is full");
    return;
  }
  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(context_.Get(isolate_)).ToLocal(&resolver)) {
    return;
  }
  const uint64_t id = next_window_call_id_++;
  pending_window_calls_.emplace(
      id, v8::Global<v8::Promise::Resolver>(isolate_, resolver));
  args->Return(resolver->GetPromise());
  // Even a host that completes inline must deliver after this JS stack has
  // unwound. No nested message loop or synchronous UI/Utility wait is needed.
  window_hooks_.call_async.Run(
      window_id, command, std::move(arguments),
      base::BindPostTaskToCurrentDefault(
          base::BindOnce(&XenonIpcMainContainer::OnBrowserWindowCallComplete,
                         weak_factory_.GetWeakPtr(), id)));
}

void XenonIpcMainContainer::OnBrowserWindowCallComplete(uint64_t id,
                                                        base::Value result,
                                                        std::string error) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto found = pending_window_calls_.find(id);
  if (shutting_down_ || found == pending_window_calls_.end()) {
    return;
  }
  if (isolate_->IsExecutionTerminating()) {
    pending_window_calls_.erase(found);
    return;
  }
  v8::Locker locker(isolate_);
  v8::Isolate::Scope isolate_scope(isolate_);
  v8::HandleScope handles(isolate_);
  auto context = context_.Get(isolate_);
  v8::Context::Scope context_scope(context);
  auto resolver = found->second.Get(isolate_);
  pending_window_calls_.erase(found);
  v8::Local<v8::Value> value;
  if (error.empty() && ValueToV8(result).ToLocal(&value)) {
    resolver->Resolve(context, value).Check();
  } else {
    if (error.empty()) {
      error = "ERR_OPERATION_FAILED: invalid asynchronous host result";
    }
    auto exception =
        v8::Exception::Error(gin::StringToV8(isolate_, error)).As<v8::Object>();
    const auto separator = error.find(':');
    if (separator != std::string::npos && error.starts_with("ERR_")) {
      exception
          ->CreateDataProperty(
              context, gin::StringToV8(isolate_, "code"),
              gin::StringToV8(isolate_, error.substr(0, separator)))
          .Check();
    }
    resolver->Reject(context, exception).Check();
  }
  isolate_->PerformMicrotaskCheckpoint();
}

}  // namespace xenon::ipc
