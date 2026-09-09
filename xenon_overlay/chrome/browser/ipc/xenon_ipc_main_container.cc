// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_ipc_main_container.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/base_paths.h"
#include "base/check.h"
#include "base/command_line.h"
#include "base/compiler_specific.h"
#include "base/containers/fixed_flat_map.h"
#include "base/containers/span.h"
#include "base/environment.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/path_service.h"
#include "base/process/process_handle.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/system/sys_info.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "crypto/openssl_util.h"
#include "third_party/boringssl/src/include/openssl/evp.h"

#if BUILDFLAG(IS_WIN)
#include <windows.h>
#endif
#include "chrome/common/chrome_paths.h"
#include "components/version_info/version_info.h"
#include "gin/arguments.h"
#include "gin/converter.h"
#include "gin/dictionary.h"
#include "gin/function_template.h"
#include "gin/public/isolate_holder.h"
#include "gin/try_catch.h"
#include "gin/v8_initializer.h"
#include "ui/base/resource/resource_bundle.h"
#include "v8/include/v8-array-buffer.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-external.h"
#include "v8/include/v8-function.h"
#include "v8/include/v8-initialization.h"
#include "v8/include/v8-json.h"
#include "v8/include/v8-locker.h"
#include "v8/include/v8-message.h"
#include "v8/include/v8-object.h"
#include "v8/include/v8-primitive.h"
#include "v8/include/v8-promise.h"
#include "v8/include/v8-script.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_app_runtime.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_os_bridge.h"
#include "xenon_overlay/chrome/browser/napi/napi_loader.h"
#include "xenon_overlay/public/xenon_ipc_switches.h"
#include "xenon_overlay/resources/grit/xenon_resources.h"

namespace xenon::ipc {

namespace {

constexpr int kMaxPackageMainDepth = 32;
constexpr int kMaxIpcValueDepth = 32;
constexpr uint64_t kMaxSafeInteger = 9007199254740991ULL;

bool ConvertV8ToValue(v8::Isolate* isolate,
                      v8::Local<v8::Context> context,
                      v8::Local<v8::Value> value,
                      base::Value* output,
                      std::string* error,
                      int depth) {
  if (depth > kMaxIpcValueDepth) {
    *error = "IPC value is nested too deeply";
    return false;
  }
  if (value.IsEmpty() || value->IsUndefined() || value->IsNull() ||
      value->IsFunction() || value->IsSymbol()) {
    // JSON.stringify([fn]) becomes [null]. Isolate stringify(fn) is not JSON.
    *output = base::Value();
    return true;
  }
  if (value->IsBigInt()) {
    bool lossless = false;
    const uint64_t as_uint = value.As<v8::BigInt>()->Uint64Value(&lossless);
    if (lossless && as_uint <= kMaxSafeInteger) {
      *output = base::Value(static_cast<double>(as_uint));
    } else {
      *output = base::Value(base::NumberToString(as_uint));
    }
    return true;
  }
  if (value->IsArrayBuffer() || value->IsArrayBufferView()) {
    v8::Local<v8::ArrayBuffer> buffer;
    size_t byte_offset = 0;
    size_t byte_length = 0;
    if (value->IsArrayBuffer()) {
      buffer = value.As<v8::ArrayBuffer>();
      byte_length = buffer->ByteLength();
    } else {
      v8::Local<v8::ArrayBufferView> view = value.As<v8::ArrayBufferView>();
      buffer = view->Buffer();
      byte_offset = view->ByteOffset();
      byte_length = view->ByteLength();
    }
    std::shared_ptr<v8::BackingStore> store = buffer->GetBackingStore();
    if (!store || byte_offset > store->ByteLength() ||
        byte_length > store->ByteLength() - byte_offset) {
      *error = "Invalid ArrayBuffer";
      return false;
    }
    base::span<const uint8_t> bytes = UNSAFE_BUFFERS(base::span(
        static_cast<const uint8_t*>(store->Data()), store->ByteLength()));
    auto slice = bytes.subspan(byte_offset, byte_length);
    *output = base::Value(
        base::Value::BlobStorage(slice.begin(), slice.end()));
    return true;
  }
  if (value->IsArray()) {
    v8::Local<v8::Array> array = value.As<v8::Array>();
    const uint32_t length = array->Length();
    base::ListValue list;
    for (uint32_t i = 0; i < length; ++i) {
      v8::Local<v8::Value> item;
      if (!array->Get(context, i).ToLocal(&item)) {
        *error = "Failed to read IPC array element";
        return false;
      }
      base::Value converted;
      if (!ConvertV8ToValue(isolate, context, item, &converted, error,
                            depth + 1)) {
        return false;
      }
      list.Append(std::move(converted));
    }
    *output = base::Value(std::move(list));
    return true;
  }
  v8::Local<v8::String> json;
  if (!v8::JSON::Stringify(context, value).ToLocal(&json)) {
    *output = base::Value();
    return true;
  }
  v8::String::Utf8Value utf8(isolate, json);
  if (*utf8 == nullptr || **utf8 == '\0') {
    *output = base::Value();
    return true;
  }
  std::optional<base::Value> parsed =
      base::JSONReader::Read(*utf8, base::JSON_PARSE_RFC);
  if (!parsed) {
    *output = base::Value();
    return true;
  }
  *output = std::move(*parsed);
  return true;
}

std::string GetMainBootstrapSource() {
  // Production and local builds must execute the same packed resource. The
  // source-tree fallback is only for targets that do not initialize a bundle.
  if (ui::ResourceBundle::HasSharedInstance()) {
    std::string resource =
        ui::ResourceBundle::GetSharedInstance().LoadDataResourceString(
            IDR_XENON_IPC_MAIN_BOOTSTRAP_JS);
    if (!resource.empty()) {
      return resource;
    }
  }
  base::FilePath root;
  if (base::PathService::Get(base::DIR_SRC_TEST_DATA_ROOT, &root)) {
    base::FilePath file = root.AppendASCII("xenon_overlay")
                              .AppendASCII("resources")
                              .AppendASCII("ipc")
                              .AppendASCII("xenon_ipc_main_bootstrap.js");
    std::string source;
    if (base::ReadFileToString(file, &source) && !source.empty()) {
      return source;
    }
  }
  return {};
}

void PromiseRejectCallback(v8::PromiseRejectMessage message) {
  if (message.GetEvent() != v8::kPromiseRejectWithNoHandler) {
    return;
  }
  v8::Isolate* isolate = v8::Isolate::GetCurrent();
  if (!isolate) {
    LOG(ERROR) << "ipcMain unhandledRejection (no isolate)";
    return;
  }
  v8::HandleScope handle_scope(isolate);
  v8::Local<v8::Value> value = message.GetValue();
  std::string text = "<empty>";
  if (!value.IsEmpty()) {
    v8::String::Utf8Value utf8(isolate, value);
    if (*utf8) {
      text.assign(*utf8, utf8.length());
    }
  }
  LOG(ERROR) << "ipcMain unhandledRejection: " << text;
}

class ScopedV8Context {
 public:
  ScopedV8Context(v8::Isolate* isolate, const v8::Global<v8::Context>& context)
      : locker_(isolate),
        isolate_scope_(isolate),
        handle_scope_(isolate),
        context_(context.Get(isolate)),
        context_scope_(context_) {}

  v8::Local<v8::Context> context() const { return context_; }

 private:
  v8::Locker locker_;
  v8::Isolate::Scope isolate_scope_;
  v8::HandleScope handle_scope_;
  v8::Local<v8::Context> context_;
  v8::Context::Scope context_scope_;
};

std::string PlatformName() {
#if BUILDFLAG(IS_WIN)
  return "win32";
#elif BUILDFLAG(IS_MAC)
  return "darwin";
#else
  return "linux";
#endif
}

std::string ArchitectureName() {
#if defined(ARCH_CPU_X86_64)
  return "x64";
#elif defined(ARCH_CPU_ARM64)
  return "arm64";
#elif defined(ARCH_CPU_X86)
  return "ia32";
#else
  return "unknown";
#endif
}

// gin::TryCatch::GetStackTrace() appends the full source line, which for
// webpack bundles can be hundreds of KB. Keep the exception text and a short
// location hint; drop oversized source-line dumps.
std::string FormatCaughtError(v8::Isolate* /*isolate*/,
                               gin::TryCatch& try_catch) {
  if (!try_catch.HasCaught()) {
    return {};
  }

  std::string full = try_catch.GetStackTrace();
  if (full.empty()) {
    return "Unknown JavaScript exception";
  }

  // Keep the first line (exception message) and any subsequent short stack
  // frames; drop huge webpack source-line dumps.
  std::string out;
  size_t start = 0;
  int kept_lines = 0;
  while (start < full.size() && kept_lines < 12) {
    const size_t end = full.find('\n', start);
    const size_t line_end = end == std::string::npos ? full.size() : end;
    const size_t line_len = line_end - start;
    const bool huge_source_dump =
        line_len > 512 && kept_lines > 0 &&
        full.compare(start, std::min<size_t>(line_len, 3), "at ") != 0 &&
        (line_len < 2 ||
         full.compare(start, std::min<size_t>(line_len, 2), "  ") != 0);
    if (!huge_source_dump) {
      if (!out.empty()) {
        out.push_back('\n');
      }
      const size_t copy_len = std::min<size_t>(line_len, 1024);
      out.append(full, start, copy_len);
      if (line_len > 1024) {
        out.append("...[truncated]");
      }
      ++kept_lines;
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  return out.empty() ? full.substr(0, std::min<size_t>(full.size(), 1024)) : out;
}

std::optional<base::FilePath> NormalizedExecutableDirectory(
    const base::FilePath& app_executable) {
  base::FilePath executable_directory = app_executable.DirName();
  if (executable_directory.empty() &&
      !base::PathService::Get(base::DIR_EXE, &executable_directory)) {
    return std::nullopt;
  }
  base::FilePath normalized;
  if (base::NormalizeFilePath(executable_directory, &normalized)) {
    return normalized;
  }
  return executable_directory;
}

bool IsNativeAddonBesideExecutable(const base::FilePath& normalized,
                                   const base::FilePath& app_executable) {
  if (!base::FilePath::CompareEqualIgnoreCase(normalized.Extension(),
                                              FILE_PATH_LITERAL(".node"))) {
    return false;
  }
  std::optional<base::FilePath> executable_directory =
      NormalizedExecutableDirectory(app_executable);
  return executable_directory &&
         base::FilePath::CompareEqualIgnoreCase(
             normalized.DirName().value(), executable_directory->value());
}

}  // namespace

struct XenonIpcMainContainer::PromiseReplyContext {
  base::WeakPtr<XenonIpcMainContainer> owner;
  InvokeCallback callback;
};

struct XenonIpcMainContainer::RendererEndpoint {
  int32_t process_id;
  int32_t frame_id;
  int32_t window_id;
  mojo::Remote<xenon::ipc::mojom::IpcRenderer> remote;
};

XenonIpcMainContainer::XenonIpcMainContainer() = default;

XenonIpcMainContainer::~XenonIpcMainContainer() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  Shutdown();
}

bool XenonIpcMainContainer::Initialize() {
  return InitializeInternal(std::nullopt);
}

bool XenonIpcMainContainer::Initialize(EmbeddedMainModule main_module) {
  return InitializeInternal(std::move(main_module));
}

void XenonIpcMainContainer::SetNativeAddonHooks(NativeAddonHooks hooks) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  native_addon_hooks_ = std::move(hooks);
}

void XenonIpcMainContainer::SetWindowHooks(WindowHooks hooks) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  window_hooks_ = std::move(hooks);
}

bool XenonIpcMainContainer::InitializeInternal(
    std::optional<EmbeddedMainModule> main_module) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (initialized_) {
    return true;
  }

  startup_error_.clear();
  embedded_main_source_.reset();
  if (main_module) {
    if (main_module->source.empty()) {
      startup_error_ = "Embedded main JavaScript source is empty";
      return false;
    }
    if (main_module->virtual_path.empty() ||
        !main_module->virtual_path.IsAbsolute()) {
      startup_error_ =
          "Embedded main JavaScript requires an absolute virtual path";
      return false;
    }
    main_script_path_ = std::move(main_module->virtual_path);
    app_path_ = main_module->app_path.empty() ? main_script_path_.DirName()
                                               : main_module->app_path;
    // Paths derived from the executable command line may preserve the caller's
    // spelling (for example, "release_64"), while resolving a required chunk
    // returns the filesystem spelling ("Release_64"). FilePath ancestry checks
    // are lexical, so canonicalize both sides before enforcing the module root.
    base::FilePath normalized_main_script;
    base::FilePath normalized_app_path;
    if (base::NormalizeFilePath(main_script_path_, &normalized_main_script)) {
      main_script_path_ = std::move(normalized_main_script);
    }
    if (base::NormalizeFilePath(app_path_, &normalized_app_path)) {
      app_path_ = std::move(normalized_app_path);
    }
    if (!app_path_.IsAbsolute() ||
        (main_script_path_ != app_path_ &&
         !app_path_.IsParent(main_script_path_))) {
      startup_error_ =
          "Embedded main JavaScript is outside its application directory";
      return false;
    }
    module_root_ = app_path_;
    app_name_ = std::move(main_module->app_name);
    app_version_ = std::move(main_module->app_version);
    executable_path_ = std::move(main_module->executable_path);
    default_user_agent_ = std::move(main_module->default_user_agent);
    renderer_url_mappings_ = std::move(main_module->renderer_url_mappings);
    renderer_base_url_ = std::move(main_module->renderer_base_url);
    embedded_main_source_ = std::move(main_module->source);
  } else if (!ResolveConfiguredMainScript()) {
    return false;
  }

  if (!ResolveAppExecutable(executable_path_, &executable_path_,
                            &startup_error_)) {
    return false;
  }

  if (!gin::IsolateHolder::Initialized()) {
#if defined(V8_USE_EXTERNAL_STARTUP_DATA)
    gin::V8Initializer::LoadV8Snapshot();
#endif
    gin::IsolateHolder::Initialize(gin::IsolateHolder::kNonStrictMode,
                                   gin::ArrayBufferAllocator::SharedInstance());
  }

  isolate_holder_ = std::make_unique<gin::IsolateHolder>(
      base::SingleThreadTaskRunner::GetCurrentDefault(),
      gin::IsolateHolder::kUseLocker,
      gin::IsolateHolder::IsolateType::kUtility);
  isolate_ = isolate_holder_->isolate();
  if (!isolate_) {
    startup_error_ = "Failed to create the main JavaScript isolate";
    return false;
  }
  isolate_->SetPromiseRejectCallback(&PromiseRejectCallback);

  {
    v8::Locker locker(isolate_);
    v8::Isolate::Scope isolate_scope(isolate_);
    v8::HandleScope handle_scope(isolate_);
    v8::Local<v8::Context> context = v8::Context::New(isolate_);
    context_.Reset(isolate_, context);
    v8::Context::Scope context_scope(context);

    v8::Local<v8::Object> global = context->Global();
    global
        ->Set(
            context, v8::String::NewFromUtf8Literal(isolate_, "__xenonAppPath"),
            v8::String::NewFromUtf8(isolate_, app_path_.AsUTF8Unsafe().c_str())
                .ToLocalChecked())
        .Check();
    global
        ->Set(context,
              v8::String::NewFromUtf8Literal(isolate_, "__xenonMainScript"),
              v8::String::NewFromUtf8(isolate_,
                                      main_script_path_.AsUTF8Unsafe().c_str())
                  .ToLocalChecked())
        .Check();
    global
        ->Set(context,
              v8::String::NewFromUtf8Literal(isolate_, "__xenonExecPath"),
              v8::String::NewFromUtf8(isolate_,
                                      executable_path_.AsUTF8Unsafe().c_str())
                  .ToLocalChecked())
        .Check();
    global
        ->Set(context, v8::String::NewFromUtf8Literal(isolate_, "__xenonPid"),
              v8::Integer::NewFromUnsigned(isolate_, base::GetCurrentProcId()))
        .Check();
    global
        ->Set(context,
              v8::String::NewFromUtf8Literal(isolate_, "__xenonPlatform"),
              v8::String::NewFromUtf8(isolate_, PlatformName().c_str())
                  .ToLocalChecked())
        .Check();
    global
        ->Set(context, v8::String::NewFromUtf8Literal(isolate_, "__xenonArch"),
              v8::String::NewFromUtf8(isolate_, ArchitectureName().c_str())
                  .ToLocalChecked())
        .Check();
    global
        ->Set(context,
              v8::String::NewFromUtf8Literal(isolate_, "__xenonOsRelease"),
              v8::String::NewFromUtf8(
                  isolate_, base::SysInfo::OperatingSystemVersion().c_str())
                  .ToLocalChecked())
        .Check();
    global
        ->Set(context,
              v8::String::NewFromUtf8Literal(isolate_, "__xenonAppName"),
              v8::String::NewFromUtf8(isolate_, app_name_.c_str())
                  .ToLocalChecked())
        .Check();
    global
        ->Set(context,
              v8::String::NewFromUtf8Literal(isolate_, "__xenonAppVersion"),
              v8::String::NewFromUtf8(isolate_, app_version_.c_str())
                  .ToLocalChecked())
        .Check();
    global
        ->Set(context,
              v8::String::NewFromUtf8Literal(isolate_, "__xenonUserAgent"),
              v8::String::NewFromUtf8(isolate_, default_user_agent_.c_str())
                  .ToLocalChecked())
        .Check();
    v8::Local<v8::Array> renderer_url_mappings =
        v8::Array::New(isolate_, renderer_url_mappings_.size());
    for (size_t i = 0; i < renderer_url_mappings_.size(); ++i) {
      v8::Local<v8::Object> mapping = v8::Object::New(isolate_);
      mapping
          ->Set(context,
                v8::String::NewFromUtf8Literal(isolate_, "sourcePathPrefix"),
                v8::String::NewFromUtf8(
                    isolate_, renderer_url_mappings_[i].first.c_str())
                    .ToLocalChecked())
          .Check();
      mapping
          ->Set(context,
                v8::String::NewFromUtf8Literal(isolate_, "targetBaseUrl"),
                v8::String::NewFromUtf8(
                    isolate_, renderer_url_mappings_[i].second.c_str())
                    .ToLocalChecked())
          .Check();
      renderer_url_mappings
          ->Set(context, static_cast<uint32_t>(i), mapping)
          .Check();
    }
    global
        ->Set(context,
              v8::String::NewFromUtf8Literal(
                  isolate_, "__xenonRendererUrlMappings"),
              renderer_url_mappings)
        .Check();
    global
        ->Set(context,
              v8::String::NewFromUtf8Literal(isolate_, "__xenonRendererBaseUrl"),
              v8::String::NewFromUtf8(isolate_, renderer_base_url_.c_str())
                  .ToLocalChecked())
        .Check();
    global
        ->Set(
            context,
            v8::String::NewFromUtf8Literal(isolate_, "__xenonChromeVersion"),
            v8::String::NewFromUtf8(
                isolate_, std::string(version_info::GetVersionNumber()).c_str())
                .ToLocalChecked())
        .Check();
    global
        ->Set(context,
              v8::String::NewFromUtf8Literal(isolate_, "__xenonV8Version"),
              v8::String::NewFromUtf8(isolate_, v8::V8::GetVersion())
                  .ToLocalChecked())
        .Check();
    v8::Local<v8::Object> environment_object = v8::Object::New(isolate_);
    std::unique_ptr<base::Environment> environment =
        base::Environment::Create();
    for (const char* name : {"APPDATA",
                             "LOCALAPPDATA",
                             "USERPROFILE",
                             "HOME",
                             "TEMP",
                             "TMP",
                             "COMPUTERNAME",
                             "PATH",
                             "NODE_ENV",
                             "SystemRoot",
                             "windir",
                             "ProgramFiles",
                             "ProgramFiles(x86)",
                             "CommonProgramFiles",
                             "CommonProgramFiles(x86)",
                             "ProgramData",
                             "ALLUSERSPROFILE",
                             "PUBLIC",
                             "USER",
                             "LOGNAME",
                             "LANG",
                             "SHELL"}) {
      const std::string variable_name(name);
      if (std::optional<std::string> value =
              environment->GetVar(variable_name)) {
        environment_object
            ->Set(context,
                  v8::String::NewFromUtf8(isolate_, name).ToLocalChecked(),
                  v8::String::NewFromUtf8(isolate_, value->c_str())
                      .ToLocalChecked())
            .Check();
      }
    }
    global
        ->Set(context, v8::String::NewFromUtf8Literal(isolate_, "__xenonEnv"),
              environment_object)
        .Check();
    auto bind_func = [&](const char* name, auto method) {
      global
          ->Set(
              context, v8::String::NewFromUtf8(isolate_, name).ToLocalChecked(),
              gin::CreateFunctionTemplate(
                  isolate_, base::BindRepeating(method, base::Unretained(this)))
                  ->GetFunction(context)
                  .ToLocalChecked())
          .Check();
    };
    bind_func("__xenonLog", &XenonIpcMainContainer::NativeLog);
    bind_func("__xenonGetPath", &XenonIpcMainContainer::NativeGetPath);
    bind_func("__xenonNetworkInterfaces",
              &XenonIpcMainContainer::NativeNetworkInterfaces);
    bind_func("__xenonSendToRenderer",
              &XenonIpcMainContainer::NativeSendToRenderer);
    bind_func("__xenonFsExists", &XenonIpcMainContainer::NativeFsExists);
    bind_func("__xenonFsReadFile", &XenonIpcMainContainer::NativeFsReadFile);
    bind_func("__xenonFsWriteFile", &XenonIpcMainContainer::NativeFsWriteFile);
    bind_func("__xenonFsStat", &XenonIpcMainContainer::NativeFsStat);
    bind_func("__xenonFsReaddir", &XenonIpcMainContainer::NativeFsReaddir);
    bind_func("__xenonFsMkdir", &XenonIpcMainContainer::NativeFsMkdir);
    bind_func("__xenonFsUnlink", &XenonIpcMainContainer::NativeFsUnlink);
    bind_func("__xenonCryptoCipher",
              &XenonIpcMainContainer::NativeCryptoCipher);
    bind_func("__xenonShowOpenDialog",
              &XenonIpcMainContainer::NativeShowOpenDialog);
    bind_func("__xenonCreateBrowserWindow",
              &XenonIpcMainContainer::NativeCreateBrowserWindow);
    bind_func("__xenonLoadBrowserWindowURL",
              &XenonIpcMainContainer::NativeLoadBrowserWindowURL);
    bind_func("__xenonSetBrowserWindowVisible",
              &XenonIpcMainContainer::NativeSetBrowserWindowVisible);
    bind_func("__xenonBrowserWindowCall",
              &XenonIpcMainContainer::NativeBrowserWindowCall);
    bind_func("__xenonCloseBrowserWindow",
              &XenonIpcMainContainer::NativeCloseBrowserWindow);
    bind_func("__xenonNativeInvokeExport",
              &XenonIpcMainContainer::NativeInvokeExport);
    bind_func("__xenonNativeConstructExport",
              &XenonIpcMainContainer::NativeConstructExport);
    bind_func("__xenonNativeInvokeInstance",
              &XenonIpcMainContainer::NativeInvokeInstance);
    global
        ->Set(context, v8::String::NewFromUtf8Literal(isolate_, "setTimeout"),
              v8::Function::New(
                  context, &XenonIpcMainContainer::NativeSetTimeoutCallback,
                  v8::External::New(isolate_, this,
                                    v8::kExternalPointerTypeTagDefault))
                  .ToLocalChecked())
        .Check();
    global
        ->Set(context, v8::String::NewFromUtf8Literal(isolate_, "clearTimeout"),
              v8::Function::New(
                  context, &XenonIpcMainContainer::NativeClearTimerCallback,
                  v8::External::New(isolate_, this,
                                    v8::kExternalPointerTypeTagDefault))
                  .ToLocalChecked())
        .Check();
    global
        ->Set(context, v8::String::NewFromUtf8Literal(isolate_, "setInterval"),
              v8::Function::New(
                  context, &XenonIpcMainContainer::NativeSetIntervalCallback,
                  v8::External::New(isolate_, this,
                                    v8::kExternalPointerTypeTagDefault))
                  .ToLocalChecked())
        .Check();
    global
        ->Set(context,
              v8::String::NewFromUtf8Literal(isolate_, "clearInterval"),
              v8::Function::New(
                  context, &XenonIpcMainContainer::NativeClearTimerCallback,
                  v8::External::New(isolate_, this,
                                    v8::kExternalPointerTypeTagDefault))
                  .ToLocalChecked())
        .Check();
  }

  if (!RunBootstrap()) {
    return false;
  }

  if (!MaybeLoadConfiguredMainScript()) {
    return false;
  }
  initialized_ = true;
  LOG(INFO) << "Xenon ipcMain container initialized in Utility process";
  return true;
}

void XenonIpcMainContainer::Shutdown() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (shutting_down_) {
    return;
  }
  shutting_down_ = true;

  if (initialized_ && !shutdown_app_.IsEmpty()) {
    ScopedV8Context scope(isolate_, context_);
    gin::TryCatch try_catch(isolate_);
    if (shutdown_app_.Get(isolate_)
            ->Call(scope.context(), scope.context()->Global(), 0, nullptr)
            .IsEmpty()) {
      LOG(ERROR) << "Electron app shutdown listener failed: "
                 << try_catch.GetStackTrace();
    }
    isolate_->PerformMicrotaskCheckpoint();
  }

  weak_factory_.InvalidateWeakPtrs();
  FailAllPendingPromises("The Utility main JavaScript container stopped");
  renderers_.clear();
  if (!context_.IsEmpty()) {
    ScopedV8Context scope(isolate_, context_);
    timers_.clear();
    module_cache_.clear();
    loaded_node_addons_.clear();
  } else {
    timers_.clear();
    module_cache_.clear();
    loaded_node_addons_.clear();
  }
  dispatch_send_.Reset();
  dispatch_invoke_.Reset();
  dispatch_sync_.Reset();
  dispatch_window_event_.Reset();
  dispatch_renderer_event_.Reset();
  mark_app_ready_.Reset();
  shutdown_app_.Reset();
  context_.Reset();
  isolate_ = nullptr;
  isolate_holder_.reset();
  initialized_ = false;
}

bool XenonIpcMainContainer::RunBootstrap() {
  ScopedV8Context scope(isolate_, context_);
  v8::Local<v8::Context> context = scope.context();
  gin::TryCatch try_catch(isolate_);
  const std::string bootstrap_source = GetMainBootstrapSource();
  if (bootstrap_source.empty()) {
    startup_error_ = "Failed to load Electron IPC main bootstrap resource";
    LOG(ERROR) << startup_error_;
    return false;
  }
  v8::Local<v8::String> source =
      v8::String::NewFromUtf8(isolate_, bootstrap_source.c_str(),
                              v8::NewStringType::kNormal,
                              static_cast<int>(bootstrap_source.length()))
          .ToLocalChecked();
  v8::ScriptOrigin origin(
      v8::String::NewFromUtf8Literal(isolate_, "xenon_ipc_main_bootstrap.js"));
  v8::Local<v8::Script> script;
  if (!v8::Script::Compile(context, source, &origin).ToLocal(&script) ||
      script->Run(context).IsEmpty()) {
    startup_error_ = "Failed to initialize Electron IPC bootstrap: " +
                     FormatCaughtError(isolate_, try_catch);
    LOG(ERROR) << startup_error_;
    return false;
  }

  auto capture_function = [&](const char* name,
                              v8::Global<v8::Function>* target) -> bool {
    v8::Local<v8::Value> value;
    if (!context->Global()
             ->Get(context,
                   v8::String::NewFromUtf8(isolate_, name).ToLocalChecked())
             .ToLocal(&value) ||
        !value->IsFunction()) {
      startup_error_ = std::string("IPC bootstrap did not define ") + name;
      return false;
    }
    target->Reset(isolate_, value.As<v8::Function>());
    return true;
  };
  return capture_function("__xenonDispatchSend", &dispatch_send_) &&
         capture_function("__xenonDispatchInvoke", &dispatch_invoke_) &&
         capture_function("__xenonDispatchSync", &dispatch_sync_) &&
         capture_function("__xenonDispatchBrowserWindowEvent",
                          &dispatch_window_event_) &&
         capture_function("__xenonDispatchRendererEvent",
                          &dispatch_renderer_event_) &&
         capture_function("__xenonMarkAppReady", &mark_app_ready_) &&
         capture_function("__xenonShutdownApp", &shutdown_app_);
}

bool XenonIpcMainContainer::ResolveConfiguredMainScript() {
  const base::CommandLine* command_line =
      base::CommandLine::ForCurrentProcess();
  base::PathService::Get(base::DIR_EXE, &app_path_);

  if (command_line->HasSwitch(switches::kMainScript)) {
    main_script_path_ =
        command_line->GetSwitchValuePath(switches::kMainScript);
    if (main_script_path_.empty() || !main_script_path_.IsAbsolute()) {
      startup_error_ = "--xenon-main-js requires an absolute JavaScript path";
      return false;
    }
    app_path_ = main_script_path_.DirName();
    module_root_ = app_path_;
  } else if (command_line->HasSwitch(switches::kElectronApp)) {
    app_path_ = command_line->GetSwitchValuePath(switches::kElectronApp);
    base::FilePath normalized_app_path;
    if (app_path_.empty() || !base::DirectoryExists(app_path_) ||
        !base::NormalizeFilePath(app_path_, &normalized_app_path)) {
      startup_error_ =
          "--xenon-electron-app requires an existing application directory";
      return false;
    }
    app_path_ = normalized_app_path;
    module_root_ = app_path_;

    const base::FilePath package_path = app_path_.AppendASCII("package.json");
    std::string package_text;
    if (!base::ReadFileToString(package_path, &package_text)) {
      startup_error_ = "Electron application has no readable package.json: " +
                       package_path.AsUTF8Unsafe();
      return false;
    }
    std::optional<base::Value> package =
        base::JSONReader::Read(package_text, base::JSON_PARSE_RFC);
    if (!package || !package->is_dict()) {
      startup_error_ = "Electron application package.json is invalid";
      return false;
    }
    if (const std::string* name = package->GetDict().FindString("name")) {
      app_name_ = *name;
    }
    if (const std::string* version = package->GetDict().FindString("version")) {
      app_version_ = *version;
    }
    const std::string* main = package->GetDict().FindString("main");
    main_script_path_ = app_path_.Append(
        base::FilePath::FromUTF8Unsafe(main ? *main : "main.js"));
  }

  if (main_script_path_.empty()) {
    return true;
  }

  base::FilePath normalized_main_script;
  if (!base::NormalizeFilePath(main_script_path_, &normalized_main_script)) {
    startup_error_ = "Configured main JavaScript does not exist: " +
                     main_script_path_.AsUTF8Unsafe();
    return false;
  }
  main_script_path_ = normalized_main_script;
  if (command_line->HasSwitch(switches::kMainScript)) {
    app_path_ = main_script_path_.DirName();
    module_root_ = app_path_;
  }
  if (module_root_.empty()) {
    module_root_ = main_script_path_.DirName();
  }
  if (main_script_path_ != module_root_ &&
      !module_root_.IsParent(main_script_path_)) {
    startup_error_ =
        "Configured main JavaScript is outside its application "
        "directory: " +
        main_script_path_.AsUTF8Unsafe();
    return false;
  }
  return true;
}

bool XenonIpcMainContainer::MaybeLoadConfiguredMainScript() {
  if (main_script_path_.empty()) {
    return true;
  }

  ScopedV8Context scope(isolate_, context_);
  std::string error;
  v8::Local<v8::Value> ignored;
  v8::MaybeLocal<v8::Value> loaded =
      embedded_main_source_
          ? LoadCommonJsSource(main_script_path_, *embedded_main_source_,
                               &error)
          : LoadCommonJsModule(main_script_path_, &error);
  if (!loaded.ToLocal(&ignored)) {
    startup_error_ = "Failed to load main JavaScript: " + error;
    LOG(ERROR) << startup_error_;
    return false;
  }
  return true;
}

void XenonIpcMainContainer::MarkAppReady() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!initialized_ || mark_app_ready_.IsEmpty()) {
    return;
  }
  ScopedV8Context scope(isolate_, context_);
  gin::TryCatch try_catch(isolate_);
  if (mark_app_ready_.Get(isolate_)
          ->Call(scope.context(), scope.context()->Global(), 0, nullptr)
          .IsEmpty()) {
    LOG(ERROR) << "Electron app ready listener failed: "
               << try_catch.GetStackTrace();
  }
  isolate_->PerformMicrotaskCheckpoint();
  if (try_catch.HasCaught()) {
    LOG(ERROR) << "Electron app ready microtask failed: "
               << try_catch.GetStackTrace();
  }
}

v8::MaybeLocal<v8::Value> XenonIpcMainContainer::LoadCommonJsModule(
    const base::FilePath& requested_path,
    std::string* error) {
  std::optional<base::FilePath> resolved =
      ResolveCommonJsPath(requested_path, error);
  if (!resolved) {
    return {};
  }
  const base::FilePath& normalized = *resolved;
  if (!module_root_.empty() && normalized != module_root_ &&
      !module_root_.IsParent(normalized) &&
      !IsNativeAddonBesideExecutable(normalized, executable_path_)) {
    *error = "Module is outside the configured main JavaScript directory: " +
             normalized.AsUTF8Unsafe();
    return {};
  }

  const std::string cache_key = normalized.AsUTF8Unsafe();
  auto cached = module_cache_.find(cache_key);
  if (cached != module_cache_.end()) {
    return cached->second.Get(isolate_);
  }

  if (normalized.MatchesExtension(FILE_PATH_LITERAL(".node"))) {
    if (native_addon_hooks_.load) {
      std::string load_error;
      bool loaded = false;
      {
        v8::Unlocker unlocker(isolate_);
        loaded = native_addon_hooks_.load.Run(normalized.AsUTF8Unsafe(),
                                              &load_error);
      }
      if (!loaded) {
        *error = load_error.empty()
                     ? "Failed to load Node-API addon: " + cache_key
                     : load_error;
        return {};
      }
      v8::Local<v8::Value> forwarder;
      if (!CreateNativeAddonForwarder(normalized.AsUTF8Unsafe())
               .ToLocal(&forwarder)) {
        *error = "Failed to create native addon forwarder: " + cache_key;
        return {};
      }
      module_cache_.emplace(cache_key,
                            v8::Global<v8::Value>(isolate_, forwarder));
      return forwarder;
    }
    auto loaded_addon = std::make_unique<xenon::LoadedNodeAddon>();
    v8::Local<v8::Value> exports = xenon::LoadAndInitializeNodeAddon(
        isolate_, context_.Get(isolate_), normalized, loaded_addon.get());
    if (exports.IsEmpty()) {
      *error = "Failed to load Node-API addon: " + cache_key;
      return {};
    }
    module_cache_.emplace(cache_key, v8::Global<v8::Value>(isolate_, exports));
    loaded_node_addons_.emplace(cache_key, std::move(loaded_addon));
    return exports;
  }

  std::string source_text;
  if (!base::ReadFileToString(normalized, &source_text)) {
    *error = "Failed to read module: " + cache_key;
    return {};
  }

  if (normalized.MatchesExtension(FILE_PATH_LITERAL(".json"))) {
    std::optional<base::Value> json =
        base::JSONReader::Read(source_text, base::JSON_PARSE_RFC);
    if (!json) {
      *error = "Invalid JSON module: " + cache_key;
      return {};
    }
    v8::Local<v8::Value> exports;
    if (!ValueToV8(*json).ToLocal(&exports)) {
      *error = "Failed to decode JSON module: " + cache_key;
      return {};
    }
    module_cache_.emplace(cache_key, v8::Global<v8::Value>(isolate_, exports));
    return exports;
  }

  return LoadCommonJsSource(normalized, source_text, error);
}

v8::MaybeLocal<v8::Value> XenonIpcMainContainer::LoadCommonJsSource(
    const base::FilePath& virtual_path,
    const std::string& source_text,
    std::string* error) {
  const std::string cache_key = virtual_path.AsUTF8Unsafe();
  auto cached = module_cache_.find(cache_key);
  if (cached != module_cache_.end()) {
    return cached->second.Get(isolate_);
  }

  v8::Local<v8::Context> context = context_.Get(isolate_);
  std::string wrapped =
      "(function(exports, require, module, __filename, __dirname) {\n" +
      source_text + "\n})";
  gin::TryCatch try_catch(isolate_);
  v8::Local<v8::String> source =
      v8::String::NewFromUtf8(isolate_, wrapped.c_str()).ToLocalChecked();
  // Resource name must be set so Error.stack CallSite#getFileName() works.
  // Node's `bindings` package crashes on undefined.getFileName() with
  // `Cannot read properties of undefined (reading 'indexOf')`.
  const std::string resource_name = virtual_path.AsUTF8Unsafe();
  v8::ScriptOrigin origin(
      v8::String::NewFromUtf8(isolate_, resource_name.c_str()).ToLocalChecked());
  v8::Local<v8::Script> script;
  v8::Local<v8::Value> wrapper_value;
  if (!v8::Script::Compile(context, source, &origin).ToLocal(&script) ||
      !script->Run(context).ToLocal(&wrapper_value) ||
      !wrapper_value->IsFunction()) {
    *error = FormatCaughtError(isolate_, try_catch);
    return {};
  }

  v8::Local<v8::Object> exports = v8::Object::New(isolate_);
  v8::Local<v8::Object> module = v8::Object::New(isolate_);
  module
      ->Set(context, v8::String::NewFromUtf8Literal(isolate_, "exports"),
            exports)
      .Check();
  module_cache_.emplace(cache_key, v8::Global<v8::Value>(isolate_, exports));

  const std::string parent_file = virtual_path.AsUTF8Unsafe();
  v8::Local<v8::Function> require_function =
      v8::Function::New(
          context,
          [](const v8::FunctionCallbackInfo<v8::Value>& info) {
            v8::Local<v8::Array> data = info.Data().As<v8::Array>();
            v8::Local<v8::Context> context =
                info.GetIsolate()->GetCurrentContext();
            v8::Local<v8::Value> host_value;
            v8::Local<v8::Value> parent_value;
            if (!data->Get(context, 0).ToLocal(&host_value) ||
                !data->Get(context, 1).ToLocal(&parent_value) ||
                !host_value->IsExternal() || info.Length() < 1) {
              return;
            }
            auto* host = static_cast<XenonIpcMainContainer*>(
                host_value.As<v8::External>()->Value(
                    v8::kExternalPointerTypeTagDefault));
            v8::String::Utf8Value request(info.GetIsolate(), info[0]);
            v8::String::Utf8Value parent(info.GetIsolate(), parent_value);
            std::string require_error;
            v8::Local<v8::Value> result;
            if (*request == nullptr || *parent == nullptr ||
                !host->RequireModule(*request,
                                     base::FilePath::FromUTF8Unsafe(*parent),
                                     &require_error)
                     .ToLocal(&result)) {
              // Match Node's MODULE_NOT_FOUND shape so packages like
              // `bindings` can try the next candidate path.
              v8::Isolate* isolate = info.GetIsolate();
              v8::Local<v8::Context> current = isolate->GetCurrentContext();
              const std::string message =
                  require_error.empty()
                      ? (std::string("Cannot find module '") +
                         (*request ? *request : "<null>") + "'")
                      : require_error;
              v8::Local<v8::Value> exception = v8::Exception::Error(
                  v8::String::NewFromUtf8(isolate, message.c_str())
                      .ToLocalChecked());
              if (exception->IsObject()) {
                exception.As<v8::Object>()
                    ->Set(current,
                          v8::String::NewFromUtf8Literal(isolate, "code"),
                          v8::String::NewFromUtf8Literal(isolate,
                                                         "MODULE_NOT_FOUND"))
                    .Check();
              }
              isolate->ThrowException(exception);
              return;
            }
            info.GetReturnValue().Set(result);
          },
          [&]() {
            v8::Local<v8::Array> data = v8::Array::New(isolate_, 2);
            data->Set(context, 0,
                      v8::External::New(isolate_, this,
                                        v8::kExternalPointerTypeTagDefault))
                .Check();
            data->Set(context, 1,
                      v8::String::NewFromUtf8(isolate_, parent_file.c_str())
                          .ToLocalChecked())
                .Check();
            return data;
          }())
          .ToLocalChecked();

  v8::Local<v8::Value> argv[] = {
      exports,
      require_function,
      module,
      v8::String::NewFromUtf8(isolate_, virtual_path.AsUTF8Unsafe().c_str())
          .ToLocalChecked(),
      v8::String::NewFromUtf8(isolate_,
                              virtual_path.DirName().AsUTF8Unsafe().c_str())
          .ToLocalChecked(),
  };
  if (wrapper_value.As<v8::Function>()
          ->Call(context, exports, std::size(argv), argv)
          .IsEmpty()) {
    module_cache_.erase(cache_key);
    *error = FormatCaughtError(isolate_, try_catch);
    return {};
  }

  v8::Local<v8::Value> final_exports;
  if (!module->Get(context, v8::String::NewFromUtf8Literal(isolate_, "exports"))
           .ToLocal(&final_exports)) {
    module_cache_.erase(cache_key);
    *error = "Failed to read module.exports";
    return {};
  }
  module_cache_[cache_key].Reset(isolate_, final_exports);
  return final_exports;
}

std::optional<base::FilePath> XenonIpcMainContainer::ResolveCommonJsPath(
    const base::FilePath& requested_path,
    std::string* error,
    int package_main_depth) {
  if (package_main_depth > kMaxPackageMainDepth) {
    *error = "package.json main resolution is too deeply nested: " +
             requested_path.AsUTF8Unsafe();
    return std::nullopt;
  }

  auto normalize_file =
      [](const base::FilePath& candidate) -> std::optional<base::FilePath> {
    if (!base::PathExists(candidate) || base::DirectoryExists(candidate)) {
      return std::nullopt;
    }
    base::FilePath normalized;
    if (!base::NormalizeFilePath(candidate, &normalized)) {
      return std::nullopt;
    }
    return normalized;
  };

  if (std::optional<base::FilePath> exact = normalize_file(requested_path)) {
    return exact;
  }
  if (requested_path.MatchesExtension(FILE_PATH_LITERAL(".node")) &&
      !module_root_.empty()) {
    for (const base::FilePath& candidate : {
             module_root_.Append(requested_path.BaseName()),
             module_root_.AppendASCII("build")
                 .AppendASCII("Release")
                 .Append(requested_path.BaseName()),
             module_root_.AppendASCII("Release")
                 .Append(requested_path.BaseName()),
         }) {
      if (std::optional<base::FilePath> found = normalize_file(candidate)) {
        return found;
      }
    }
  }
  if (requested_path.Extension().empty()) {
    for (const base::FilePath::CharType* extension :
         {FILE_PATH_LITERAL("js"), FILE_PATH_LITERAL("json"),
          FILE_PATH_LITERAL("cjs"), FILE_PATH_LITERAL("node")}) {
      if (std::optional<base::FilePath> with_extension =
              normalize_file(requested_path.AddExtension(extension))) {
        return with_extension;
      }
    }
  }

  if (base::DirectoryExists(requested_path)) {
    const base::FilePath package_path =
        requested_path.AppendASCII("package.json");
    std::string package_text;
    if (base::ReadFileToString(package_path, &package_text)) {
      std::optional<base::Value> package =
          base::JSONReader::Read(package_text, base::JSON_PARSE_RFC);
      if (!package || !package->is_dict()) {
        *error = "Invalid package.json: " + package_path.AsUTF8Unsafe();
        return std::nullopt;
      }
      if (const std::string* main = package->GetDict().FindString("main")) {
        const base::FilePath main_path =
            requested_path.Append(base::FilePath::FromUTF8Unsafe(*main));
        base::FilePath normalized_directory;
        base::FilePath normalized_main;
        const bool points_to_same_directory =
            base::NormalizeFilePath(requested_path, &normalized_directory) &&
            base::NormalizeFilePath(main_path, &normalized_main) &&
            normalized_directory == normalized_main;
        if (!points_to_same_directory) {
          std::string main_error;
          if (std::optional<base::FilePath> resolved = ResolveCommonJsPath(
                  main_path, &main_error, package_main_depth + 1)) {
            return resolved;
          }
        }
      }
    }
    for (const char* index_file : {"index.js", "index.json", "index.cjs"}) {
      if (std::optional<base::FilePath> index =
              normalize_file(requested_path.AppendASCII(index_file))) {
        return index;
      }
    }
  }

  *error = "Cannot find module '" + requested_path.AsUTF8Unsafe() + "'";
  return std::nullopt;
}

v8::MaybeLocal<v8::Value> XenonIpcMainContainer::RequireModule(
    const std::string& request,
    const base::FilePath& parent_file,
    std::string* error) {
  v8::Local<v8::Context> context = context_.Get(isolate_);

  static constexpr auto kBuiltinModules =
      base::MakeFixedFlatMap<std::string_view, const char*>({
          {"assert", "__xenonAssert"},
          {"async_hooks", "__xenonAsyncHooks"},
          {"buffer", "__xenonBuffer"},
          {"child_process", "__xenonChildProcess"},
          {"constants", "__xenonConstants"},
          {"crypto", "__xenonCrypto"},
          {"dns", "__xenonDns"},
          {"electron", "__xenonElectron"},
          {"events", "__xenonEvents"},
          {"fs", "__xenonFs"},
          {"fs/promises", "__xenonFsPromises"},
          {"http", "__xenonHttp"},
          {"http2", "__xenonHttp"},
          {"https", "__xenonHttps"},
          {"net", "__xenonNet"},
          {"node:assert", "__xenonAssert"},
          {"node:async_hooks", "__xenonAsyncHooks"},
          {"node:buffer", "__xenonBuffer"},
          {"node:child_process", "__xenonChildProcess"},
          {"node:constants", "__xenonConstants"},
          {"node:crypto", "__xenonCrypto"},
          {"node:dns", "__xenonDns"},
          {"node:electron", "__xenonElectron"},
          {"node:events", "__xenonEvents"},
          {"node:fs", "__xenonFs"},
          {"node:fs/promises", "__xenonFsPromises"},
          {"node:http", "__xenonHttp"},
          {"node:http2", "__xenonHttp"},
          {"node:https", "__xenonHttps"},
          {"node:net", "__xenonNet"},
          {"node:os", "__xenonOs"},
          {"node:path", "__xenonPath"},
          {"node:path/posix", "__xenonPathPosix"},
          {"node:path/win32", "__xenonPathWin32"},
          {"node:perf_hooks", "__xenonPerfHooks"},
          {"node:querystring", "__xenonQuerystring"},
          {"node:readline", "__xenonReadline"},
          {"node:stream", "__xenonStream"},
          {"node:string_decoder", "__xenonStringDecoder"},
          {"node:timers", "__xenonTimers"},
          {"node:tls", "__xenonNet"},
          {"node:tty", "__xenonTty"},
          {"node:url", "__xenonUrl"},
          {"node:util", "__xenonUtil"},
          {"node:zlib", "__xenonZlib"},
          {"os", "__xenonOs"},
          {"path", "__xenonPath"},
          {"path/posix", "__xenonPathPosix"},
          {"path/win32", "__xenonPathWin32"},
          {"perf_hooks", "__xenonPerfHooks"},
          {"querystring", "__xenonQuerystring"},
          {"readline", "__xenonReadline"},
          {"stream", "__xenonStream"},
          {"string_decoder", "__xenonStringDecoder"},
          {"timers", "__xenonTimers"},
          {"tls", "__xenonNet"},
          {"tty", "__xenonTty"},
          {"url", "__xenonUrl"},
          {"util", "__xenonUtil"},
          {"zlib", "__xenonZlib"},
      });

  auto it = kBuiltinModules.find(request);
  if (it != kBuiltinModules.end()) {
    return context->Global()->Get(
        context,
        v8::String::NewFromUtf8(isolate_, it->second).ToLocalChecked());
  }

  if (request.starts_with("./") || request.starts_with("../") ||
      base::FilePath::FromUTF8Unsafe(request).IsAbsolute()) {
    base::FilePath path = base::FilePath::FromUTF8Unsafe(request);
    if (!path.IsAbsolute()) {
      base::FilePath parent_dir = base::DirectoryExists(parent_file)
                                      ? parent_file
                                      : parent_file.DirName();
      path = parent_dir.Append(path);
    }
    return LoadCommonJsModule(path, error);
  }

  const base::FilePath request_path = base::FilePath::FromUTF8Unsafe(request);
  if (!request_path.empty() && !request_path.ReferencesParent()) {
    base::FilePath start_dir = base::DirectoryExists(parent_file)
                                   ? parent_file
                                   : parent_file.DirName();
    for (base::FilePath directory = start_dir;;) {
      if (directory != module_root_ && !module_root_.IsParent(directory)) {
        break;
      }
      const base::FilePath candidate =
          directory.AppendASCII("node_modules").Append(request_path);
      std::string resolve_error;
      if (ResolveCommonJsPath(candidate, &resolve_error)) {
        return LoadCommonJsModule(candidate, error);
      }
      if (directory == module_root_) {
        break;
      }
      const base::FilePath parent = directory.DirName();
      if (parent == directory) {
        break;
      }
      directory = parent;
    }
  }
  *error = "Unsupported Node module in Browser main container: " + request;
  return {};
}

void XenonIpcMainContainer::NativeLog(gin::Arguments* args) {
  std::string message;
  if (!args->GetNext(&message)) {
    return;
  }
  LOG(ERROR) << "ipcMain: " << message;
}

void XenonIpcMainContainer::NativeGetPath(gin::Arguments* args) {
  std::string name;
  if (!args->GetNext(&name)) {
    args->ThrowTypeError("getPath expects a name string");
    return;
  }
  base::FilePath path;
  if (name == "exe") {
    path = executable_path_;
  } else if (name == "module") {
    base::PathService::Get(base::DIR_MODULE, &path);
  } else if (name == "home") {
    base::PathService::Get(base::DIR_HOME, &path);
  } else if (name == "appData") {
    std::unique_ptr<base::Environment> env = base::Environment::Create();
    if (std::optional<std::string> app_data = env->GetVar("APPDATA")) {
      path = base::FilePath::FromUTF8Unsafe(*app_data);
    } else {
      base::PathService::Get(base::DIR_HOME, &path);
    }
  } else if (name == "userData") {
    if (!base::PathService::Get(chrome::DIR_USER_DATA, &path) || path.empty()) {
      std::unique_ptr<base::Environment> env = base::Environment::Create();
      if (std::optional<std::string> app_data = env->GetVar("APPDATA")) {
        path = base::FilePath::FromUTF8Unsafe(*app_data).Append(
            base::FilePath::FromUTF8Unsafe(app_name_));
      } else {
        path = app_path_;
      }
    }
  } else if (name == "temp") {
    base::PathService::Get(base::DIR_TEMP, &path);
  } else if (name == "desktop") {
    base::PathService::Get(base::DIR_USER_DESKTOP, &path);
  } else if (name == "documents") {
    base::PathService::Get(chrome::DIR_USER_DOCUMENTS, &path);
  } else if (name == "downloads") {
    base::PathService::Get(chrome::DIR_DEFAULT_DOWNLOADS, &path);
  } else if (name == "music") {
    base::PathService::Get(chrome::DIR_USER_MUSIC, &path);
  } else if (name == "pictures") {
    base::PathService::Get(chrome::DIR_USER_PICTURES, &path);
  } else if (name == "videos") {
    base::PathService::Get(chrome::DIR_USER_VIDEOS, &path);
  } else if (name == "app" || name == "appPath") {
    path = app_path_;
  } else {
    path = app_path_;
  }
  args->GetFunctionCallbackInfo()->GetReturnValue().Set(
      gin::StringToV8(args->isolate(), path.AsUTF8Unsafe()));
}

void XenonIpcMainContainer::NativeNetworkInterfaces(gin::Arguments* args) {
  auto result = GetNetworkInterfaces();
  if (!result->success) {
    args->ThrowTypeError(result->error);
    return;
  }
  v8::Local<v8::Value> value;
  if (ValueToV8(result->value).ToLocal(&value)) {
    args->Return(value);
  }
}

void XenonIpcMainContainer::NativeFsExists(gin::Arguments* args) {
  std::string path_str;
  if (!args->GetNext(&path_str)) {
    args->GetFunctionCallbackInfo()->GetReturnValue().Set(false);
    return;
  }
  const base::FilePath path = base::FilePath::FromUTF8Unsafe(path_str);
  args->GetFunctionCallbackInfo()->GetReturnValue().Set(base::PathExists(path));
}

void XenonIpcMainContainer::NativeFsReadFile(gin::Arguments* args) {
  std::string path_str;
  if (!args->GetNext(&path_str)) {
    args->ThrowTypeError("readFile expects a path string");
    return;
  }
  const base::FilePath path = base::FilePath::FromUTF8Unsafe(path_str);
  std::string contents;
  if (!base::ReadFileToString(path, &contents)) {
    args->GetFunctionCallbackInfo()->GetReturnValue().SetUndefined();
    return;
  }
  v8::Isolate* isolate = args->isolate();
  args->GetFunctionCallbackInfo()->GetReturnValue().Set(
      v8::String::NewFromUtf8(isolate, contents.data(),
                              v8::NewStringType::kNormal,
                              static_cast<int>(contents.size()))
          .ToLocalChecked());
}

void XenonIpcMainContainer::NativeFsWriteFile(gin::Arguments* args) {
  std::string path_str;
  std::string data;
  if (!args->GetNext(&path_str) || !args->GetNext(&data)) {
    args->ThrowTypeError("writeFile expects path and data strings");
    return;
  }
  const base::FilePath path = base::FilePath::FromUTF8Unsafe(path_str);
  base::CreateDirectory(path.DirName());
  bool ok = base::WriteFile(path, data);
  args->GetFunctionCallbackInfo()->GetReturnValue().Set(ok);
}

void XenonIpcMainContainer::NativeFsStat(gin::Arguments* args) {
  std::string path_str;
  if (!args->GetNext(&path_str)) {
    args->ThrowTypeError("stat expects a path string");
    return;
  }
  const base::FilePath path = base::FilePath::FromUTF8Unsafe(path_str);
  base::File::Info info;
  v8::Isolate* isolate = args->isolate();
  v8::Local<v8::Context> context = isolate->GetCurrentContext();
  v8::Local<v8::Object> stat_obj = v8::Object::New(isolate);
  if (!base::GetFileInfo(path, &info)) {
    stat_obj
        ->Set(context, v8::String::NewFromUtf8Literal(isolate, "exists"),
              v8::Boolean::New(isolate, false))
        .Check();
  } else {
    stat_obj
        ->Set(context, v8::String::NewFromUtf8Literal(isolate, "exists"),
              v8::Boolean::New(isolate, true))
        .Check();
    stat_obj
        ->Set(context, v8::String::NewFromUtf8Literal(isolate, "isDirectory"),
              v8::Boolean::New(isolate, info.is_directory))
        .Check();
    stat_obj
        ->Set(context, v8::String::NewFromUtf8Literal(isolate, "isFile"),
              v8::Boolean::New(isolate, !info.is_directory))
        .Check();
    stat_obj
        ->Set(context, v8::String::NewFromUtf8Literal(isolate, "size"),
              v8::Number::New(isolate, static_cast<double>(info.size)))
        .Check();
    stat_obj
        ->Set(context, v8::String::NewFromUtf8Literal(isolate, "mtimeMs"),
              v8::Number::New(
                  isolate, info.last_modified.InMillisecondsFSinceUnixEpoch()))
        .Check();
    stat_obj
        ->Set(context, v8::String::NewFromUtf8Literal(isolate, "birthtimeMs"),
              v8::Number::New(
                  isolate, info.creation_time.InMillisecondsFSinceUnixEpoch()))
        .Check();
  }
  args->GetFunctionCallbackInfo()->GetReturnValue().Set(stat_obj);
}

void XenonIpcMainContainer::NativeFsReaddir(gin::Arguments* args) {
  std::string path_str;
  if (!args->GetNext(&path_str)) {
    args->ThrowTypeError("readdir expects a path string");
    return;
  }
  const base::FilePath path = base::FilePath::FromUTF8Unsafe(path_str);
  v8::Isolate* isolate = args->isolate();
  v8::Local<v8::Context> context = isolate->GetCurrentContext();
  v8::Local<v8::Array> array = v8::Array::New(isolate);

  if (base::DirectoryExists(path)) {
    base::FileEnumerator enumerator(
        path, false,
        base::FileEnumerator::FILES | base::FileEnumerator::DIRECTORIES);
    uint32_t index = 0;
    for (base::FilePath name = enumerator.Next(); !name.empty();
         name = enumerator.Next()) {
      array
          ->Set(context, index++,
                gin::StringToV8(isolate, name.BaseName().AsUTF8Unsafe()))
          .Check();
    }
  }
  args->GetFunctionCallbackInfo()->GetReturnValue().Set(array);
}

void XenonIpcMainContainer::NativeFsMkdir(gin::Arguments* args) {
  std::string path_str;
  if (!args->GetNext(&path_str)) {
    args->ThrowTypeError("mkdir expects a path string");
    return;
  }
  const base::FilePath path = base::FilePath::FromUTF8Unsafe(path_str);
  bool ok = base::CreateDirectory(path);
  args->GetFunctionCallbackInfo()->GetReturnValue().Set(ok);
}

void XenonIpcMainContainer::NativeFsUnlink(gin::Arguments* args) {
  std::string path_str;
  if (!args->GetNext(&path_str)) {
    args->ThrowTypeError("unlink expects a path string");
    return;
  }
  const base::FilePath path = base::FilePath::FromUTF8Unsafe(path_str);
  bool ok = base::DeleteFile(path);
  args->GetFunctionCallbackInfo()->GetReturnValue().Set(ok);
}

void XenonIpcMainContainer::NativeCryptoCipher(gin::Arguments* args) {
  std::string algorithm;
  bool encrypt = false;
  std::string key_base64;
  std::string iv_base64;
  std::string input_base64;
  bool auto_padding = true;
  if (!args->GetNext(&algorithm) || !args->GetNext(&encrypt) ||
      !args->GetNext(&key_base64) || !args->GetNext(&iv_base64) ||
      !args->GetNext(&input_base64) || !args->GetNext(&auto_padding)) {
    args->ThrowTypeError(
        "crypto cipher expects algorithm, mode, key, iv, data, and padding");
    return;
  }

  const EVP_CIPHER* cipher = nullptr;
  bool uses_iv = false;
  if (algorithm == "aes-128-ecb") {
    cipher = EVP_aes_128_ecb();
  } else if (algorithm == "aes-192-ecb") {
    cipher = EVP_aes_192_ecb();
  } else if (algorithm == "aes-256-ecb") {
    cipher = EVP_aes_256_ecb();
  } else if (algorithm == "aes-128-cbc") {
    cipher = EVP_aes_128_cbc();
    uses_iv = true;
  } else if (algorithm == "aes-192-cbc") {
    cipher = EVP_aes_192_cbc();
    uses_iv = true;
  } else if (algorithm == "aes-256-cbc") {
    cipher = EVP_aes_256_cbc();
    uses_iv = true;
  } else {
    args->ThrowTypeError("Unsupported cipher algorithm: " + algorithm);
    return;
  }

  std::string key;
  std::string iv;
  std::string input;
  if (!base::Base64Decode(key_base64, &key) ||
      !base::Base64Decode(iv_base64, &iv) ||
      !base::Base64Decode(input_base64, &input)) {
    args->ThrowTypeError("Invalid base64 cipher input");
    return;
  }
  if (key.size() != static_cast<size_t>(EVP_CIPHER_key_length(cipher))) {
    args->ThrowTypeError("Invalid key length for " + algorithm);
    return;
  }
  if (uses_iv && iv.size() != static_cast<size_t>(EVP_CIPHER_iv_length(cipher))) {
    args->ThrowTypeError("Invalid initialization vector for " + algorithm);
    return;
  }

  crypto::OpenSSLErrStackTracer err_tracer(FROM_HERE);
  bssl::ScopedEVP_CIPHER_CTX context;
  const uint8_t* iv_data = uses_iv
                               ? reinterpret_cast<const uint8_t*>(iv.data())
                               : nullptr;
  const uint8_t* key_data = reinterpret_cast<const uint8_t*>(key.data());
  const uint8_t* input_data =
      reinterpret_cast<const uint8_t*>(input.data());
  if (!EVP_CipherInit_ex(context.get(), cipher, nullptr, key_data, iv_data,
                         encrypt ? 1 : 0) ||
      !EVP_CIPHER_CTX_set_padding(context.get(), auto_padding ? 1 : 0)) {
    args->ThrowTypeError("Failed to initialize " + algorithm);
    return;
  }

  const size_t block_size = EVP_CIPHER_block_size(cipher);
  std::vector<uint8_t> output(input.size() + block_size);
  size_t update_size = 0;
  if (!EVP_CipherUpdate_ex(context.get(), output.data(), &update_size,
                           output.size(), input_data, input.size())) {
    args->ThrowTypeError("Cipher update failed for " + algorithm);
    return;
  }
  size_t final_size = 0;
  auto remainder = base::span(output).subspan(update_size);
  if (!EVP_CipherFinal_ex2(context.get(), remainder.data(), &final_size,
                           remainder.size())) {
    args->ThrowTypeError("Cipher final failed for " + algorithm);
    return;
  }
  output.resize(update_size + final_size);
  const std::string_view output_view(
      reinterpret_cast<const char*>(output.data()), output.size());
  args->GetFunctionCallbackInfo()->GetReturnValue().Set(gin::StringToV8(
      args->isolate(), base::Base64Encode(output_view)));
}

xenon::ipc::mojom::IpcResultPtr XenonIpcMainContainer::FileSystemCall(
    base::Value arguments) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  if (!arguments.is_list() || arguments.GetList().empty() ||
      !arguments.GetList().front().is_dict()) {
    return ErrorResult("EINVAL: invalid argument, fs request");
  }
  const base::DictValue& request = arguments.GetList().front().GetDict();
  const std::string* operation = request.FindString("operation");
  const std::string* path_string = request.FindString("path");
  if (!operation || !path_string) {
    return ErrorResult("EINVAL: fs request requires operation and path");
  }

  const base::FilePath path = base::FilePath::FromUTF8Unsafe(*path_string);
  auto error = [&](const char* code, const char* description,
                   const char* syscall) {
    return ErrorResult(std::string(code) + ": " + description + ", " +
                       syscall + " '" + *path_string + "'");
  };
  auto success = [](base::Value value = base::Value()) {
    auto result = xenon::ipc::mojom::IpcResult::New();
    result->success = true;
    result->value = std::move(value);
    return result;
  };

  if (*operation == "exists") {
    return success(base::Value(base::PathExists(path)));
  }

  if (*operation == "stat" || *operation == "lstat") {
    base::File::Info info;
    if (!base::GetFileInfo(path, &info)) {
      return error("ENOENT", "no such file or directory", "stat");
    }
    base::DictValue stat;
    stat.Set("isFile", !info.is_directory);
    stat.Set("isDirectory", info.is_directory);
    stat.Set("isSymbolicLink", false);
    stat.Set("size", static_cast<double>(info.size));
    stat.Set("mtimeMs", info.last_modified.InMillisecondsFSinceUnixEpoch());
    stat.Set("birthtimeMs", info.creation_time.InMillisecondsFSinceUnixEpoch());
    return success(base::Value(std::move(stat)));
  }

  if (*operation == "read_file") {
    if (base::DirectoryExists(path)) {
      return error("EISDIR", "illegal operation on a directory", "read");
    }
    std::string contents;
    if (!base::ReadFileToString(path, &contents)) {
      return error("ENOENT", "no such file or directory", "open");
    }
    return success(base::Value(base::Base64Encode(contents)));
  }

  if (*operation == "write_file" || *operation == "append_file") {
    const std::string* encoded = request.FindString("dataBase64");
    std::string data;
    if (!encoded || !base::Base64Decode(*encoded, &data)) {
      return error("EINVAL", "invalid file data", "write");
    }
    if (!base::DirectoryExists(path.DirName())) {
      return error("ENOENT", "no such file or directory", "open");
    }
    if (base::DirectoryExists(path)) {
      return error("EISDIR", "illegal operation on a directory", "open");
    }
    const bool ok = *operation == "append_file"
                        ? base::AppendToFile(path, data)
                        : base::WriteFile(path, data);
    if (!ok) {
      return error("EACCES", "permission denied", "open");
    }
    return success();
  }

  if (*operation == "readdir") {
    if (!base::PathExists(path)) {
      return error("ENOENT", "no such file or directory", "scandir");
    }
    if (!base::DirectoryExists(path)) {
      return error("ENOTDIR", "not a directory", "scandir");
    }
    base::ListValue names;
    base::FileEnumerator enumerator(
        path, false,
        base::FileEnumerator::FILES | base::FileEnumerator::DIRECTORIES);
    for (base::FilePath child = enumerator.Next(); !child.empty();
         child = enumerator.Next()) {
      names.Append(child.BaseName().AsUTF8Unsafe());
    }
    return success(base::Value(std::move(names)));
  }

  if (*operation == "mkdir") {
    const bool recursive = request.FindBool("recursive").value_or(false);
    if (base::PathExists(path)) {
      if (recursive && base::DirectoryExists(path)) {
        return success();
      }
      return error("EEXIST", "file already exists", "mkdir");
    }
    if (!recursive && !base::DirectoryExists(path.DirName())) {
      return error("ENOENT", "no such file or directory", "mkdir");
    }
    if (!base::CreateDirectory(path)) {
      return error("EACCES", "permission denied", "mkdir");
    }
    return success();
  }

  if (*operation == "unlink") {
    if (!base::PathExists(path)) {
      return error("ENOENT", "no such file or directory", "unlink");
    }
    if (base::DirectoryExists(path)) {
      return error("EISDIR", "illegal operation on a directory", "unlink");
    }
    if (!base::DeleteFile(path)) {
      return error("EACCES", "permission denied", "unlink");
    }
    return success();
  }

  if (*operation == "rm" || *operation == "rmdir") {
    const bool force = request.FindBool("force").value_or(false);
    const bool recursive = request.FindBool("recursive").value_or(false);
    if (!base::PathExists(path)) {
      return force ? success()
                   : error("ENOENT", "no such file or directory",
                           operation->c_str());
    }
    if (*operation == "rmdir" && !base::DirectoryExists(path)) {
      return error("ENOTDIR", "not a directory", "rmdir");
    }
    const bool ok = recursive ? base::DeletePathRecursively(path)
                              : base::DeleteFile(path);
    if (!ok) {
      return error("ENOTEMPTY", "directory not empty", operation->c_str());
    }
    return success();
  }

  if (*operation == "access") {
    if (!base::PathExists(path)) {
      return error("ENOENT", "no such file or directory", "access");
    }
    return success();
  }

  if (*operation == "rename" || *operation == "copy_file") {
    const std::string* destination_string = request.FindString("destination");
    if (!destination_string) {
      return error("EINVAL", "missing destination", operation->c_str());
    }
    const base::FilePath destination =
        base::FilePath::FromUTF8Unsafe(*destination_string);
    if (!base::PathExists(path)) {
      return error("ENOENT", "no such file or directory",
                   operation->c_str());
    }
    if (!base::DirectoryExists(destination.DirName())) {
      return error("ENOENT", "no such file or directory",
                   operation->c_str());
    }
    const bool ok = *operation == "rename"
                        ? base::Move(path, destination)
                        : base::CopyFile(path, destination);
    if (!ok) {
      return error("EACCES", "permission denied", operation->c_str());
    }
    return success();
  }

  if (*operation == "realpath") {
    if (!base::PathExists(path)) {
      return error("ENOENT", "no such file or directory", "realpath");
    }
    base::FilePath normalized;
    if (!base::NormalizeFilePath(path, &normalized)) {
      normalized = path;
    }
    return success(base::Value(normalized.AsUTF8Unsafe()));
  }

  // Windows does not expose POSIX mode bits with Node's semantics. Matching
  // Node's successful no-op behavior for modes unsupported by this host is
  // preferable to fabricating an in-memory permission model.
  if (*operation == "chmod") {
    if (!base::PathExists(path)) {
      return error("ENOENT", "no such file or directory", "chmod");
    }
    return success();
  }

  return error("ENOSYS", "operation not supported", operation->c_str());
}

std::string XenonIpcMainContainer::AddRenderer(
    mojo::PendingRemote<xenon::ipc::mojom::IpcRenderer> renderer,
    int32_t process_id,
    int32_t frame_id,
    int32_t window_id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  const std::string endpoint_id = base::NumberToString(next_endpoint_id_++);
  AddRenderer(endpoint_id, std::move(renderer), process_id, frame_id,
              window_id);
  return endpoint_id;
}

void XenonIpcMainContainer::AddRenderer(
    const std::string& endpoint_id,
    mojo::PendingRemote<xenon::ipc::mojom::IpcRenderer> renderer,
    int32_t process_id,
    int32_t frame_id,
    int32_t window_id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  RemoveRenderer(endpoint_id);
  mojo::Remote<xenon::ipc::mojom::IpcRenderer> remote(std::move(renderer));
  remote.set_disconnect_handler(
      base::BindOnce(&XenonIpcMainContainer::RemoveRenderer,
                     weak_factory_.GetWeakPtr(), endpoint_id));
  renderers_.emplace(endpoint_id,
                     RendererEndpoint{process_id, frame_id, window_id,
                                      std::move(remote)});
  DispatchRendererEvent(endpoint_id, true);
}

void XenonIpcMainContainer::RemoveRenderer(const std::string& endpoint_id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (renderers_.contains(endpoint_id)) {
    DispatchRendererEvent(endpoint_id, false);
    renderers_.erase(endpoint_id);
  }
}

void XenonIpcMainContainer::DispatchRendererEvent(
    const std::string& endpoint_id,
    bool attached) {
  if (!initialized_ || shutting_down_ || dispatch_renderer_event_.IsEmpty()) {
    return;
  }
  ScopedV8Context scope(isolate_, context_);
  v8::Local<v8::Value> argv[] = {
      CreateSenderMetadata(endpoint_id), v8::Boolean::New(isolate_, attached)};
  gin::TryCatch try_catch(isolate_);
  if (dispatch_renderer_event_.Get(isolate_)
          ->Call(scope.context(), scope.context()->Global(), std::size(argv),
                 argv)
          .IsEmpty()) {
    LOG(ERROR) << "Electron renderer lifecycle event failed: "
               << try_catch.GetStackTrace();
  }
  isolate_->PerformMicrotaskCheckpoint();
}

void XenonIpcMainContainer::DispatchToRenderer(
    const std::string& endpoint_id,
    const std::string& channel,
    base::Value arguments) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto renderer = renderers_.find(endpoint_id);
  if (renderer != renderers_.end()) {
    renderer->second.remote->Dispatch(channel, std::move(arguments));
  }
}

void XenonIpcMainContainer::DispatchWindowEvent(
    int32_t window_id,
    const std::string& event_name,
    base::Value arguments) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!initialized_ || dispatch_window_event_.IsEmpty()) {
    return;
  }
  ScopedV8Context scope(isolate_, context_);
  v8::Local<v8::Value> argv[] = {
      v8::Integer::New(isolate_, window_id),
      gin::StringToV8(isolate_, event_name),
      ValueToV8(arguments).ToLocalChecked(),
  };
  gin::TryCatch try_catch(isolate_);
  if (dispatch_window_event_.Get(isolate_)
          ->Call(scope.context(), scope.context()->Global(), std::size(argv),
                 argv)
          .IsEmpty()) {
    LOG(ERROR) << "Electron BrowserWindow event failed: "
               << try_catch.GetStackTrace();
  }
  isolate_->PerformMicrotaskCheckpoint();
}

v8::MaybeLocal<v8::Value> XenonIpcMainContainer::ValueToV8(
    const base::Value& value) {
  std::string json;
  if (!base::JSONWriter::Write(value, &json)) {
    return {};
  }
  v8::Local<v8::String> json_value =
      v8::String::NewFromUtf8(isolate_, json.c_str()).ToLocalChecked();
  return v8::JSON::Parse(context_.Get(isolate_), json_value);
}

bool XenonIpcMainContainer::V8ToValue(v8::Local<v8::Value> value,
                                      base::Value* output,
                                      std::string* error) {
  return ConvertV8ToValue(isolate_, context_.Get(isolate_), value, output,
                          error, 0);
}

v8::Local<v8::Object> XenonIpcMainContainer::CreateSenderMetadata(
    const std::string& endpoint_id) {
  v8::Local<v8::Context> context = context_.Get(isolate_);
  v8::Local<v8::Object> sender = v8::Object::New(isolate_);
  const auto endpoint = renderers_.find(endpoint_id);
  const int32_t process_id =
      endpoint == renderers_.end() ? 0 : endpoint->second.process_id;
  const int32_t frame_id =
      endpoint == renderers_.end() ? 0 : endpoint->second.frame_id;
  const int32_t window_id =
      endpoint == renderers_.end() ? 0 : endpoint->second.window_id;
  sender
      ->Set(context, v8::String::NewFromUtf8Literal(isolate_, "endpointId"),
            v8::String::NewFromUtf8(isolate_, endpoint_id.c_str())
                .ToLocalChecked())
      .Check();
  sender
      ->Set(context, v8::String::NewFromUtf8Literal(isolate_, "processId"),
            v8::Integer::New(isolate_, process_id))
      .Check();
  sender
      ->Set(context, v8::String::NewFromUtf8Literal(isolate_, "frameId"),
            v8::Integer::New(isolate_, frame_id))
      .Check();
  sender
      ->Set(context, v8::String::NewFromUtf8Literal(isolate_, "windowId"),
            v8::Integer::New(isolate_, window_id))
      .Check();
  return sender;
}

void XenonIpcMainContainer::Send(const std::string& endpoint_id,
                                 const std::string& channel,
                                 base::Value arguments) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!initialized_) {
    return;
  }
  ScopedV8Context scope(isolate_, context_);
  v8::Local<v8::Value> args_value;
  if (!ValueToV8(arguments).ToLocal(&args_value)) {
    return;
  }
  v8::Local<v8::Value> argv[] = {
      CreateSenderMetadata(endpoint_id),
      v8::String::NewFromUtf8(isolate_, channel.c_str()).ToLocalChecked(),
      args_value,
  };
  gin::TryCatch try_catch(isolate_);
  if (dispatch_send_.Get(isolate_)
          ->Call(scope.context(), scope.context()->Global(), std::size(argv),
                 argv)
          .IsEmpty()) {
    LOG(ERROR) << "ipcMain send handler failed for " << channel << ": "
               << try_catch.GetStackTrace();
  }
  isolate_->PerformMicrotaskCheckpoint();
}

void XenonIpcMainContainer::Invoke(const std::string& endpoint_id,
                                   const std::string& channel,
                                   base::Value arguments,
                                   InvokeCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!initialized_) {
    std::move(callback).Run(ErrorResult(
        startup_error_.empty() ? "ipcMain container is not initialized"
                               : startup_error_));
    return;
  }

  ScopedV8Context scope(isolate_, context_);
  v8::Local<v8::Value> args_value;
  if (!ValueToV8(arguments).ToLocal(&args_value)) {
    std::move(callback).Run(ErrorResult("Failed to decode IPC arguments"));
    return;
  }
  v8::Local<v8::Value> argv[] = {
      CreateSenderMetadata(endpoint_id),
      v8::String::NewFromUtf8(isolate_, channel.c_str()).ToLocalChecked(),
      args_value,
  };
  gin::TryCatch try_catch(isolate_);
  v8::Local<v8::Value> result;
  if (!dispatch_invoke_.Get(isolate_)
           ->Call(scope.context(), scope.context()->Global(), std::size(argv),
                  argv)
           .ToLocal(&result)) {
    std::move(callback).Run(ErrorResult(try_catch.GetStackTrace()));
    return;
  }

  if (!result->IsPromise()) {
    base::Value converted;
    std::string error;
    if (!V8ToValue(result, &converted, &error)) {
      std::move(callback).Run(ErrorResult(error));
      return;
    }
    auto reply = xenon::ipc::mojom::IpcResult::New();
    reply->success = true;
    reply->value = std::move(converted);
    std::move(callback).Run(std::move(reply));
    return;
  }

  auto* reply =
      new PromiseReplyContext{weak_factory_.GetWeakPtr(), std::move(callback)};
  pending_promise_replies_.insert(reply);
  v8::Local<v8::External> data =
      v8::External::New(isolate_, reply, v8::kExternalPointerTypeTagDefault);
  v8::Local<v8::Function> resolved =
      v8::Function::New(scope.context(), OnPromiseResolved, data)
          .ToLocalChecked();
  v8::Local<v8::Function> rejected =
      v8::Function::New(scope.context(), OnPromiseRejected, data)
          .ToLocalChecked();
  if (result.As<v8::Promise>()
          ->Then(scope.context(), resolved, rejected)
          .IsEmpty()) {
    pending_promise_replies_.erase(reply);
    std::move(reply->callback)
        .Run(ErrorResult("Failed to attach to ipcMain Promise"));
    delete reply;
    return;
  }
  isolate_->PerformMicrotaskCheckpoint();
}

xenon::ipc::mojom::IpcResultPtr XenonIpcMainContainer::SendSync(
    const std::string& endpoint_id,
    const std::string& channel,
    base::Value arguments) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!initialized_) {
    return ErrorResult("ipcMain container is not initialized");
  }

  ScopedV8Context scope(isolate_, context_);
  v8::Local<v8::Value> args_value;
  if (!ValueToV8(arguments).ToLocal(&args_value)) {
    return ErrorResult("Failed to decode synchronous IPC arguments");
  }
  v8::Local<v8::Value> argv[] = {
      CreateSenderMetadata(endpoint_id),
      v8::String::NewFromUtf8(isolate_, channel.c_str()).ToLocalChecked(),
      args_value,
  };
  gin::TryCatch try_catch(isolate_);
  v8::Local<v8::Value> result;
  if (!dispatch_sync_.Get(isolate_)
           ->Call(scope.context(), scope.context()->Global(), std::size(argv),
                  argv)
           .ToLocal(&result)) {
    return ErrorResult(try_catch.GetStackTrace());
  }
  if (result->IsPromise()) {
    return ErrorResult("sendSync handler returned a Promise");
  }

  base::Value converted;
  std::string error;
  if (!V8ToValue(result, &converted, &error)) {
    return ErrorResult(error);
  }
  auto reply = xenon::ipc::mojom::IpcResult::New();
  reply->success = true;
  reply->value = std::move(converted);
  return reply;
}

void XenonIpcMainContainer::NativeSendToRenderer(gin::Arguments* args) {
  std::string endpoint_id;
  std::string channel;
  if (!args->GetNext(&endpoint_id) || !args->GetNext(&channel)) {
    args->ThrowTypeError("sendToRenderer expects endpoint and channel");
    return;
  }
  v8::Local<v8::Value> value = args->PeekNext();
  base::Value arguments;
  std::string error;
  if (value.IsEmpty() || !V8ToValue(value, &arguments, &error)) {
    args->ThrowTypeError(error.empty() ? "Invalid IPC arguments" : error);
    return;
  }
  if (endpoint_id == "*") {
    for (auto& renderer : renderers_) {
      renderer.second.remote->Dispatch(channel, arguments.Clone());
    }
    return;
  }
  auto renderer = renderers_.find(endpoint_id);
  if (renderer != renderers_.end()) {
    renderer->second.remote->Dispatch(channel, std::move(arguments));
  }
}

void XenonIpcMainContainer::NativeShowOpenDialog(gin::Arguments* args) {
  std::string title = "打开文件";
  bool directory = false;
  bool allow_multi = false;
  std::vector<std::string> extensions;

  v8::Isolate* isolate = isolate_;
  v8::Local<v8::Context> context = context_.Get(isolate);
  v8::Local<v8::Value> options_val;
  if (args->GetNext(&options_val) && options_val->IsObject()) {
    v8::Local<v8::Object> options_obj = options_val.As<v8::Object>();
    v8::Local<v8::Value> title_v;
    if (options_obj->Get(context, gin::StringToV8(isolate, "title"))
            .ToLocal(&title_v) &&
        title_v->IsString()) {
      gin::ConvertFromV8(isolate, title_v, &title);
    }
    v8::Local<v8::Value> props_v;
    if (options_obj->Get(context, gin::StringToV8(isolate, "properties"))
            .ToLocal(&props_v) &&
        props_v->IsArray()) {
      v8::Local<v8::Array> props = props_v.As<v8::Array>();
      for (uint32_t i = 0; i < props->Length(); ++i) {
        v8::Local<v8::Value> item;
        std::string name;
        if (!props->Get(context, i).ToLocal(&item) ||
            !gin::ConvertFromV8(isolate, item, &name)) {
          continue;
        }
        if (name == "openDirectory") {
          directory = true;
        } else if (name == "multiSelections") {
          allow_multi = true;
        }
      }
    }
    v8::Local<v8::Value> filters_v;
    if (options_obj->Get(context, gin::StringToV8(isolate, "filters"))
            .ToLocal(&filters_v) &&
        filters_v->IsArray()) {
      v8::Local<v8::Array> filters = filters_v.As<v8::Array>();
      for (uint32_t i = 0; i < filters->Length(); ++i) {
        v8::Local<v8::Value> filter_v;
        if (!filters->Get(context, i).ToLocal(&filter_v) ||
            !filter_v->IsObject()) {
          continue;
        }
        v8::Local<v8::Value> exts_v;
        if (!filter_v.As<v8::Object>()
                 ->Get(context, gin::StringToV8(isolate, "extensions"))
                 .ToLocal(&exts_v) ||
            !exts_v->IsArray()) {
          continue;
        }
        v8::Local<v8::Array> exts = exts_v.As<v8::Array>();
        for (uint32_t j = 0; j < exts->Length(); ++j) {
          v8::Local<v8::Value> ext_v;
          std::string ext;
          if (!exts->Get(context, j).ToLocal(&ext_v) ||
              !gin::ConvertFromV8(isolate, ext_v, &ext) || ext.empty()) {
            continue;
          }
          if (ext[0] == '.') {
            ext.erase(0, 1);
          }
          extensions.push_back(std::move(ext));
        }
      }
    }
  }

  std::vector<std::string> results;
  if (!window_hooks_.show_open_dialog) {
    LOG(ERROR) << "showOpenDialog hook is not bound";
    args->Return(results);
    return;
  }
  LOG(INFO) << "showOpenDialog hop title=" << title
            << " directory=" << directory << " multi=" << allow_multi
            << " filters=" << extensions.size();
  {
    v8::Unlocker unlocker(isolate_);
    if (!window_hooks_.show_open_dialog.Run(title, directory, allow_multi,
                                            extensions, &results)) {
      LOG(ERROR) << "showOpenDialog hop to Browser failed";
      results.clear();
    }
  }
  LOG(INFO) << "showOpenDialog hop returned " << results.size() << " path(s)";
  args->Return(results);
}

void XenonIpcMainContainer::NativeCreateBrowserWindow(gin::Arguments* args) {
  if (!window_hooks_.create) {
    args->ThrowTypeError("BrowserWindow host is not bound");
    return;
  }
  v8::Local<v8::Object> options;
  if (!args->GetNext(&options)) {
    args->ThrowTypeError("createBrowserWindow expects an options object");
    return;
  }
  gin::Dictionary dict(args->isolate(), options);
  int width = 800;
  int height = 600;
  bool show = true;
  bool frame = true;
  bool transparent = false;
  int32_t parent_id = 0;
  std::string title;
  dict.Get("width", &width);
  dict.Get("height", &height);
  dict.Get("show", &show);
  dict.Get("frame", &frame);
  dict.Get("transparent", &transparent);
  dict.Get("parentId", &parent_id);
  dict.Get("title", &title);

  int32_t window_id = 0;
  uint64_t hwnd = 0;
  std::string error;
  bool ok = false;
  {
    v8::Unlocker unlocker(isolate_);
    ok = window_hooks_.create.Run(width, height, show, frame, transparent,
                                  parent_id, title, &window_id, &hwnd, &error);
  }
  if (!ok) {
    args->ThrowTypeError(error.empty() ? "Failed to create BrowserWindow"
                                       : error);
    return;
  }
  if (!error.empty()) {
    args->ThrowTypeError(error);
    return;
  }
  gin::Dictionary result = gin::Dictionary::CreateEmpty(args->isolate());
  result.Set("id", window_id);
  result.Set("hwnd", base::NumberToString(hwnd));
  args->Return(result);
}

void XenonIpcMainContainer::NativeLoadBrowserWindowURL(gin::Arguments* args) {
  int32_t window_id = 0;
  std::string url;
  if (!args->GetNext(&window_id) || !args->GetNext(&url)) {
    args->ThrowTypeError("loadURL expects window id and url");
    return;
  }
  if (window_hooks_.load_url) {
    v8::Unlocker unlocker(isolate_);
    window_hooks_.load_url.Run(window_id, url);
  }
}

void XenonIpcMainContainer::NativeSetBrowserWindowVisible(gin::Arguments* args) {
  int32_t window_id = 0;
  bool visible = true;
  if (!args->GetNext(&window_id) || !args->GetNext(&visible)) {
    args->ThrowTypeError("setVisible expects window id and visible");
    return;
  }
  if (window_hooks_.set_visible) {
    v8::Unlocker unlocker(isolate_);
    window_hooks_.set_visible.Run(window_id, visible);
  }
}

void XenonIpcMainContainer::NativeBrowserWindowCall(gin::Arguments* args) {
  int32_t window_id = 0;
  std::string command;
  if (!args->GetNext(&window_id) || !args->GetNext(&command)) {
    args->ThrowTypeError("BrowserWindow call expects window id and command");
    return;
  }
  base::Value arguments(base::DictValue{});
  v8::Local<v8::Value> value = args->PeekNext();
  std::string error;
  if (!value.IsEmpty() && !value->IsUndefined() &&
      !V8ToValue(value, &arguments, &error)) {
    args->ThrowTypeError(error.empty() ? "Invalid BrowserWindow arguments"
                                       : error);
    return;
  }
  if (!window_hooks_.call) {
    args->ThrowTypeError("BrowserWindow call hook is not bound");
    return;
  }
  base::Value result;
  bool ok = false;
  {
    v8::Unlocker unlocker(isolate_);
    ok = window_hooks_.call.Run(window_id, command, arguments, &result, &error);
  }
  if (!ok || !error.empty()) {
    args->ThrowTypeError(error.empty() ? "BrowserWindow call failed" : error);
    return;
  }
  v8::MaybeLocal<v8::Value> converted = ValueToV8(result);
  v8::Local<v8::Value> return_value;
  if (!converted.ToLocal(&return_value)) {
    args->ThrowTypeError("BrowserWindow result conversion failed");
    return;
  }
  args->GetFunctionCallbackInfo()->GetReturnValue().Set(return_value);
}

void XenonIpcMainContainer::NativeCloseBrowserWindow(gin::Arguments* args) {
  int32_t window_id = 0;
  if (!args->GetNext(&window_id)) {
    args->ThrowTypeError("close expects window id");
    return;
  }
  if (window_hooks_.close) {
    v8::Unlocker unlocker(isolate_);
    window_hooks_.close.Run(window_id);
  }
}

void XenonIpcMainContainer::NativeInvokeExport(gin::Arguments* args) {
  std::string module_path;
  std::string function_name;
  v8::Local<v8::Value> args_val;
  if (!args->GetNext(&module_path) || !args->GetNext(&function_name) ||
      !args->GetNext(&args_val)) {
    args->ThrowTypeError(
        "invokeExport expects module path, export path, and arguments");
    return;
  }
  if (!native_addon_hooks_.invoke) {
    args->ThrowTypeError("Native addon invoke hook is not bound");
    return;
  }
  base::Value converted;
  std::string error;
  if (args_val.IsEmpty() || args_val->IsUndefined() || args_val->IsNull()) {
    converted = base::Value(base::Value::Type::LIST);
  } else if (!V8ToValue(args_val, &converted, &error)) {
    args->ThrowTypeError(error.empty() ? "Invalid native arguments" : error);
    return;
  }
  base::Value result;
  bool ok = false;
  {
    v8::Unlocker unlocker(isolate_);
    ok = native_addon_hooks_.invoke.Run(module_path, function_name, converted,
                                        &result, &error);
  }
  if (!ok) {
    args->ThrowTypeError(error.empty() ? "Native export invocation failed"
                                       : error);
    return;
  }
  if (result.is_none()) {
    return;
  }
  v8::Local<v8::Value> out;
  if (!ValueToV8(result).ToLocal(&out)) {
    args->ThrowTypeError("Failed to convert native result");
    return;
  }
  args->Return(out);
}

void XenonIpcMainContainer::NativeConstructExport(gin::Arguments* args) {
  std::string module_path;
  std::string export_path;
  v8::Local<v8::Value> args_val;
  if (!args->GetNext(&module_path) || !args->GetNext(&export_path) ||
      !args->GetNext(&args_val)) {
    args->ThrowTypeError(
        "constructExport expects module path, export path, and arguments");
    return;
  }
  if (!native_addon_hooks_.construct) {
    args->ThrowTypeError("Native addon construct hook is not bound");
    return;
  }
  base::Value converted;
  std::string error;
  if (args_val.IsEmpty() || args_val->IsUndefined() || args_val->IsNull()) {
    converted = base::Value(base::Value::Type::LIST);
  } else if (!V8ToValue(args_val, &converted, &error)) {
    args->ThrowTypeError(error.empty() ? "Invalid native arguments" : error);
    return;
  }
  int32_t instance_id = 0;
  bool ok = false;
  {
    v8::Unlocker unlocker(isolate_);
    ok = native_addon_hooks_.construct.Run(module_path, export_path, converted,
                                           &instance_id, &error);
  }
  if (!ok) {
    args->ThrowTypeError(error.empty() ? "Native construct failed" : error);
    return;
  }
  args->Return(instance_id);
}

void XenonIpcMainContainer::NativeInvokeInstance(gin::Arguments* args) {
  std::string module_path;
  int32_t instance_id = 0;
  std::string method_name;
  v8::Local<v8::Value> args_val;
  if (!args->GetNext(&module_path) || !args->GetNext(&instance_id) ||
      !args->GetNext(&method_name) || !args->GetNext(&args_val)) {
    args->ThrowTypeError(
        "invokeInstance expects module path, instance id, method, and "
        "arguments");
    return;
  }
  if (!native_addon_hooks_.invoke_instance) {
    args->ThrowTypeError("Native addon instance hook is not bound");
    return;
  }
  base::Value converted;
  std::string error;
  if (args_val.IsEmpty() || args_val->IsUndefined() || args_val->IsNull()) {
    converted = base::Value(base::Value::Type::LIST);
  } else if (!V8ToValue(args_val, &converted, &error)) {
    args->ThrowTypeError(error.empty() ? "Invalid native arguments" : error);
    return;
  }
  base::Value result;
  bool ok = false;
  {
    v8::Unlocker unlocker(isolate_);
    ok = native_addon_hooks_.invoke_instance.Run(
        module_path, instance_id, method_name, converted, &result, &error);
  }
  if (!ok) {
    args->ThrowTypeError(error.empty() ? "Native instance invocation failed"
                                       : error);
    return;
  }
  if (result.is_none()) {
    return;
  }
  v8::Local<v8::Value> out;
  if (!ValueToV8(result).ToLocal(&out)) {
    args->ThrowTypeError("Failed to convert native result");
    return;
  }
  args->Return(out);
}

v8::MaybeLocal<v8::Value> XenonIpcMainContainer::CreateNativeAddonForwarder(
    const std::string& module_path) {
  v8::Local<v8::Context> context = context_.Get(isolate_);
  static constexpr char kFactorySource[] = R"((function(modulePath) {
  function wrapResult(value) {
    if (Array.isArray(value)) {
      return value.map(wrapResult);
    }
    if (!value || typeof value !== 'object') {
      return value;
    }
    const wireType = value.__xenon_node_wire_type__;
    if (wireType === 'undefined') {
      return undefined;
    }
    if (wireType === 'bigint') {
      try {
        return BigInt(value.value || '0');
      } catch (_error) {
        return 0n;
      }
    }
    if (wireType === 'native_instance') {
      return createInstanceProxy(
          value.instance_id, value.fields, value.prototype);
    }
    return value;
  }
  function createInstanceProxy(instanceId, fields, prototypeMembers) {
    const data = (fields && typeof fields === 'object') ? fields : {};
    const methods = {};
    for (const member of (prototypeMembers || [])) {
      if (!member || typeof member.name !== 'string') {
        continue;
      }
      methods[member.name] = function(...args) {
        return wrapResult(__xenonNativeInvokeInstance(
            modulePath, instanceId, member.name, args));
      };
    }
    const fn = function() {};
    return new Proxy(fn, {
      get(target, prop) {
        if (prop === '__instanceId') {
          return instanceId;
        }
        if (Object.prototype.hasOwnProperty.call(data, prop)) {
          return wrapResult(data[prop]);
        }
        if (Object.prototype.hasOwnProperty.call(methods, prop)) {
          return methods[prop];
        }
        if (typeof prop !== 'string' || prop === 'then') {
          return Reflect.get(target, prop);
        }
        return undefined;
      }
    });
  }
  function createPathProxy(exportPath) {
    const fn = function(...args) {
      if (new.target) {
        const id = __xenonNativeConstructExport(
            modulePath, exportPath, args);
        return createInstanceProxy(id);
      }
      return wrapResult(__xenonNativeInvokeExport(
          modulePath, exportPath, args));
    };
    return new Proxy(fn, {
      get(target, prop, receiver) {
        if (prop === 'then' || prop === '__esModule') {
          return undefined;
        }
        if (typeof prop === 'string' &&
            Object.prototype.hasOwnProperty.call(target, prop)) {
          return target[prop];
        }
        // Native forwarders are functions; callers use .call/.apply/.bind.
        // Those must not be rewritten into export-path proxies.
        if (prop === 'call' || prop === 'apply' || prop === 'bind') {
          return Function.prototype[prop].bind(target);
        }
        if (typeof prop !== 'string') {
          return Reflect.get(target, prop, receiver);
        }
        return createPathProxy(
            exportPath ? (exportPath + '.' + prop) : prop);
      },
      set(target, prop, value) {
        target[prop] = value;
        return true;
      },
      has(target, prop) {
        return Object.prototype.hasOwnProperty.call(target, prop) ||
            Reflect.has(target, prop);
      },
      ownKeys(target) {
        return Reflect.ownKeys(target);
      },
      getOwnPropertyDescriptor(target, prop) {
        return Object.getOwnPropertyDescriptor(target, prop) ||
            Reflect.getOwnPropertyDescriptor(target, prop);
      },
      construct(target, argsList) {
        const id = __xenonNativeConstructExport(
            modulePath, exportPath, argsList);
        return createInstanceProxy(id);
      }
    });
  }
  return createPathProxy('');
}))";
  v8::Local<v8::String> source =
      v8::String::NewFromUtf8(isolate_, kFactorySource,
                              v8::NewStringType::kNormal,
                              static_cast<int>(sizeof(kFactorySource) - 1))
          .ToLocalChecked();
  v8::Local<v8::Script> script;
  if (!v8::Script::Compile(context, source).ToLocal(&script)) {
    return {};
  }
  v8::Local<v8::Value> factory_value;
  if (!script->Run(context).ToLocal(&factory_value) ||
      !factory_value->IsFunction()) {
    return {};
  }
  v8::Local<v8::Value> argv[] = {gin::StringToV8(isolate_, module_path)};
  v8::Local<v8::Value> forwarder;
  if (!factory_value.As<v8::Function>()
           ->Call(context, context->Global(), 1, argv)
           .ToLocal(&forwarder)) {
    return {};
  }
  return forwarder;
}

// static
void XenonIpcMainContainer::OnPromiseResolved(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  auto* reply =
      static_cast<PromiseReplyContext*>(info.Data().As<v8::External>()->Value(
          v8::kExternalPointerTypeTagDefault));
  if (reply->owner) {
    reply->owner->CompletePromise(
        reply, true,
        info.Length() ? info[0] : v8::Undefined(info.GetIsolate()));
  }
}

// static
void XenonIpcMainContainer::OnPromiseRejected(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  auto* reply =
      static_cast<PromiseReplyContext*>(info.Data().As<v8::External>()->Value(
          v8::kExternalPointerTypeTagDefault));
  if (reply->owner) {
    reply->owner->CompletePromise(
        reply, false,
        info.Length() ? info[0] : v8::Undefined(info.GetIsolate()));
  }
}

void XenonIpcMainContainer::CompletePromise(PromiseReplyContext* reply,
                                            bool success,
                                            v8::Local<v8::Value> value) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  pending_promise_replies_.erase(reply);
  if (success) {
    base::Value converted;
    std::string error;
    if (!V8ToValue(value, &converted, &error)) {
      std::move(reply->callback).Run(ErrorResult(error));
    } else {
      auto result = xenon::ipc::mojom::IpcResult::New();
      result->success = true;
      result->value = std::move(converted);
      std::move(reply->callback).Run(std::move(result));
    }
  } else {
    v8::String::Utf8Value message(isolate_, value);
    std::move(reply->callback)
        .Run(ErrorResult(*message ? *message : "ipcMain handler rejected"));
  }
  delete reply;
}

void XenonIpcMainContainer::FailAllPendingPromises(const std::string& error) {
  std::set<PromiseReplyContext*> pending;
  pending.swap(pending_promise_replies_);
  for (PromiseReplyContext* reply : pending) {
    std::move(reply->callback).Run(ErrorResult(error));
    delete reply;
  }
}

void XenonIpcMainContainer::NativeSetTimeoutCallback(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  auto* self =
      static_cast<XenonIpcMainContainer*>(info.Data().As<v8::External>()->Value(
          v8::kExternalPointerTypeTagDefault));
  if (self) {
    self->HandleSetTimer(info, false);
  }
}

void XenonIpcMainContainer::NativeSetIntervalCallback(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  auto* self =
      static_cast<XenonIpcMainContainer*>(info.Data().As<v8::External>()->Value(
          v8::kExternalPointerTypeTagDefault));
  if (self) {
    self->HandleSetTimer(info, true);
  }
}

void XenonIpcMainContainer::NativeClearTimerCallback(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  auto* self =
      static_cast<XenonIpcMainContainer*>(info.Data().As<v8::External>()->Value(
          v8::kExternalPointerTypeTagDefault));
  if (self) {
    self->HandleClearTimer(info);
  }
}

void XenonIpcMainContainer::HandleSetTimer(
    const v8::FunctionCallbackInfo<v8::Value>& info,
    bool is_interval) {
  v8::Isolate* isolate = info.GetIsolate();
  if (info.Length() < 1 || !info[0]->IsFunction()) {
    info.GetReturnValue().Set(0);
    return;
  }
  int delay = 0;
  if (info.Length() > 1 && info[1]->IsNumber()) {
    delay = std::max(
        0, info[1]->Int32Value(isolate->GetCurrentContext()).FromMaybe(0));
  }
  if (is_interval && delay <= 0) {
    delay = 1;
  }

  int id = next_timer_id_++;
  auto timer = std::make_unique<TimerInfo>();
  timer->id = id;
  timer->is_interval = is_interval;
  timer->delay_ms = delay;
  timer->callback.Reset(isolate, info[0].As<v8::Function>());
  for (int i = 2; i < info.Length(); ++i) {
    timer->args.emplace_back(isolate, info[i]);
  }
  timers_[id] = std::move(timer);

  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&XenonIpcMainContainer::OnTimerTriggered,
                     weak_factory_.GetWeakPtr(), id),
      base::Milliseconds(delay));

  info.GetReturnValue().Set(id);
}

void XenonIpcMainContainer::HandleClearTimer(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  if (info.Length() < 1 || !info[0]->IsNumber()) {
    return;
  }
  int id =
      info[0]->Int32Value(info.GetIsolate()->GetCurrentContext()).FromMaybe(0);
  timers_.erase(id);
}

void XenonIpcMainContainer::OnTimerTriggered(int id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (shutting_down_ || !initialized_) {
    return;
  }
  auto it = timers_.find(id);
  if (it == timers_.end()) {
    return;
  }
  TimerInfo* timer = it->second.get();
  bool is_interval = timer->is_interval;
  int delay_ms = timer->delay_ms;

  {
    ScopedV8Context scope(isolate_, context_);
    v8::Local<v8::Context> context = context_.Get(isolate_);
    v8::Local<v8::Function> callback = timer->callback.Get(isolate_);
    std::vector<v8::Local<v8::Value>> argv;
    argv.reserve(timer->args.size());
    for (auto& arg : timer->args) {
      argv.push_back(arg.Get(isolate_));
    }
    v8::TryCatch try_catch(isolate_);
    std::ignore = callback->Call(context, context->Global(),
                                 static_cast<int>(argv.size()), argv.data());
    if (try_catch.HasCaught()) {
      v8::String::Utf8Value error(isolate_, try_catch.Exception());
      LOG(ERROR) << "Uncaught exception in timer callback: "
                 << (error.length() ? *error : "unknown error");
    }
  }

  if (is_interval) {
    if (timers_.find(id) != timers_.end()) {
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
          FROM_HERE,
          base::BindOnce(&XenonIpcMainContainer::OnTimerTriggered,
                         weak_factory_.GetWeakPtr(), id),
          base::Milliseconds(delay_ms));
    }
  } else {
    timers_.erase(id);
  }
}

xenon::ipc::mojom::IpcResultPtr XenonIpcMainContainer::ErrorResult(
    const std::string& error) const {
  auto result = xenon::ipc::mojom::IpcResult::New();
  result->success = false;
  result->value = base::Value();
  result->error = error.empty() ? "Unknown ipcMain error" : error;
  return result;
}

}  // namespace xenon::ipc
