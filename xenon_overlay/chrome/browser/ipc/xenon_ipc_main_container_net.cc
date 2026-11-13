// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
#include <cmath>

#include "base/logging.h"
#include "gin/arguments.h"
#include "gin/try_catch.h"
#include "v8/include/v8.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_ipc_main_container.h"

namespace xenon::ipc {
void XenonIpcMainContainer::NativeElectronNetStart(gin::Arguments* args) {
  v8::Local<v8::Value> value;
  base::Value options;
  std::string error;
  if (!args->GetNext(&value) || !V8ToValue(value, &options, &error) ||
      !options.is_dict()) {
    args->ThrowTypeError(
        "ERR_INVALID_ARG_TYPE: network options must be an object");
    return;
  }
  if (!electron_net_ || shutting_down_) {
    args->ThrowTypeError(
        "ERR_NOT_SUPPORTED: Electron network host is unavailable");
    return;
  }
  const uint64_t id = electron_net_->Start(options.GetDict(), &error);
  if (!id) {
    args->ThrowTypeError(error);
    return;
  }
  args->Return(static_cast<double>(id));
}
void XenonIpcMainContainer::NativeElectronNetControl(gin::Arguments* args) {
  double id;
  std::string command;
  if (!args->GetNext(&id) || !std::isfinite(id) || id < 1 ||
      id > 9007199254740991.0 || std::floor(id) != id ||
      !args->GetNext(&command)) {
    args->ThrowTypeError(
        "ERR_INVALID_ARG_TYPE: invalid network control operation");
    return;
  }
  std::string error;
  const bool result =
      electron_net_ &&
      electron_net_->Control(static_cast<uint64_t>(id), command, &error);
  if (!error.empty()) {
    args->ThrowTypeError(error);
    return;
  }
  args->Return(result);
}
void XenonIpcMainContainer::NativeElectronNetOnline(gin::Arguments* args) {
  bool online = false;
  std::string error;
  if (!electron_net_online_ || shutting_down_ ||
      !electron_net_online_.Run(&online, &error)) {
    args->ThrowTypeError(
        error.empty()
            ? "ERR_NOT_SUPPORTED: Electron network status is unavailable"
            : error);
    return;
  }
  if (!error.empty()) {
    args->ThrowTypeError(error);
    return;
  }
  args->Return(online);
}
void XenonIpcMainContainer::NativeElectronNetAuth(gin::Arguments* args) {
  double id, challenge;
  auto valid_id = [](double value) {
    return std::isfinite(value) && value >= 1 && value <= 9007199254740991.0 &&
           std::floor(value) == value;
  };
  if (!args->GetNext(&id) || !valid_id(id) || !args->GetNext(&challenge) ||
      !valid_id(challenge)) {
    args->ThrowTypeError(
        "ERR_INVALID_ARG_TYPE: invalid authentication challenge");
    return;
  }
  std::optional<std::string> username, password;
  if (args->Length() > 2) {
    v8::Local<v8::Value> user, pass;
    if (!args->GetNext(&user) || !args->GetNext(&pass) ||
        !((user->IsUndefined() && pass->IsUndefined()) ||
          (user->IsString() && pass->IsString()))) {
      args->ThrowTypeError(
          "ERR_INVALID_ARG_TYPE: authentication requires username and password "
          "strings");
      return;
    }
    if (user->IsString()) {
      v8::String::Utf8Value user_text(isolate_, user);
      v8::String::Utf8Value pass_text(isolate_, pass);
      if (user_text.length() > 65536 || pass_text.length() > 65536) {
        args->ThrowTypeError(
            "ERR_OUT_OF_RANGE: authentication credentials are too long");
        return;
      }
      username = std::string(*user_text, user_text.length());
      password = std::string(*pass_text, pass_text.length());
    }
  }
  args->Return(electron_net_ &&
               electron_net_->Auth(static_cast<uint64_t>(id),
                                   static_cast<uint64_t>(challenge), username,
                                   password));
}
void XenonIpcMainContainer::DispatchElectronNetEvent(base::Value event) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!initialized_ || shutting_down_ ||
      dispatch_electron_net_event_.IsEmpty()) {
    return;
  }
  v8::Locker locker(isolate_);
  v8::Isolate::Scope isolate_scope(isolate_);
  v8::HandleScope handles(isolate_);
  auto context = context_.Get(isolate_);
  v8::Context::Scope context_scope(context);
  gin::TryCatch try_catch(isolate_);
  v8::Local<v8::Value> value;
  if (!ValueToV8(event).ToLocal(&value) ||
      dispatch_electron_net_event_.Get(isolate_)
          ->Call(context, context->Global(), 1, &value)
          .IsEmpty()) {
    LOG(ERROR) << "Electron network event failed: "
               << try_catch.GetStackTrace();
  }
  isolate_->PerformMicrotaskCheckpoint();
}
}  // namespace xenon::ipc
