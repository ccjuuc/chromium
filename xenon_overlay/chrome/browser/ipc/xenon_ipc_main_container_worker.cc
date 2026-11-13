// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string_view>
#include <utility>

#include "base/compiler_specific.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/task/single_thread_task_runner.h"
#include "gin/arguments.h"
#include "gin/converter.h"
#include "gin/function_template.h"
#include "v8/include/v8.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_file_descriptors.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_ipc_main_container.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_worker_engine.h"
#include "xenon_overlay/chrome/browser/napi/native_addon_event_loop.h"
#include "xenon_overlay/common/ipc/xenon_message_port_codec.h"

namespace xenon::ipc {
namespace {
void WriteUtilityDiagnostic(std::string_view text) {
  // Utility startup has already installed the real CRT/POSIX stderr fd. Keep
  // fatal JS diagnostics observable through fork({stdio: 'pipe'}), as well as
  // the host log; an ordinary JS exception is not a V8 FatalError event.
  // SAFETY: string_view supplies the exact byte count of the contiguous input;
  // fwrite reads that range and does not require a trailing NUL.
  UNSAFE_BUFFERS(std::fwrite(text.data(), 1, text.size(), stderr));
  std::fputc('\n', stderr);
  std::fflush(stderr);
}

class ContainerWorkerEnvironment final : public WorkerEnvironment {
 public:
  explicit ContainerWorkerEnvironment(scoped_refptr<XenonFileDescriptors> files)
      : files_(std::move(files)) {}
  bool Initialize(WorkerOptions options,
                  std::unique_ptr<WorkerMessage> data,
                  std::unique_ptr<v8::Isolate::CreateParams> params,
                  WorkerControl* control,
                  std::string* error) override {
    if (container_.InitializeWorker(std::move(options), std::move(data),
                                    std::move(params), files_, control)) {
      return true;
    }
    *error = container_.startup_error();
    return false;
  }
  void Deliver(std::unique_ptr<WorkerMessage> message) override {
    WorkerEngine::Event event;
    event.type = "message";
    event.message = std::move(message);
    container_.DispatchWorkerEvent(std::move(event));
  }
  void ParentPortClosed() override {
    WorkerEngine::Event event;
    event.type = "close";
    container_.DispatchWorkerEvent(std::move(event));
  }
  bool HasActiveWork() const override {
    return container_.WorkerHasActiveWork();
  }
  int BeforeExit() override { return container_.WorkerBeforeExit(); }
  int Shutdown(int code, bool terminated) override {
    return container_.FinishWorker(code, terminated);
  }
  void BeginNativeShutdown() override { container_.BeginNativeShutdown(); }
  bool PollNativeShutdown() override { return container_.PollNativeShutdown(); }
  std::string DescribeNativeShutdown() const override {
    return container_.DescribeNativeShutdown();
  }

 private:
  scoped_refptr<XenonFileDescriptors> files_;
  XenonIpcMainContainer container_;
};
bool ValidWorkerId(double id) {
  return std::isfinite(id) && id >= 0 && id <= 2147483647 &&
         std::floor(id) == id;
}
}  // namespace

bool XenonIpcMainContainer::InitializeWorker(
    WorkerOptions options,
    std::unique_ptr<WorkerMessage> data,
    std::unique_ptr<v8::Isolate::CreateParams> params,
    scoped_refptr<XenonFileDescriptors> files,
    WorkerControl* control) {
  CHECK(control && files && params && data);
  if (options.network_factory) {
    SetNetworkLoaderFactory(network::SharedURLLoaderFactory::Create(
        std::move(options.network_factory)));
  }
  worker_options_ = std::move(options);
  worker_data_ = std::move(data);
  worker_params_ = std::move(params);
  worker_control_ = control;
  file_descriptors_ = std::move(files);
  return InitializeInternal(std::nullopt);
}
bool XenonIpcMainContainer::WorkerHasActiveWork() const {
#if BUILDFLAG(ENABLE_XENON_NODE_UV_COMPAT)
  if (native_event_loop_ && native_event_loop_->HasReferencedWork()) {
    return true;
  }
#endif
  return std::ranges::any_of(
             timers_,
             [](const auto& entry) { return entry.second->referenced; }) ||
         !pending_fs_calls_.empty() || !pending_zlib_calls_.empty() ||
         !pending_http_requests_.empty() || !worker_rejections_.empty();
}
int XenonIpcMainContainer::WorkerBeforeExit() {
  v8::Locker locker(isolate_);
  v8::Isolate::Scope isolate_scope(isolate_);
  v8::HandleScope handles(isolate_);
  auto context = context_.Get(isolate_);
  v8::Context::Scope context_scope(context);
  v8::TryCatch caught(isolate_);
  v8::Local<v8::Value> callback, result;
  if (!context->Global()
           ->Get(context, gin::StringToV8(isolate_, "__xenonWorkerBeforeExit"))
           .ToLocal(&callback) ||
      !callback->IsFunction() ||
      !callback.As<v8::Function>()
           ->Call(context, context->Global(), 0, nullptr)
           .ToLocal(&result)) {
    ReportNodeException(caught.Exception());
    return 1;
  }
  isolate_->PerformMicrotaskCheckpoint();
  // beforeExit listeners and their microtasks may change or clear exitCode.
  // Read the current value without emitting beforeExit for a second time.
  v8::Local<v8::Value> process, exit_code;
  if (!context->Global()
           ->Get(context, gin::StringToV8(isolate_, "process"))
           .ToLocal(&process) ||
      !process->IsObject() ||
      !process.As<v8::Object>()
           ->Get(context, gin::StringToV8(isolate_, "exitCode"))
           .ToLocal(&exit_code)) {
    ReportNodeException(caught.Exception());
    return 1;
  }
  if (!exit_code->IsNullOrUndefined()) {
    return exit_code->Int32Value(context).FromMaybe(0);
  }
  if (main_evaluation_pending_ && !WorkerHasActiveWork()) {
    return 13;
  }
  return 0;
}
int XenonIpcMainContainer::FinishWorker(int code, bool terminated) {
  if (!terminated && initialized_ && !context_.IsEmpty()) {
    v8::Locker locker(isolate_);
    v8::Isolate::Scope isolate_scope(isolate_);
    v8::HandleScope handles(isolate_);
    auto context = context_.Get(isolate_);
    v8::Context::Scope context_scope(context);
    v8::TryCatch caught(isolate_);
    v8::Local<v8::Value> callback;
    if (context->Global()
            ->Get(context, gin::StringToV8(isolate_, "__xenonWorkerShutdown"))
            .ToLocal(&callback) &&
        callback->IsFunction()) {
      v8::Local<v8::Value> value = v8::Integer::New(isolate_, code);
      v8::Local<v8::Value> result;
      if (!callback.As<v8::Function>()
               ->Call(context, context->Global(), 1, &value)
               .ToLocal(&result)) {
        ReportNodeException(caught.Exception());
      } else {
        code = result->Int32Value(context).FromMaybe(code);
      }
    }
  }
  Shutdown();
  return code;
}
bool XenonIpcMainContainer::InitializeWorkerBindings(
    v8::Local<v8::Context> context) {
  auto global = context->Global();
  if (worker_control_ || utility_options_) {
    if (!global
             ->SetPrivate(
                 context,
                 v8::Private::ForApi(
                     isolate_,
                     gin::StringToV8(isolate_, "xenon.worker.environment")
                         .As<v8::String>()),
                 v8::External::New(isolate_, this,
                                   v8::kExternalPointerTypeTagDefault))
             .FromMaybe(false)) {
      return false;
    }
    isolate_->SetPromiseRejectCallback(&OnWorkerPromiseRejected);
    isolate_->AddMessageListener(
        &OnWorkerV8Message,
        v8::External::New(isolate_, this, v8::kExternalPointerTypeTagDefault));
  }
  auto set = [&](const char* key, v8::Local<v8::Value> value) {
    return global
        ->CreateDataProperty(context, gin::StringToV8(isolate_, key), value)
        .FromMaybe(false);
  };
  auto bind = [&](const char* key, auto method) {
    return set(
        key, gin::CreateFunctionTemplate(
                 isolate_, base::BindRepeating(method, base::Unretained(this)))
                 ->GetFunction(context)
                 .ToLocalChecked());
  };
  if (!bind("__xenonWorkerCreate",
            &XenonIpcMainContainer::NativeWorkerCreate) ||
      !bind("__xenonWorkerPost", &XenonIpcMainContainer::NativeWorkerPost) ||
      !bind("__xenonWorkerControl",
            &XenonIpcMainContainer::NativeWorkerControl) ||
      !bind("__xenonWorkerExit", &XenonIpcMainContainer::NativeWorkerExit) ||
      !bind("__xenonMarkAsUncloneable",
            &XenonIpcMainContainer::NativeMarkAsUncloneable)) {
    return false;
  }
  v8::Local<v8::Value> data = v8::Null(isolate_);
  v8::Local<v8::Value> limits = v8::Object::New(isolate_);
  if (worker_options_) {
    if (!worker_data_->Deserialize(context).ToLocal(&data) ||
        !ValueToV8(base::Value(worker_options_->resource_limits.Clone()))
             .ToLocal(&limits)) {
      return false;
    }
    worker_data_.reset();
  }
  return set("__xenonWorkerThreadId",
             v8::Integer::New(isolate_,
                              worker_options_ ? worker_options_->id : 0)) &&
         set("__xenonWorkerThreadName",
             gin::StringToV8(isolate_,
                             worker_options_ ? worker_options_->name : "")) &&
         set("__xenonWorkerResourceLimits", limits) &&
         set("__xenonWorkerData", data);
}
void XenonIpcMainContainer::NativeWorkerCreate(gin::Arguments* args) {
  if (worker_control_) {
    args->ThrowTypeError(
        "ERR_NOT_SUPPORTED: nested Worker creation is unavailable");
    return;
  }
  std::string source;
  v8::Local<v8::Value> raw_options, data, transfers;
  base::Value options;
  std::string error;
  if (!args->GetNext(&source) || !args->GetNext(&raw_options) ||
      !args->GetNext(&data) || !args->GetNext(&transfers) ||
      !V8ToValue(raw_options, &options, &error) || !options.is_dict()) {
    args->ThrowTypeError("ERR_INVALID_ARG_TYPE: invalid Worker arguments");
    return;
  }
  if (shutting_down_ || source.size() > 16 * 1024 * 1024) {
    args->ThrowTypeError(
        "ERR_INVALID_STATE: Worker source is too large or application stopped");
    return;
  }
  WorkerOptions config;
  const auto& values = options.GetDict();
  config.eval = values.FindBool("eval").value_or(false);
  config.track_unmanaged_fds =
      values.FindBool("trackUnmanagedFds").value_or(true);
  config.app_path = app_path_;
  config.executable_path = executable_path_;
  config.resources_directory = resources_directory_;
  if (file_descriptors_->GetWorkingDirectory(&config.working_directory) != 0) {
    args->ThrowTypeError("Process working directory is unavailable");
    return;
  }
  if (const auto* name = values.FindString("name")) {
    config.name = *name;
  }
  config.filename = config.eval ? app_path_.AppendASCII("[worker eval].cjs")
                                : base::FilePath::FromUTF8Unsafe(source);
  if (!config.filename.IsAbsolute() || source.find('\0') != std::string::npos ||
      (!module_root_.empty() && !module_root_.IsParent(config.filename))) {
    args->ThrowTypeError(
        "ERR_ACCESS_DENIED: Worker entry must be inside the application "
        "directory");
    return;
  }
  if (config.eval) {
    config.source = std::move(source);
  }
  config.argv = {executable_path_.AsUTF8Unsafe(),
                 config.filename.AsUTF8Unsafe()};
  if (const auto* argv = values.FindList("argv")) {
    for (const auto& value : *argv) {
      if (!value.is_string() ||
          value.GetString().find('\0') != std::string::npos) {
        args->ThrowTypeError(
            "ERR_INVALID_ARG_TYPE: Worker argv must contain strings");
        return;
      }
      config.argv.push_back(value.GetString());
    }
  }
  if (const auto* env = values.FindDict("env")) {
    for (const auto [key, value] : *env) {
      if (!value.is_string() || key.find('\0') != std::string::npos ||
          value.GetString().find('\0') != std::string::npos) {
        args->ThrowTypeError(
            "ERR_INVALID_ARG_TYPE: Worker env must contain strings");
        return;
      }
    }
    config.environment = env->Clone();
  }
  if (const auto* limits = values.FindDict("resourceLimits")) {
    for (const auto [name, value] : *limits) {
      const auto amount = value.GetIfDouble();
      if (!amount || !std::isfinite(*amount) || *amount <= 0 ||
          *amount > 65536) {
        args->ThrowTypeError("ERR_OUT_OF_RANGE: invalid Worker resource limit");
        return;
      }
      if (name == "maxOldGenerationSizeMb") {
        config.old_generation_mb = std::max(2.0, *amount);
      } else if (name == "maxYoungGenerationSizeMb") {
        config.young_generation_mb = *amount;
      } else if (name == "stackSizeMb" && *amount >= 0.5 && *amount <= 64) {
        config.stack_mb = *amount;
      } else {
        args->ThrowTypeError(
            "ERR_NOT_SUPPORTED: unsupported Worker resource limit");
        return;
      }
    }
  }
  auto prepared =
      PreparedWorkerMessage::Serialize(context_.Get(isolate_), data, transfers);
  if (!prepared) {
    return;
  }
  if (network_loader_factory_) {
    config.network_factory = network_loader_factory_->Clone();
  }
  if (!worker_engine_) {
    worker_engine_ = std::make_unique<WorkerEngine>(
        base::BindRepeating(
            [](scoped_refptr<XenonFileDescriptors> files)
                -> std::unique_ptr<WorkerEnvironment> {
              return std::make_unique<ContainerWorkerEnvironment>(
                  std::move(files));
            },
            file_descriptors_),
        base::BindRepeating(&XenonIpcMainContainer::DispatchWorkerEvent,
                            weak_factory_.GetWeakPtr()));
  }
  if (!worker_engine_->Create(&config, std::move(prepared), isolate_, &error)) {
    if (!error.empty()) {
      args->ThrowTypeError("ERR_WORKER_INIT_FAILED: " + error);
    }
    return;
  }
  auto result = base::DictValue()
                    .Set("id", config.id)
                    .Set("threadName", config.name)
                    .Set("resourceLimits", std::move(config.resource_limits));
  v8::Local<v8::Value> value;
  if (ValueToV8(base::Value(std::move(result))).ToLocal(&value)) {
    args->Return(value);
  }
}
void XenonIpcMainContainer::NativeWorkerPost(gin::Arguments* args) {
  double id;
  v8::Local<v8::Value> value, transfers;
  if (!args->GetNext(&id) || !ValidWorkerId(id) || !args->GetNext(&value) ||
      !args->GetNext(&transfers)) {
    args->ThrowTypeError("ERR_INVALID_ARG_TYPE: invalid Worker message");
    return;
  }
  auto prepared = PreparedWorkerMessage::Serialize(context_.Get(isolate_),
                                                   value, transfers);
  if (!prepared) {
    return;
  }
  std::string error;
  const bool result =
      id == 0 ? worker_control_ && worker_control_->PostToParent(
                                       std::move(prepared), isolate_, &error)
              : worker_engine_ &&
                    worker_engine_->Post(static_cast<int>(id),
                                         std::move(prepared), isolate_, &error);
  if (!error.empty()) {
    args->ThrowTypeError(error);
    return;
  }
  args->Return(result);
}
void XenonIpcMainContainer::NativeWorkerControl(gin::Arguments* args) {
  double id;
  std::string command;
  if (!args->GetNext(&id) || !ValidWorkerId(id) || !args->GetNext(&command)) {
    args->ThrowTypeError("ERR_INVALID_ARG_TYPE: invalid Worker control");
    return;
  }
  if (id == 0 && worker_control_) {
    if (command == "close") {
      worker_control_->CloseParentPort();
    } else if (command == "ref" || command == "unref") {
      worker_control_->SetParentPortReferenced(command == "ref");
    } else {
      args->Return(false);
      return;
    }
    args->Return(true);
    return;
  }
  args->Return(worker_engine_ &&
               worker_engine_->Control(static_cast<int>(id), command));
}
void XenonIpcMainContainer::NativeWorkerExit(gin::Arguments* args) {
  int code = 0;
  if (!worker_control_ || !args->GetNext(&code)) {
    args->ThrowTypeError(
        "ERR_INVALID_ARG_TYPE: Worker exit code must be an integer");
    return;
  }
  worker_control_->Exit(code);
}
void XenonIpcMainContainer::NativeMarkAsUncloneable(gin::Arguments* args) {
  v8::Local<v8::Value> value;
  if (!args->GetNext(&value)) {
    return;
  }
  std::ignore = MarkAsUncloneable(context_.Get(isolate_), value);
}
void XenonIpcMainContainer::DispatchWorkerEvent(WorkerEngine::Event event) {
  if (!initialized_ || shutting_down_ || dispatch_worker_event_.IsEmpty()) {
    return;
  }
  v8::Locker locker(isolate_);
  v8::Isolate::Scope isolate_scope(isolate_);
  v8::HandleScope handles(isolate_);
  auto context = context_.Get(isolate_);
  v8::Context::Scope context_scope(context);
  v8::TryCatch caught(isolate_);
  v8::Local<v8::Value> payload;
  if (event.message ? !event.message->Deserialize(context).ToLocal(&payload)
                    : !ValueToV8(event.payload).ToLocal(&payload)) {
    if (worker_control_ || utility_options_) {
      ReportNodeException(caught.Exception());
    }
    return;
  }
  v8::Local<v8::Value> arguments[] = {v8::Integer::New(isolate_, event.id),
                                      gin::StringToV8(isolate_, event.type),
                                      payload};
  if (dispatch_worker_event_.Get(isolate_)
          ->Call(context, context->Global(), 3, arguments)
          .IsEmpty()) {
    if (worker_control_ || utility_options_) {
      ReportNodeException(caught.Exception());
    } else {
      LOG(ERROR) << "Worker event handler threw";
    }
  }
  isolate_->PerformMicrotaskCheckpoint();
}
bool XenonIpcMainContainer::ReportNodeException(
    v8::Local<v8::Value> exception) {
  if ((!worker_control_ && !utility_options_) || exception.IsEmpty() ||
      isolate_->IsExecutionTerminating()) {
    return false;
  }
  v8::TryCatch protect(isolate_);
  auto context = context_.Get(isolate_);
  v8::Local<v8::Value> process, emit, handled;
  if (context->Global()
          ->Get(context, gin::StringToV8(isolate_, "process"))
          .ToLocal(&process) &&
      process->IsObject() &&
      process.As<v8::Object>()
          ->Get(context, gin::StringToV8(isolate_, "emit"))
          .ToLocal(&emit) &&
      emit->IsFunction()) {
    v8::Local<v8::Value> arguments[] = {
        gin::StringToV8(isolate_, "uncaughtException"), exception,
        gin::StringToV8(isolate_, "uncaughtException")};
    if (emit.As<v8::Function>()
            ->Call(context, process, 3, arguments)
            .ToLocal(&handled) &&
        handled->IsTrue()) {
      return true;
    }
    if (protect.HasTerminated() || isolate_->IsExecutionTerminating()) {
      // An uncaughtException listener may itself call process.exit(). The
      // termination sentinel is not a JavaScript error to inspect or report.
      return false;
    }
    if (protect.HasCaught()) {
      exception = protect.Exception();
    }
    protect.Reset();
  }
  base::DictValue error;
  error.Set("name", "Error");
  error.Set("message", gin::V8ToString(isolate_, exception));
  if (exception->IsObject()) {
    for (const char* key : {"name", "message", "stack", "code"}) {
      v8::Local<v8::Value> value;
      if (exception.As<v8::Object>()
              ->Get(context_.Get(isolate_), gin::StringToV8(isolate_, key))
              .ToLocal(&value) &&
          value->IsString()) {
        error.Set(key, gin::V8ToString(isolate_, value));
      }
      protect.Reset();
    }
  }
  int exit_code = 1;
  if (!process.IsEmpty() && process->IsObject()) {
    v8::Local<v8::Value> exiting, code;
    if (process.As<v8::Object>()
            ->Get(context, gin::StringToV8(isolate_, "_exiting"))
            .ToLocal(&exiting) &&
        exiting->IsTrue() &&
        process.As<v8::Object>()
            ->Get(context, gin::StringToV8(isolate_, "exitCode"))
            .ToLocal(&code)) {
      exit_code = code->Int32Value(context).FromMaybe(1);
    }
    protect.Reset();
  }
  if (worker_control_) {
    worker_control_->Fail(std::move(error), exit_code);
  } else {
    const std::string* details = error.FindString("stack");
    if (!details) {
      details = error.FindString("message");
    }
    LOG(ERROR) << "Uncaught utilityProcess exception: "
               << (details ? *details : "Unknown error");
    WriteUtilityDiagnostic(details ? *details : "Unknown error");
    RequestUtilityExit(exit_code);
  }
  return false;
}
void XenonIpcMainContainer::OnWorkerV8Message(v8::Local<v8::Message> message,
                                              v8::Local<v8::Value> data) {
  auto* self = static_cast<XenonIpcMainContainer*>(
      data.As<v8::External>()->Value(v8::kExternalPointerTypeTagDefault));
  self->ReportNodeException(v8::Exception::Error(message->Get()));
}
void XenonIpcMainContainer::OnWorkerPromiseRejected(
    v8::PromiseRejectMessage message) {
  auto* isolate = v8::Isolate::GetCurrent();
  v8::HandleScope handles(isolate);
  auto context = isolate->GetCurrentContext();
  if (context.IsEmpty()) {
    return;
  }
  v8::Local<v8::Value> value;
  if (!context->Global()
           ->GetPrivate(
               context,
               v8::Private::ForApi(
                   isolate, gin::StringToV8(isolate, "xenon.worker.environment")
                                .As<v8::String>()))
           .ToLocal(&value) ||
      !value->IsExternal()) {
    return;
  }
  auto* self = static_cast<XenonIpcMainContainer*>(
      value.As<v8::External>()->Value(v8::kExternalPointerTypeTagDefault));
  if (self->shutting_down_) {
    return;
  }
  if (message.GetEvent() == v8::kPromiseHandlerAddedAfterReject) {
    for (auto it = self->worker_rejections_.begin();
         it != self->worker_rejections_.end(); ++it) {
      if (it->second.promise.Get(isolate) == message.GetPromise()) {
        self->worker_rejections_.erase(it);
        break;
      }
    }
  } else if (message.GetEvent() == v8::kPromiseRejectWithNoHandler) {
    const uint64_t id = ++self->next_worker_rejection_id_;
    WorkerRejection rejection;
    rejection.promise.Reset(isolate, message.GetPromise());
    rejection.reason.Reset(isolate, message.GetValue());
    self->worker_rejections_.emplace(id, std::move(rejection));
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(&XenonIpcMainContainer::ProcessWorkerPromiseRejection,
                       self->weak_factory_.GetWeakPtr(), id));
  }
}
void XenonIpcMainContainer::ProcessWorkerPromiseRejection(uint64_t id) {
  auto it = worker_rejections_.find(id);
  if (it == worker_rejections_.end() || shutting_down_ ||
      (utility_options_ && app_exit_requested_)) {
    return;
  }
  v8::Locker locker(isolate_);
  v8::Isolate::Scope isolate_scope(isolate_);
  v8::HandleScope handles(isolate_);
  auto context = context_.Get(isolate_);
  v8::Context::Scope context_scope(context);
  auto promise = it->second.promise.Get(isolate_);
  auto reason = it->second.reason.Get(isolate_);
  worker_rejections_.erase(it);
  v8::TryCatch caught(isolate_);
  v8::Local<v8::Value> process, emit, handled;
  if (context->Global()
          ->Get(context, gin::StringToV8(isolate_, "process"))
          .ToLocal(&process) &&
      process->IsObject() &&
      process.As<v8::Object>()
          ->Get(context, gin::StringToV8(isolate_, "emit"))
          .ToLocal(&emit) &&
      emit->IsFunction()) {
    v8::Local<v8::Value> arguments[] = {
        gin::StringToV8(isolate_, "unhandledRejection"), reason, promise};
    if (emit.As<v8::Function>()
            ->Call(context, process, 3, arguments)
            .ToLocal(&handled) &&
        handled->IsTrue()) {
      return;
    }
    if (caught.HasCaught()) {
      auto exception = caught.Exception();
      caught.Reset();
      ReportNodeException(exception);
      return;
    }
    caught.Reset();
  }
  if (utility_options_) {
    // Electron's NodeService selects warn-with-error-code for unhandled
    // rejections. A listener owns recovery; otherwise retain exitCode=1 and
    // report the reason without converting rejection into uncaughtException.
    // gin::V8ToString only accepts actual strings; passing an Error loses its
    // message. V8's diagnostic conversion handles arbitrary reasons without
    // calling user-defined getters or toString hooks.
    v8::Local<v8::String> reason_text;
    if (!reason->ToDetailString(context).ToLocal(&reason_text)) {
      return;
    }
    const std::string diagnostic = "UnhandledPromiseRejectionWarning: " +
                                   gin::V8ToString(isolate_, reason_text);
    LOG(WARNING) << diagnostic;
    WriteUtilityDiagnostic(diagnostic);
    if (!process.IsEmpty() && process->IsObject()) {
      std::ignore = process.As<v8::Object>()->Set(
          context, gin::StringToV8(isolate_, "exitCode"),
          v8::Integer::New(isolate_, 1));
    }
  } else {
    ReportNodeException(reason);
  }
}
}  // namespace xenon::ipc
