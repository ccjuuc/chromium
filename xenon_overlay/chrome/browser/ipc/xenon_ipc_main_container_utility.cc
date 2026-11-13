// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_ipc_main_container.h"

#include <cerrno>
#include <cstring>
#include <utility>

#include "base/functional/bind.h"
#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/logging.h"
#include "base/task/single_thread_task_runner.h"
#include "build/build_config.h"
#include "gin/arguments.h"
#include "gin/converter.h"
#include "gin/function_template.h"
#include "gin/try_catch.h"
#include "v8/include/v8.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_message_port.h"

#if BUILDFLAG(IS_WIN)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace xenon::ipc {
namespace {
v8::Local<v8::Uint8Array> UtilityBytes(v8::Isolate* isolate,
                                     const std::vector<uint8_t>& bytes) {
  auto buffer = v8::ArrayBuffer::New(isolate, bytes.size());
  if (!bytes.empty()) {
    auto store = buffer->GetBackingStore();
    auto destination = UNSAFE_BUFFERS(base::span(
        static_cast<uint8_t*>(store->Data()), store->ByteLength()));
    destination.copy_from(base::span(bytes));
  }
  return v8::Uint8Array::New(buffer, 0, bytes.size());
}

void ThrowUtilityError(v8::Isolate* isolate, const std::string& code,
                       const std::string& message) {
  auto error = v8::Exception::Error(gin::StringToV8(isolate, message))
                   .As<v8::Object>();
  error->CreateDataProperty(isolate->GetCurrentContext(),
                           gin::StringToV8(isolate, "code"),
                           gin::StringToV8(isolate, code)).Check();
  isolate->ThrowException(error);
}
}  // namespace

void XenonIpcMainContainer::SetUtilityRuntimeOptions(
    UtilityRuntimeOptions options) {
  CHECK(!initialized_ && !worker_options_);
  utility_options_ = std::move(options);
}

void XenonIpcMainContainer::SetUtilityProcessHooks(UtilityProcessHooks hooks) {
  CHECK(!initialized_);
  utility_process_hooks_ = std::move(hooks);
}

void XenonIpcMainContainer::UtilityShutdown(int exit_code) {
  if (!utility_options_ || context_.IsEmpty()) return;
  if (isolate_->IsExecutionTerminating()) isolate_->CancelTerminateExecution();
  v8::Locker locker(isolate_);
  v8::Isolate::Scope isolate_scope(isolate_);
  v8::HandleScope handles(isolate_);
  auto context = context_.Get(isolate_);
  v8::Context::Scope context_scope(context);
  gin::TryCatch caught(isolate_);
  v8::Local<v8::Value> callback;
  if (!context->Global()->Get(context, gin::StringToV8(isolate_, "__xenonUtilityShutdown")).ToLocal(&callback) ||
      !callback->IsFunction()) return;
  v8::Local<v8::Value> code = v8::Integer::New(isolate_, exit_code);
  if (callback.As<v8::Function>()->Call(context, context->Global(), 1, &code).IsEmpty())
    LOG(ERROR) << "Utility exit listener failed: " << caught.GetStackTrace();
}

bool XenonIpcMainContainer::InitializeUtilityBindings(
    v8::Local<v8::Context> context) {
  auto set = [&](const char* name, v8::Local<v8::Value> value) {
    return context->Global()->CreateDataProperty(
        context, gin::StringToV8(isolate_, name), value).FromMaybe(false);
  };
  auto bind = [&](const char* name, auto method) {
    return set(name, gin::CreateFunctionTemplate(
        isolate_, base::BindRepeating(method, base::Unretained(this)))
        ->GetFunction(context).ToLocalChecked());
  };
  if (!bind("__xenonUtilityCreate", &XenonIpcMainContainer::NativeUtilityCreate) ||
      !bind("__xenonUtilityControl", &XenonIpcMainContainer::NativeUtilityControl) ||
      !bind("__xenonUtilityRead", &XenonIpcMainContainer::NativeUtilityRead) ||
      !set("__xenonUtilityProcess", v8::Boolean::New(isolate_, utility_options_.has_value())))
    return false;
  if (!utility_options_) return true;
  v8::Local<v8::Value> environment;
  if (!ValueToV8(base::Value(utility_options_->environment.Clone()))
          .ToLocal(&environment)) return false;
  return set("__xenonEnv", environment) &&
      set("__xenonArgv", gin::ConvertToV8(isolate_, utility_options_->argv)) &&
      set("__xenonExecArgv", gin::ConvertToV8(isolate_, utility_options_->exec_argv)) &&
      bind("__xenonUtilityProcessCall", &XenonIpcMainContainer::NativeUtilityProcessCall) &&
      bind("__xenonUtilityExit", &XenonIpcMainContainer::NativeUtilityExit);
}

bool XenonIpcMainContainer::InstallUtilityParentPort() {
  if (!utility_options_) return true;
  v8::Locker locker(isolate_);
  v8::Isolate::Scope isolate_scope(isolate_);
  v8::HandleScope handles(isolate_);
  auto context = context_.Get(isolate_);
  v8::Context::Scope context_scope(context);
  v8::Local<v8::Object> port;
  v8::Local<v8::Value> install;
  if (!message_ports_ ||
      !message_ports_->AdoptPort(std::move(utility_options_->parent_port)).ToLocal(&port) ||
      !context->Global()->Get(context, gin::StringToV8(isolate_, "__xenonInstallUtilityParentPort")).ToLocal(&install) ||
      !install->IsFunction()) {
    startup_error_ = "Failed to initialize utilityProcess parentPort";
    return false;
  }
  v8::Local<v8::Value> argument = port;
  if (install.As<v8::Function>()->Call(context, context->Global(), 1, &argument).IsEmpty()) {
    startup_error_ = "Failed to install utilityProcess parentPort facade";
    return false;
  }
  return true;
}

XenonIpcMainContainer::UtilityProcessState::UtilityProcessState(
    base::WeakPtr<XenonIpcMainContainer> owner, int id)
    : owner(std::move(owner)), id(id) {}
XenonIpcMainContainer::UtilityProcessState::~UtilityProcessState() = default;
void XenonIpcMainContainer::UtilityProcessState::OnSpawn(int32_t pid) {
  if (owner) owner->DispatchUtilityEvent(id, "spawn", pid, "");
}
void XenonIpcMainContainer::UtilityProcessState::OnExit(
    int32_t exit_code, const std::string& reason) {
  if (owner) owner->DispatchUtilityEvent(id, "exit", exit_code, reason);
}
void XenonIpcMainContainer::UtilityProcessState::OnError(const std::string& error) {
  if (owner) owner->DispatchUtilityEvent(id, "error", 0, error);
}

void XenonIpcMainContainer::NativeUtilityCreate(gin::Arguments* args) {
  v8::Local<v8::Value> raw;
  v8::Local<v8::Object> port;
  base::Value value;
  std::string error;
  if (worker_options_ || utility_options_ || shutting_down_ ||
      !utility_process_hooks_.create || !message_ports_) {
    ThrowUtilityError(isolate_, "ERR_NOT_SUPPORTED", "utilityProcess is unavailable in this context");
    return;
  }
  if (!args->GetNext(&raw) || !args->GetNext(&port) ||
      !V8ToValue(raw, &value, &error) || !value.is_dict()) {
    args->ThrowTypeError("ERR_INVALID_ARG_TYPE: Invalid utilityProcess options");
    return;
  }
  const auto& fields = value.GetDict();
  auto options = mojom::UtilityProcessOptions::New();
  const auto* module = fields.FindString("modulePath");
  const auto* directory = fields.FindString("cwd");
  const auto* environment = fields.FindDict("env");
  const auto* argv = fields.FindList("args");
  const auto* exec_argv = fields.FindList("execArgv");
  if (!module || !directory || !environment || !argv || !exec_argv) {
    args->ThrowTypeError("ERR_INVALID_ARG_TYPE: Invalid utilityProcess options");
    return;
  }
  options->module_path = *module;
  options->working_directory = *directory;
  for (const auto [key, setting] : *environment) {
    if (!setting.is_string()) {
      args->ThrowTypeError("ERR_INVALID_ARG_TYPE: Invalid utilityProcess environment");
      return;
    }
    options->environment.emplace(key, setting.GetString());
  }
  for (const auto& argument : *argv) {
    if (!argument.is_string()) {
      args->ThrowTypeError("ERR_INVALID_ARG_TYPE: Invalid utilityProcess args");
      return;
    }
    options->args.push_back(argument.GetString());
  }
  for (const auto& argument : *exec_argv) {
    if (!argument.is_string()) {
      args->ThrowTypeError("ERR_INVALID_ARG_TYPE: Invalid utilityProcess execArgv");
      return;
    }
    options->exec_argv.push_back(argument.GetString());
  }
  options->service_name = fields.FindString("serviceName") ? *fields.FindString("serviceName") : "Node Utility Process";
  options->stdout_mode = fields.FindString("stdout") ? *fields.FindString("stdout") : "inherit";
  options->stderr_mode = fields.FindString("stderr") ? *fields.FindString("stderr") : "inherit";
  options->allow_loading_unsigned_libraries = fields.FindBool("allowLoadingUnsignedLibraries").value_or(false);
  options->respond_to_auth_requests_from_main_process = fields.FindBool("respondToAuthRequestsFromMainProcess").value_or(false);
  blink::MessagePortDescriptor descriptor;
  if (!message_ports_->TakePort(port, &descriptor)) return;
  const int id = next_utility_process_id_++;
  auto state = std::make_unique<UtilityProcessState>(weak_factory_.GetWeakPtr(), id);
  auto control = state->control.BindNewPipeAndPassReceiver();
  auto client = state->receiver.BindNewPipeAndPassRemote();
  state->control.set_disconnect_handler(base::BindOnce(
      [](base::WeakPtr<XenonIpcMainContainer> owner, int id) {
        if (!owner) return;
        owner->CompleteUtilityRead(id, 1, {}, true, "Utility process control disconnected");
        owner->CompleteUtilityRead(id, 2, {}, true, "Utility process control disconnected");
        owner->DispatchUtilityEvent(id, "error", 0, "Utility process control disconnected");
        owner->DispatchUtilityEvent(id, "exit", -1, "disconnected");
      }, weak_factory_.GetWeakPtr(), id));
  utility_processes_.emplace(id, std::move(state));
  utility_process_hooks_.create.Run(std::move(options), std::move(descriptor),
      std::move(control), std::move(client), base::BindOnce(
          [](base::WeakPtr<XenonIpcMainContainer> owner, int id, bool success,
             const std::string& error) {
            if (success || !owner) return;
            base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
                base::BindOnce([](base::WeakPtr<XenonIpcMainContainer> owner, int id,
                                  std::string error) {
                  if (!owner) return;
                  owner->DispatchUtilityEvent(id, "error", 0, error);
                  owner->DispatchUtilityEvent(id, "exit", -1, "launch-failed");
                }, owner, id, error));
          }, weak_factory_.GetWeakPtr(), id));
  args->Return(id);
}

void XenonIpcMainContainer::NativeUtilityControl(gin::Arguments* args) {
  int id;
  std::string command;
  if (!args->GetNext(&id) || !args->GetNext(&command)) {
    args->ThrowTypeError("ERR_INVALID_ARG_TYPE: Invalid utility process control");
    return;
  }
  auto found = utility_processes_.find(id);
  if (found == utility_processes_.end()) { args->Return(false); return; }
  if (command == "release") {
    utility_processes_.erase(found);
    args->Return(true);
  } else if (command == "kill") {
    bool killed = false;
    if (found->second->control.is_bound()) found->second->control->Kill(&killed);
    args->Return(killed);
  } else if (command == "close-output") {
    int fd;
    if (!args->GetNext(&fd) || (fd != 1 && fd != 2)) {
      args->ThrowTypeError("ERR_INVALID_ARG_VALUE: Invalid utility output descriptor");
      return;
    }
    if (found->second->control.is_bound()) found->second->control->CloseOutput(fd);
    args->Return(true);
  } else args->ThrowTypeError("ERR_INVALID_ARG_VALUE: Unknown utility process control");
}

void XenonIpcMainContainer::NativeUtilityRead(gin::Arguments* args) {
  int id, fd, maximum;
  if (!args->GetNext(&id) || !args->GetNext(&fd) || !args->GetNext(&maximum) ||
      (fd != 1 && fd != 2) || maximum < 1 || maximum > 65536) {
    args->ThrowTypeError("ERR_OUT_OF_RANGE: Invalid utility process output read");
    return;
  }
  auto found = utility_processes_.find(id);
  if (found == utility_processes_.end() || !found->second->control.is_connected() ||
      found->second->reads.contains(fd)) {
    ThrowUtilityError(isolate_, "ERR_INVALID_STATE", "Utility output read is unavailable or already pending");
    return;
  }
  auto resolver = v8::Promise::Resolver::New(isolate_->GetCurrentContext()).ToLocalChecked();
  found->second->reads.emplace(fd, v8::Global<v8::Promise::Resolver>(isolate_, resolver));
  found->second->control->ReadOutput(fd, maximum, base::BindOnce(
      &XenonIpcMainContainer::CompleteUtilityRead, weak_factory_.GetWeakPtr(), id, fd));
  args->Return(resolver->GetPromise());
}

void XenonIpcMainContainer::CompleteUtilityRead(
    int id, int fd, const std::vector<uint8_t>& bytes, bool eof,
    const std::string& error) {
  if (shutting_down_ || context_.IsEmpty()) return;
  auto found = utility_processes_.find(id);
  if (found == utility_processes_.end()) return;
  auto pending = found->second->reads.find(fd);
  if (pending == found->second->reads.end()) return;
  v8::Locker locker(isolate_);
  v8::Isolate::Scope isolate_scope(isolate_);
  v8::HandleScope handles(isolate_);
  auto context = context_.Get(isolate_);
  v8::Context::Scope context_scope(context);
  auto resolver = pending->second.Get(isolate_);
  found->second->reads.erase(pending);
  if (!error.empty()) {
    resolver->Reject(context, v8::Exception::Error(gin::StringToV8(isolate_, error))).Check();
  } else {
    auto result = v8::Object::New(isolate_);
    result->CreateDataProperty(context, gin::StringToV8(isolate_, "data"), UtilityBytes(isolate_, bytes)).Check();
    result->CreateDataProperty(context, gin::StringToV8(isolate_, "eof"), v8::Boolean::New(isolate_, eof)).Check();
    resolver->Resolve(context, result).Check();
  }
  isolate_->PerformMicrotaskCheckpoint();
}

void XenonIpcMainContainer::DispatchUtilityEvent(
    int id, const char* type, int value, const std::string& message) {
  if (shutting_down_ || context_.IsEmpty() || !utility_processes_.contains(id)) return;
  v8::Locker locker(isolate_);
  v8::Isolate::Scope isolate_scope(isolate_);
  v8::HandleScope handles(isolate_);
  auto context = context_.Get(isolate_);
  v8::Context::Scope context_scope(context);
  gin::TryCatch caught(isolate_);
  v8::Local<v8::Value> callback;
  if (!context->Global()->Get(context, gin::StringToV8(isolate_, "__xenonDispatchUtilityProcessEvent")).ToLocal(&callback) ||
      !callback->IsFunction()) return;
  v8::Local<v8::Value> argv[] = {v8::Integer::New(isolate_, id),
      gin::StringToV8(isolate_, type), v8::Integer::New(isolate_, value),
      gin::StringToV8(isolate_, message)};
  if (callback.As<v8::Function>()->Call(context, context->Global(), 4, argv).IsEmpty())
    LOG(ERROR) << "Utility process event listener failed: " << caught.GetStackTrace();
  isolate_->PerformMicrotaskCheckpoint();
}

void XenonIpcMainContainer::NativeUtilityProcessCall(gin::Arguments* args) {
  std::string operation;
  int fd;
  if (!utility_options_ || !args->GetNext(&operation) || !args->GetNext(&fd) || fd < 0 || fd > 2) {
    args->ThrowTypeError("ERR_INVALID_ARG_TYPE: Invalid utility standard stream operation");
    return;
  }
  if (operation == "isTTY") {
#if BUILDFLAG(IS_WIN)
    args->Return(_isatty(fd) != 0);
#else
    args->Return(isatty(fd) != 0);
#endif
    return;
  }
  int result = -1;
  std::vector<uint8_t> bytes;
  if (operation == "read" && fd == 0) {
    int maximum;
    if (!args->GetNext(&maximum) || maximum < 1 || maximum > 65536) {
      args->ThrowTypeError("ERR_OUT_OF_RANGE: Invalid standard input read size"); return;
    }
    bytes.resize(maximum);
    do {
#if BUILDFLAG(IS_WIN)
      result = _read(fd, bytes.data(), maximum);
#else
      result = static_cast<int>(read(fd, bytes.data(), maximum));
#endif
    } while (result < 0 && errno == EINTR);
    if (result == 0) { args->Return(v8::Null(isolate_).As<v8::Value>()); return; }
    if (result > 0) { bytes.resize(result); args->Return(UtilityBytes(isolate_, bytes).As<v8::Value>()); return; }
  } else if (operation == "write" && (fd == 1 || fd == 2)) {
    v8::Local<v8::Value> value;
    if (!args->GetNext(&value) || !value->IsUint8Array() ||
        value.As<v8::Uint8Array>()->ByteLength() > 65536) {
      args->ThrowTypeError("ERR_INVALID_ARG_TYPE: Standard output requires at most 64 KiB of bytes"); return;
    }
    auto view = value.As<v8::Uint8Array>();
    bytes.resize(view->ByteLength());
    if (view->CopyContents(bytes.data(), bytes.size()) != bytes.size()) {
      ThrowUtilityError(isolate_, "ERR_INVALID_STATE", "Detached standard output buffer"); return;
    }
    do {
#if BUILDFLAG(IS_WIN)
      result = _write(fd, bytes.data(), static_cast<unsigned>(bytes.size()));
#else
      result = static_cast<int>(write(fd, bytes.data(), bytes.size()));
#endif
    } while (result < 0 && errno == EINTR);
    if (result >= 0) { args->Return(result); return; }
  } else {
    args->ThrowTypeError("ERR_INVALID_ARG_VALUE: Invalid standard stream direction"); return;
  }
  ThrowUtilityError(isolate_, errno == EPIPE ? "EPIPE" : "EIO", std::strerror(errno));
}

void XenonIpcMainContainer::NativeUtilityExit(gin::Arguments* args) {
  if (!utility_options_) {
    ThrowUtilityError(isolate_, "ERR_NOT_SUPPORTED", "Not a utility process");
    return;
  }
  int code;
  if (!args->GetNext(&code)) {
    args->ThrowTypeError("ERR_INVALID_ARG_TYPE: Exit code must be an integer");
    return;
  }
  RequestUtilityExit(code);
}

void XenonIpcMainContainer::RequestUtilityExit(int exit_code) {
  if (app_exit_requested_ || shutting_down_) return;
  if (!app_exit_handler_) {
    ThrowUtilityError(isolate_, "ERR_NOT_SUPPORTED", "Native utility exit is unavailable");
    return;
  }
  app_exit_requested_ = true;
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
      base::BindOnce(&XenonIpcMainContainer::NotifyAppExit,
          weak_factory_.GetWeakPtr(), exit_code));
  isolate_->TerminateExecution();
}
}  // namespace xenon::ipc
