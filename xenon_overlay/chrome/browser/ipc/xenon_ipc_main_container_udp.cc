// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license.
#include "base/functional/bind.h"
#include "base/logging.h"
#include "gin/arguments.h"
#include "gin/converter.h"
#include "gin/try_catch.h"
#include "v8/include/v8.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_ipc_main_container.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_udp_socket_bridge.h"
#include "xenon_overlay/chrome/browser/napi/native_addon_event_loop.h"

namespace xenon::ipc {
uv_loop_s* XenonIpcMainContainer::EnsureNativeEventLoop(std::string* error) {
#if BUILDFLAG(ENABLE_XENON_NODE_UV_COMPAT)
  if (shutting_down_) {
    *error = "ERR_NOT_SUPPORTED: native runtime is shutting down";
    return nullptr;
  }
  if (!native_event_loop_) {
    auto loop = xenon::NativeAddonEventLoop::Create();
    if (!loop.has_value()) {
      *error = loop.error();
      return nullptr;
    }
    native_event_loop_ = std::move(loop.value());
  }
  ScheduleNativeEventLoopPump();
  return native_event_loop_->loop();
#else
  *error = "ERR_NOT_SUPPORTED: native libuv support is disabled";
  return nullptr;
#endif
}
void XenonIpcMainContainer::NativeDgramCall(gin::Arguments* args) {
  base::Value request;
  std::string error;
  v8::Local<v8::Value> argument;
  if (!args->GetNext(&argument) || !V8ToValue(argument, &request, &error) ||
      !request.is_dict()) {
    args->ThrowTypeError("UDP call expects a request object");
    return;
  }
  v8::Local<v8::Value> data;
  if (args->GetNext(&data) && !data->IsUndefined()) {
    base::Value bytes;
    if ((!data->IsArrayBuffer() && !data->IsArrayBufferView()) ||
        !V8ToValue(data, &bytes, &error) || !bytes.is_blob()) {
      args->ThrowTypeError("UDP payload must be an ArrayBuffer or a view");
      return;
    }
    request.GetDict().Set("bytes", std::move(bytes));
  }
  auto* loop = EnsureNativeEventLoop(&error);
  if (loop && !udp_sockets_) {
    udp_sockets_ = std::make_unique<XenonUdpSocketBridge>(
        loop, base::BindRepeating(&XenonIpcMainContainer::DispatchDgramEvent,
                                  base::Unretained(this)));
  }
  auto response = base::DictValue();
  if (!error.empty()) {
    response.Set("error", error);
  } else {
    auto result = udp_sockets_->Call(request.GetDict());
    if (result.has_value()) {
      response.Set("value", std::move(result.value()));
    } else {
      response.Set("error", result.error());
    }
  }
  v8::Local<v8::Value> value;
  if (ValueToV8(base::Value(std::move(response))).ToLocal(&value)) {
    args->Return(value);
  }
}
void XenonIpcMainContainer::DispatchDgramEvent(base::Value event) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!initialized_ || shutting_down_ || context_.IsEmpty()) {
    return;
  }
  v8::Locker locker(isolate_);
  v8::Isolate::Scope isolate_scope(isolate_);
  v8::HandleScope handles(isolate_);
  auto context = context_.Get(isolate_);
  v8::Context::Scope context_scope(context);
  gin::TryCatch try_catch(isolate_);
  v8::Local<v8::Value> dispatch, value;
  if (!context->Global()
           ->Get(context, gin::StringToV8(isolate_, "__xenonDgramEvent"))
           .ToLocal(&dispatch) ||
      !dispatch->IsFunction() || !ValueToV8(event).ToLocal(&value) ||
      dispatch.As<v8::Function>()
          ->Call(context, context->Global(), 1, &value)
          .IsEmpty()) {
    LOG(ERROR) << "UDP event failed: " << try_catch.GetStackTrace();
  }
  isolate_->PerformMicrotaskCheckpoint();
}
}  // namespace xenon::ipc
