// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_ipc_main_container.h"

#include <algorithm>
#include <cmath>
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
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "crypto/openssl_util.h"
#include "crypto/random.h"
#include "third_party/boringssl/src/include/openssl/evp.h"
#include "third_party/boringssl/src/include/openssl/hmac.h"

#if BUILDFLAG(IS_WIN)
#include <windows.h>
#endif
#include "base/task/bind_post_task.h"
#include "chrome/common/chrome_paths.h"
#include "components/version_info/version_info.h"
#include "gin/arguments.h"
#include "gin/converter.h"
#include "gin/dictionary.h"
#include "gin/function_template.h"
#include "gin/public/isolate_holder.h"
#include "gin/try_catch.h"
#include "gin/v8_initializer.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
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
#include "v8/include/v8-typed-array.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_app_runtime.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_file_system_bridge.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_network_request_bridge.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_os_bridge.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_zlib_bridge.h"
#include "xenon_overlay/chrome/browser/napi/napi_loader.h"
#include "xenon_overlay/common/ipc/xenon_ipc_value_codec.h"
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
    // A bundled application can fail long after its entry module completed.
    // Keep the real JS call site so startup regressions can be diagnosed from
    // the browser log. A hostile/custom stack getter must not escape logging.
    v8::TryCatch try_catch(isolate);
    v8::Local<v8::Value> printable = value;
    if (value->IsNativeError() && !isolate->GetCurrentContext().IsEmpty()) {
      v8::Local<v8::Value> stack;
      if (value.As<v8::Object>()
              ->Get(isolate->GetCurrentContext(),
                    gin::StringToV8(isolate, "stack"))
              .ToLocal(&stack) &&
          stack->IsString()) {
        printable = stack;
      }
      try_catch.Reset();
    }
    v8::String::Utf8Value utf8(isolate, printable);
    if (*utf8) {
      text.assign(*utf8, std::min<size_t>(utf8.length(), 8192));
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
  std::string endpoint_id;
  bool serialized = false;
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

void XenonIpcMainContainer::SetNetworkLoaderFactory(
    scoped_refptr<network::SharedURLLoaderFactory> factory) {
  network_loader_factory_ = std::move(factory);
}

void XenonIpcMainContainer::SetWindowHooks(WindowHooks hooks) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  window_hooks_ = std::move(hooks);
}

void XenonIpcMainContainer::SetNetPipeSender(
    base::RepeatingCallback<void(const std::string&, base::Value)> sender) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  net_pipe_sender_ = std::move(sender);
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
    bind_func("__xenonNetSend", &XenonIpcMainContainer::NativeNetSend);
    bind_func("__xenonFsCall", &XenonIpcMainContainer::NativeFsCall);
    bind_func("__xenonFsCallAsync", &XenonIpcMainContainer::NativeFsCallAsync);
    bind_func("__xenonCryptoCipher",
              &XenonIpcMainContainer::NativeCryptoCipher);
    bind_func("__xenonCryptoDigest",
              &XenonIpcMainContainer::NativeCryptoDigest);
    bind_func("__xenonCryptoRandom",
              &XenonIpcMainContainer::NativeCryptoRandom);
    bind_func("__xenonZlibCall", &XenonIpcMainContainer::NativeZlibCall);
    bind_func("__xenonHttpRequest", &XenonIpcMainContainer::NativeHttpRequest);
    bind_func("__xenonHttpAbort", &XenonIpcMainContainer::NativeHttpAbort);
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
    bind_func("__xenonNativeDescribeExport",
              &XenonIpcMainContainer::NativeDescribeExport);
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
  for (auto& [id, request] : pending_http_requests_) {
    if (request.cancel) {
      std::move(request.cancel).Run();
    }
  }
  FailAllPendingPromises("The Utility main JavaScript container stopped");
  renderers_.clear();
  module_resolution_cache_.clear();
  if (!context_.IsEmpty()) {
    ScopedV8Context scope(isolate_, context_);
    pending_fs_calls_.clear();
    pending_zlib_calls_.clear();
    pending_http_requests_.clear();
    timers_.clear();
    module_cache_.clear();
    loaded_node_addons_.clear();
  } else {
    pending_fs_calls_.clear();
    pending_zlib_calls_.clear();
    pending_http_requests_.clear();
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
  v8::TryCatch try_catch(isolate_);
  std::string error;
  v8::Local<v8::Value> ignored;
  v8::MaybeLocal<v8::Value> loaded =
      embedded_main_source_
          ? LoadCommonJsSource(main_script_path_, *embedded_main_source_,
                               &error)
          : LoadCommonJsModule(main_script_path_, &error);
  if (!loaded.ToLocal(&ignored)) {
    if (try_catch.HasCaught()) {
      v8::String::Utf8Value message(isolate_, try_catch.Exception());
      if (*message) {
        error.assign(*message, message.length());
      }
    }
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
  return LoadResolvedCommonJsModule(*resolved, error);
}

v8::MaybeLocal<v8::Value> XenonIpcMainContainer::LoadResolvedCommonJsModule(
    const base::FilePath& normalized,
    std::string* error) {
  if (!module_root_.empty() && normalized != module_root_ &&
      !module_root_.IsParent(normalized) &&
      !IsNativeAddonBesideExecutable(normalized, executable_path_)) {
    *error = "Module is outside the configured main JavaScript directory: " +
             normalized.AsUTF8Unsafe();
    return {};
  }

  const std::string cache_key = normalized.AsUTF8Unsafe();
  v8::Local<v8::Context> context = context_.Get(isolate_);
  auto cached = module_cache_.find(cache_key);
  if (cached != module_cache_.end()) {
    return cached->second.Get(isolate_).As<v8::Object>()->Get(
        context, v8::String::NewFromUtf8Literal(isolate_, "exports"));
  }
  auto cache_exports = [&](v8::Local<v8::Value> exports) {
    v8::Local<v8::Object> module = v8::Object::New(isolate_);
    module
        ->Set(context, v8::String::NewFromUtf8Literal(isolate_, "exports"),
              exports)
        .Check();
    module_cache_.emplace(cache_key, v8::Global<v8::Value>(isolate_, module));
  };

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
      cache_exports(forwarder);
      return forwarder;
    }
    auto loaded_addon = std::make_unique<xenon::LoadedNodeAddon>();
    v8::Local<v8::Value> exports = xenon::LoadAndInitializeNodeAddon(
        isolate_, context_.Get(isolate_), normalized, loaded_addon.get());
    if (exports.IsEmpty()) {
      *error = "Failed to load Node-API addon: " + cache_key;
      return {};
    }
    cache_exports(exports);
    loaded_node_addons_.emplace(cache_key, std::move(loaded_addon));
    return exports;
  }

  std::string source_text;
  if (!base::ReadFileToString(normalized, &source_text)) {
    *error = "Failed to read module: " + cache_key;
    return {};
  }

  if (normalized.MatchesExtension(FILE_PATH_LITERAL(".json"))) {
    v8::Local<v8::Value> exports;
    if (!v8::JSON::Parse(
             context, gin::StringToV8(isolate_, source_text).As<v8::String>())
             .ToLocal(&exports)) {
      *error = "Invalid JSON module: " + cache_key;
      return {};
    }
    cache_exports(exports);
    return exports;
  }

  return LoadCommonJsSource(normalized, source_text, error);
}

v8::MaybeLocal<v8::Value> XenonIpcMainContainer::LoadCommonJsSource(
    const base::FilePath& virtual_path,
    const std::string& source_text,
    std::string* error) {
  const std::string cache_key = virtual_path.AsUTF8Unsafe();
  v8::Local<v8::Context> context = context_.Get(isolate_);
  auto cached = module_cache_.find(cache_key);
  if (cached != module_cache_.end()) {
    return cached->second.Get(isolate_).As<v8::Object>()->Get(
        context, v8::String::NewFromUtf8Literal(isolate_, "exports"));
  }

  std::string wrapped =
      "(function(exports, require, module, __filename, __dirname) {\n" +
      source_text + "\n})";
  v8::TryCatch try_catch(isolate_);
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
    *error = "Failed to compile CommonJS module: " + cache_key;
    if (try_catch.HasCaught()) {
      try_catch.ReThrow();
    }
    return {};
  }

  v8::Local<v8::Object> exports = v8::Object::New(isolate_);
  v8::Local<v8::Object> module = v8::Object::New(isolate_);
  module
      ->Set(context, v8::String::NewFromUtf8Literal(isolate_, "exports"),
            exports)
      .Check();
  // Keep the module record: a circular dependency can observe a replacement
  // module.exports before this wrapper returns, or an exported function can
  // replace it after evaluation. An exports snapshot is stale in both cases.
  module_cache_.emplace(cache_key, v8::Global<v8::Value>(isolate_, module));

  const std::string parent_file = virtual_path.AsUTF8Unsafe();
  auto require_callback = [](const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Local<v8::Array> data = info.Data().As<v8::Array>();
    v8::Isolate* isolate = info.GetIsolate();
    v8::Local<v8::Context> context = isolate->GetCurrentContext();
    v8::Local<v8::Value> host_value;
    v8::Local<v8::Value> parent_value;
    v8::Local<v8::Value> resolve_only;
    if (!data->Get(context, 0).ToLocal(&host_value) ||
        !data->Get(context, 1).ToLocal(&parent_value) ||
        !data->Get(context, 2).ToLocal(&resolve_only) ||
        !host_value->IsExternal()) {
      return;
    }
    if (info.Length() < 1 || !info[0]->IsString()) {
      auto exception = v8::Exception::TypeError(v8::String::NewFromUtf8Literal(
          isolate, "The module id must be a string"));
      exception.As<v8::Object>()
          ->Set(context, v8::String::NewFromUtf8Literal(isolate, "code"),
                v8::String::NewFromUtf8Literal(isolate, "ERR_INVALID_ARG_TYPE"))
          .Check();
      isolate->ThrowException(exception);
      return;
    }
    if (info[0].As<v8::String>()->Length() == 0) {
      auto exception = v8::Exception::TypeError(v8::String::NewFromUtf8Literal(
          isolate, "The module id must not be empty"));
      exception.As<v8::Object>()
          ->Set(
              context, v8::String::NewFromUtf8Literal(isolate, "code"),
              v8::String::NewFromUtf8Literal(isolate, "ERR_INVALID_ARG_VALUE"))
          .Check();
      isolate->ThrowException(exception);
      return;
    }
    auto* host = static_cast<XenonIpcMainContainer*>(
        host_value.As<v8::External>()->Value(
            v8::kExternalPointerTypeTagDefault));
    v8::String::Utf8Value request(isolate, info[0]);
    v8::String::Utf8Value parent(isolate, parent_value);
    std::string require_error;
    v8::Local<v8::Value> result;
    if (*request && *parent) {
      v8::TryCatch require_try_catch(isolate);
      if (!host->RequireModule(std::string(*request, request.length()),
                               base::FilePath::FromUTF8Unsafe(*parent),
                               &require_error, resolve_only->IsTrue())
               .ToLocal(&result) &&
          require_try_catch.HasCaught()) {
        require_try_catch.ReThrow();
        return;
      }
    }
    if (result.IsEmpty()) {
      const std::string message = require_error.empty()
                                      ? (std::string("Cannot find module '") +
                                         (*request ? *request : "<null>") + "'")
                                      : require_error;
      std::string code = "MODULE_NOT_FOUND";
      if (message.starts_with("ERR_")) {
        code = message.substr(0, message.find(':'));
      }
      v8::Local<v8::String> exception_message =
          gin::StringToV8(isolate, message).As<v8::String>();
      v8::Local<v8::Value> exception =
          code.starts_with("ERR_INVALID_ARG_")
              ? v8::Exception::TypeError(exception_message)
              : v8::Exception::Error(exception_message);
      exception.As<v8::Object>()
          ->Set(context, v8::String::NewFromUtf8Literal(isolate, "code"),
                gin::StringToV8(isolate, code))
          .Check();
      isolate->ThrowException(exception);
      return;
    }
    info.GetReturnValue().Set(result);
  };
  auto create_require = [&](bool resolve_only) {
    v8::Local<v8::Array> data = v8::Array::New(isolate_, 3);
    data->Set(context, 0,
              v8::External::New(isolate_, this,
                                v8::kExternalPointerTypeTagDefault))
        .Check();
    data->Set(context, 1, gin::StringToV8(isolate_, parent_file)).Check();
    data->Set(context, 2, v8::Boolean::New(isolate_, resolve_only)).Check();
    return v8::Function::New(context, require_callback, data).ToLocalChecked();
  };
  v8::Local<v8::Function> require_function = create_require(false);
  require_function
      ->Set(context, v8::String::NewFromUtf8Literal(isolate_, "resolve"),
            create_require(true))
      .Check();

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
    *error = "Failed to execute CommonJS module: " + cache_key;
    if (try_catch.HasCaught()) {
      try_catch.ReThrow();
    }
    return {};
  }

  v8::Local<v8::Value> final_exports;
  if (!module->Get(context, v8::String::NewFromUtf8Literal(isolate_, "exports"))
           .ToLocal(&final_exports)) {
    module_cache_.erase(cache_key);
    *error = "Failed to read module.exports";
    if (try_catch.HasCaught()) {
      try_catch.ReThrow();
    }
    return {};
  }
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
  for (const base::FilePath::CharType* extension :
       {FILE_PATH_LITERAL("js"), FILE_PATH_LITERAL("json"),
        FILE_PATH_LITERAL("node")}) {
    if (std::optional<base::FilePath> with_extension =
            normalize_file(requested_path.AddExtension(extension))) {
      return with_extension;
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
    for (const char* index_file : {"index.js", "index.json", "index.node"}) {
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
    std::string* error,
    bool resolve_only) {
  v8::Local<v8::Context> context = context_.Get(isolate_);

  if (request.find('\0') != std::string::npos) {
    *error = "ERR_INVALID_ARG_VALUE: The module id must not contain null bytes";
    return {};
  }

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
          {"http2", "__xenonHttp2"},
          {"https", "__xenonHttps"},
          {"net", "__xenonNet"},
          {"node:assert", "__xenonAssert"},
          {"node:async_hooks", "__xenonAsyncHooks"},
          {"node:buffer", "__xenonBuffer"},
          {"node:child_process", "__xenonChildProcess"},
          {"node:constants", "__xenonConstants"},
          {"node:crypto", "__xenonCrypto"},
          {"node:dns", "__xenonDns"},
          {"node:events", "__xenonEvents"},
          {"node:fs", "__xenonFs"},
          {"node:fs/promises", "__xenonFsPromises"},
          {"node:http", "__xenonHttp"},
          {"node:http2", "__xenonHttp2"},
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
          {"node:tls", "__xenonTls"},
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
          {"tls", "__xenonTls"},
          {"tty", "__xenonTty"},
          {"url", "__xenonUrl"},
          {"util", "__xenonUtil"},
          {"zlib", "__xenonZlib"},
      });

  auto it = kBuiltinModules.find(request);
  if (it != kBuiltinModules.end()) {
    if (resolve_only) {
      return gin::StringToV8(isolate_, request);
    }
    return context->Global()->Get(
        context,
        v8::String::NewFromUtf8(isolate_, it->second).ToLocalChecked());
  }

  if (request.starts_with("node:")) {
    *error = "ERR_UNKNOWN_BUILTIN_MODULE: No such built-in module: " + request;
    return {};
  }
  if (request.starts_with('#')) {
    *error =
        "ERR_NOT_SUPPORTED: package imports resolution is not implemented: " +
        request;
    return {};
  }

  const auto resolution_key =
      std::make_pair(parent_file.AsUTF8Unsafe(), request);
  auto resolution = module_resolution_cache_.find(resolution_key);
  if (resolution != module_resolution_cache_.end()) {
    auto cached = module_cache_.find(resolution->second);
    if (cached != module_cache_.end()) {
      if (resolve_only) {
        return gin::StringToV8(isolate_, resolution->second);
      }
      return cached->second.Get(isolate_).As<v8::Object>()->Get(
          context, v8::String::NewFromUtf8Literal(isolate_, "exports"));
    }
    module_resolution_cache_.erase(resolution);
  }

  auto load_resolved =
      [&](const base::FilePath& path) -> v8::MaybeLocal<v8::Value> {
    if (!module_root_.empty() && path != module_root_ &&
        !module_root_.IsParent(path) &&
        !IsNativeAddonBesideExecutable(path, executable_path_)) {
      *error = "Cannot find module '" + request + "'";
      return {};
    }
    if (resolve_only) {
      return gin::StringToV8(isolate_, path.AsUTF8Unsafe());
    }
    v8::Local<v8::Value> exports;
    if (!LoadResolvedCommonJsModule(path, error).ToLocal(&exports)) {
      return {};
    }
    module_resolution_cache_.insert_or_assign(resolution_key,
                                              path.AsUTF8Unsafe());
    return exports;
  };

  if (request == "." || request == ".." || request.starts_with("./") ||
      request.starts_with("../") || request.starts_with(".\\") ||
      request.starts_with("..\\") ||
      base::FilePath::FromUTF8Unsafe(request).IsAbsolute()) {
    base::FilePath path = base::FilePath::FromUTF8Unsafe(request);
    if (!path.IsAbsolute()) {
      path = parent_file.DirName().Append(path);
    }
    std::optional<base::FilePath> resolved = ResolveCommonJsPath(path, error);
    return resolved ? load_resolved(*resolved) : v8::MaybeLocal<v8::Value>();
  }

  const base::FilePath request_path = base::FilePath::FromUTF8Unsafe(request);
  if (!request_path.empty() && !request_path.ReferencesParent()) {
    base::FilePath start_dir = parent_file.DirName();
    for (base::FilePath directory = start_dir;;) {
      if (directory != module_root_ && !module_root_.IsParent(directory)) {
        break;
      }
      if (directory.BaseName().value() != FILE_PATH_LITERAL("node_modules")) {
        const base::FilePath modules = directory.AppendASCII("node_modules");
        const base::FilePath candidate = modules.Append(request_path);
        // Package exports must not be silently bypassed by package.main or a
        // filesystem subpath. This loader currently implements CommonJS only.
        size_t package_end = request.find_first_of("/\\");
        if (request.starts_with('@') && package_end != std::string::npos) {
          package_end = request.find_first_of("/\\", package_end + 1);
        }
        const base::FilePath package_path =
            modules
                .Append(base::FilePath::FromUTF8Unsafe(
                    request.substr(0, package_end)))
                .AppendASCII("package.json");
        std::string package_text;
        if (base::ReadFileToString(package_path, &package_text)) {
          std::optional<base::Value> package =
              base::JSONReader::Read(package_text, base::JSON_PARSE_RFC);
          if (!package || !package->is_dict()) {
            *error = "ERR_INVALID_PACKAGE_CONFIG: Invalid package.json: " +
                     package_path.AsUTF8Unsafe();
            return {};
          }
          if (package->GetDict().contains("exports")) {
            *error =
                "ERR_NOT_SUPPORTED: package exports resolution is not "
                "implemented: " +
                request;
            return {};
          }
        }
        std::string resolve_error;
        if (std::optional<base::FilePath> resolved =
                ResolveCommonJsPath(candidate, &resolve_error)) {
          return load_resolved(*resolved);
        }
        if (!resolve_error.starts_with("Cannot find module '")) {
          *error = std::move(resolve_error);
          return {};
        }
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
  *error = "Cannot find module '" + request + "'";
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

bool XenonIpcMainContainer::ReadFileSystemArguments(gin::Arguments* args,
                                                    base::Value* arguments) {
  v8::Local<v8::Value> request_value;
  base::Value request;
  std::string error;
  if (!args->GetNext(&request_value) ||
      !V8ToValue(request_value, &request, &error) || !request.is_dict()) {
    args->ThrowTypeError("fs call expects a request object");
    return false;
  }
  v8::Local<v8::Value> data;
  if (args->GetNext(&data) && !data->IsUndefined()) {
    base::Value bytes;
    if ((!data->IsArrayBuffer() && !data->IsArrayBufferView()) ||
        !V8ToValue(data, &bytes, &error) || !bytes.is_blob()) {
      args->ThrowTypeError("fs file data must be an ArrayBuffer or a view");
      return false;
    }
    request.GetDict().Set("data", std::move(bytes));
  }
  base::ListValue list;
  list.Append(std::move(request));
  *arguments = base::Value(std::move(list));
  return true;
}

void XenonIpcMainContainer::NativeFsCall(gin::Arguments* args) {
  base::Value arguments;
  if (!ReadFileSystemArguments(args, &arguments)) {
    return;
  }
  auto result = PerformFileSystemCall(std::move(arguments));
  if (!result->success) {
    isolate_->ThrowException(
        v8::Exception::Error(gin::StringToV8(isolate_, result->error)));
    return;
  }
  v8::Local<v8::Value> value;
  if (ValueToV8(result->value).ToLocal(&value)) {
    args->Return(value);
  }
}

void XenonIpcMainContainer::NativeFsCallAsync(gin::Arguments* args) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  base::Value arguments;
  if (!ReadFileSystemArguments(args, &arguments)) {
    return;
  }
  if (shutting_down_) {
    args->ThrowTypeError("The Utility main JavaScript container stopped");
    return;
  }
  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(context_.Get(isolate_)).ToLocal(&resolver)) {
    return;
  }
  const uint64_t request_id = next_fs_request_id_++;
  pending_fs_calls_.emplace(
      request_id, v8::Global<v8::Promise::Resolver>(isolate_, resolver));
  args->Return(resolver->GetPromise());
  // Snapshot the input bytes before handing off to the worker. Reply delivery
  // and all V8 handles stay on the originating sequence.
  if (!base::ThreadPool::PostTaskAndReplyWithResult(
          FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
          base::BindOnce(&PerformFileSystemCall, std::move(arguments)),
          base::BindOnce(&XenonIpcMainContainer::OnFileSystemCallComplete,
                         weak_factory_.GetWeakPtr(), request_id))) {
    pending_fs_calls_.erase(request_id);
    resolver
        ->Reject(context_.Get(isolate_),
                 v8::Exception::Error(gin::StringToV8(
                     isolate_, "EIO: unable to schedule filesystem task")))
        .Check();
  }
}

void XenonIpcMainContainer::OnFileSystemCallComplete(
    uint64_t request_id,
    xenon::ipc::mojom::IpcResultPtr result) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto found = pending_fs_calls_.find(request_id);
  if (shutting_down_ || found == pending_fs_calls_.end()) {
    return;
  }
  ScopedV8Context scope(isolate_, context_);
  v8::Local<v8::Promise::Resolver> resolver = found->second.Get(isolate_);
  pending_fs_calls_.erase(found);
  v8::Local<v8::Value> value;
  if (result->success && ValueToV8(result->value).ToLocal(&value)) {
    resolver->Resolve(scope.context(), value).Check();
  } else {
    resolver
        ->Reject(scope.context(),
                 v8::Exception::Error(gin::StringToV8(
                     isolate_, result->success
                                   ? "EIO: unable to convert filesystem result"
                                   : result->error)))
        .Check();
  }
  isolate_->PerformMicrotaskCheckpoint();
}

void XenonIpcMainContainer::NativeHttpRequest(gin::Arguments* args) {
  v8::Local<v8::Value> request;
  base::Value converted;
  std::string error;
  if (!args->GetNext(&request) || !V8ToValue(request, &converted, &error) ||
      !converted.is_dict()) {
    args->ThrowTypeError(
        "ERR_INVALID_ARG_TYPE: HTTP request must be an object");
    return;
  }
  if (shutting_down_ || pending_http_requests_.size() >= 128) {
    args->ThrowTypeError(
        "ERR_RESOURCE_BUSY: HTTP request queue is full or stopped");
    return;
  }
  v8::Local<v8::Context> context = context_.Get(isolate_);
  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(context).ToLocal(&resolver)) {
    return;
  }
  const uint64_t id = next_http_request_id_++;
  HttpRequestInfo info;
  info.resolver.Reset(isolate_, resolver);
  pending_http_requests_.emplace(id, std::move(info));
  v8::Local<v8::Object> operation = v8::Object::New(isolate_);
  operation
      ->Set(context, gin::StringToV8(isolate_, "id"),
            v8::Number::New(isolate_, static_cast<double>(id)))
      .Check();
  operation
      ->Set(context, gin::StringToV8(isolate_, "promise"),
            resolver->GetPromise())
      .Check();
  args->Return(operation);
  base::ListValue arguments;
  arguments.Append(std::move(converted));
  auto cancel = PerformNetworkRequest(
      network_loader_factory_, base::Value(std::move(arguments)),
      base::BindPostTaskToCurrentDefault(
          base::BindOnce(&XenonIpcMainContainer::OnHttpRequestComplete,
                         weak_factory_.GetWeakPtr(), id)));
  // Validation may have completed synchronously and removed this request.
  if (auto found = pending_http_requests_.find(id);
      found != pending_http_requests_.end()) {
    found->second.cancel = std::move(cancel);
  }
}

void XenonIpcMainContainer::NativeHttpAbort(gin::Arguments* args) {
  double id = 0;
  if (!args->GetNext(&id) || !std::isfinite(id) || id < 1 ||
      id > kMaxSafeInteger || std::floor(id) != id) {
    return;
  }
  auto found = pending_http_requests_.find(static_cast<uint64_t>(id));
  if (found != pending_http_requests_.end() && found->second.cancel) {
    auto cancel = std::move(found->second.cancel);
    std::move(cancel).Run();
  }
}

void XenonIpcMainContainer::OnHttpRequestComplete(
    uint64_t id,
    xenon::ipc::mojom::IpcResultPtr result) {
  auto found = pending_http_requests_.find(id);
  if (shutting_down_ || found == pending_http_requests_.end()) {
    return;
  }
  ScopedV8Context scope(isolate_, context_);
  v8::Local<v8::Promise::Resolver> resolver =
      found->second.resolver.Get(isolate_);
  pending_http_requests_.erase(found);
  v8::Local<v8::Value> value;
  if (result->success && ValueToV8(result->value).ToLocal(&value)) {
    resolver->Resolve(scope.context(), value).Check();
  } else {
    resolver
        ->Reject(
            scope.context(),
            v8::Exception::Error(gin::StringToV8(
                isolate_, result->success
                              ? "ERR_OPERATION_FAILED: invalid HTTP response"
                              : result->error)))
        .Check();
  }
  isolate_->PerformMicrotaskCheckpoint();
}

void XenonIpcMainContainer::NativeZlibCall(gin::Arguments* args) {
  std::string operation;
  v8::Local<v8::Value> input;
  v8::Local<v8::Value> options;
  bool asynchronous = false;
  base::Value bytes;
  base::Value settings;
  std::string error;
  if (!args->GetNext(&operation) || !args->GetNext(&input) ||
      !args->GetNext(&options) || !args->GetNext(&asynchronous) ||
      !V8ToValue(input, &bytes, &error) || !bytes.is_blob() ||
      !V8ToValue(options, &settings, &error) || !settings.is_dict()) {
    args->ThrowTypeError("zlib expects an operation, binary input and options");
    return;
  }
  if (!asynchronous) {
    auto result =
        PerformZlibCall(std::move(operation), std::move(bytes.GetBlob()),
                        std::move(settings.GetDict()));
    if (!result->success) {
      isolate_->ThrowException(
          v8::Exception::Error(gin::StringToV8(isolate_, result->error)));
      return;
    }
    v8::Local<v8::Value> value;
    if (ValueToV8(result->value).ToLocal(&value)) {
      args->Return(value);
    }
    return;
  }
  if (shutting_down_ || pending_zlib_calls_.size() >= 128) {
    args->ThrowTypeError(
        "ERR_RESOURCE_BUSY: zlib worker queue is full or stopped");
    return;
  }
  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(context_.Get(isolate_)).ToLocal(&resolver)) {
    return;
  }
  const uint64_t request_id = next_zlib_request_id_++;
  pending_zlib_calls_.emplace(
      request_id, v8::Global<v8::Promise::Resolver>(isolate_, resolver));
  args->Return(resolver->GetPromise());
  if (!base::ThreadPool::PostTaskAndReplyWithResult(
          FROM_HERE, {base::TaskPriority::USER_VISIBLE},
          base::BindOnce(&PerformZlibCall, std::move(operation),
                         std::move(bytes.GetBlob()),
                         std::move(settings.GetDict())),
          base::BindOnce(&XenonIpcMainContainer::OnZlibCallComplete,
                         weak_factory_.GetWeakPtr(), request_id))) {
    pending_zlib_calls_.erase(request_id);
    resolver
        ->Reject(context_.Get(isolate_),
                 v8::Exception::Error(gin::StringToV8(
                     isolate_, "ERR_RESOURCE_BUSY: cannot schedule zlib task")))
        .Check();
  }
}

void XenonIpcMainContainer::OnZlibCallComplete(
    uint64_t request_id,
    xenon::ipc::mojom::IpcResultPtr result) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto found = pending_zlib_calls_.find(request_id);
  if (shutting_down_ || found == pending_zlib_calls_.end()) {
    return;
  }
  ScopedV8Context scope(isolate_, context_);
  v8::Local<v8::Promise::Resolver> resolver = found->second.Get(isolate_);
  pending_zlib_calls_.erase(found);
  v8::Local<v8::Value> value;
  if (result->success && ValueToV8(result->value).ToLocal(&value)) {
    resolver->Resolve(scope.context(), value).Check();
  } else {
    resolver
        ->Reject(scope.context(),
                 v8::Exception::Error(gin::StringToV8(
                     isolate_,
                     result->success
                         ? "ERR_OPERATION_FAILED: cannot convert zlib result"
                         : result->error)))
        .Check();
  }
  isolate_->PerformMicrotaskCheckpoint();
}

void XenonIpcMainContainer::NativeCryptoDigest(gin::Arguments* args) {
  std::string algorithm;
  v8::Local<v8::Value> input;
  v8::Local<v8::Value> key;
  if (!args->GetNext(&algorithm) || !args->GetNext(&input) ||
      !args->GetNext(&key)) {
    args->ThrowTypeError(
        "crypto digest expects an algorithm, bytes and optional key");
    return;
  }
  const EVP_MD* digest = nullptr;
  if (algorithm == "md5") {
    digest = EVP_md5();
  } else if (algorithm == "sha1") {
    digest = EVP_sha1();
  } else if (algorithm == "sha224") {
    digest = EVP_sha224();
  } else if (algorithm == "sha256") {
    digest = EVP_sha256();
  } else if (algorithm == "sha384") {
    digest = EVP_sha384();
  } else if (algorithm == "sha512") {
    digest = EVP_sha512();
  }
  if (!digest) {
    args->ThrowTypeError("Unsupported digest algorithm: " + algorithm);
    return;
  }
  base::Value bytes;
  base::Value key_bytes;
  std::string error;
  const bool keyed = !key->IsNullOrUndefined();
  if (!V8ToValue(input, &bytes, &error) || !bytes.is_blob() ||
      (keyed &&
       (!V8ToValue(key, &key_bytes, &error) || !key_bytes.is_blob()))) {
    args->ThrowTypeError("crypto digest data and key must be binary buffers");
    return;
  }
  const auto& data = bytes.GetBlob();
  base::Value::BlobStorage output(EVP_MAX_MD_SIZE);
  unsigned int length = 0;
  const bool success =
      keyed
          ? HMAC(digest, key_bytes.GetBlob().data(), key_bytes.GetBlob().size(),
                 data.data(), data.size(), output.data(), &length) != nullptr
          : EVP_Digest(data.data(), data.size(), output.data(), &length, digest,
                       nullptr) == 1;
  if (!success) {
    args->ThrowTypeError("Native digest computation failed");
    return;
  }
  output.resize(length);
  v8::Local<v8::Value> result;
  if (!ValueToV8(base::Value(std::move(output))).ToLocal(&result)) {
    args->ThrowTypeError("Failed to convert digest result");
    return;
  }
  args->Return(result);
}

void XenonIpcMainContainer::NativeCryptoRandom(gin::Arguments* args) {
  int size = 0;
  if (!args->GetNext(&size) || size < 0) {
    args->ThrowTypeError("crypto random size must be a non-negative integer");
    return;
  }
  base::Value::BlobStorage bytes(static_cast<size_t>(size));
  crypto::RandBytes(bytes);
  v8::Local<v8::Value> result;
  if (!ValueToV8(base::Value(std::move(bytes))).ToLocal(&result)) {
    args->ThrowTypeError("Failed to convert random bytes");
    return;
  }
  args->Return(result);
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
  return PerformFileSystemCall(std::move(arguments));
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
  FailPendingPromisesForEndpoint(
      endpoint_id, "Renderer disconnected before ipcMain handler completed");
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
  if (value.is_blob()) {
    const auto& bytes = value.GetBlob();
    v8::Local<v8::ArrayBuffer> buffer =
        v8::ArrayBuffer::New(isolate_, bytes.size());
    if (!bytes.empty()) {
      auto store = buffer->GetBackingStore();
      auto destination = UNSAFE_BUFFERS(base::span(
          static_cast<uint8_t*>(store->Data()), store->ByteLength()));
      destination.copy_from(base::span(bytes));
    }
    return v8::Uint8Array::New(buffer, 0, bytes.size());
  }
  std::string json;
  if (!base::JSONWriter::Write(value, &json)) {
    return {};
  }
  v8::Local<v8::String> json_value =
      v8::String::NewFromUtf8(isolate_, json.c_str()).ToLocalChecked();
  return v8::JSON::Parse(context_.Get(isolate_), json_value);
}

v8::MaybeLocal<v8::Value> XenonIpcMainContainer::IpcPayloadToV8(
    const base::Value& value,
    std::string* error) {
  if (IsSerializedIpcValue(value)) {
    return DeserializeIpcValue(isolate_, context_.Get(isolate_), value, error);
  }
  v8::Local<v8::Value> converted;
  if (!ValueToV8(value).ToLocal(&converted)) {
    if (error) {
      *error = "Failed to decode private IPC arguments";
    }
    return {};
  }
  if (error) {
    error->clear();
  }
  return converted;
}

bool XenonIpcMainContainer::V8ToValue(v8::Local<v8::Value> value,
                                      base::Value* output,
                                      std::string* error) {
  return ConvertV8ToValue(isolate_, context_.Get(isolate_), value, output,
                          error, 0);
}

bool XenonIpcMainContainer::V8ToIpcPayload(v8::Local<v8::Value> value,
                                           bool serialized,
                                           base::Value* output,
                                           std::string* error) {
  if (!serialized) {
    return V8ToValue(value, output, error);
  }
  return SerializeIpcValue(isolate_, context_.Get(isolate_), value,
                           /*rethrow_exception=*/false, output, error);
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
  std::string decode_error;
  if (!IpcPayloadToV8(arguments, &decode_error).ToLocal(&args_value)) {
    LOG(ERROR) << "Failed to decode IPC send '" << channel
               << "': " << decode_error;
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
  const bool serialized = IsSerializedIpcValue(arguments);
  v8::Local<v8::Value> args_value;
  std::string decode_error;
  if (!IpcPayloadToV8(arguments, &decode_error).ToLocal(&args_value)) {
    std::move(callback).Run(ErrorResult(decode_error.empty()
                                            ? "Failed to decode IPC arguments"
                                            : decode_error));
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
    if (!V8ToIpcPayload(result, serialized, &converted, &error)) {
      std::move(callback).Run(ErrorResult(error));
      return;
    }
    auto reply = xenon::ipc::mojom::IpcResult::New();
    reply->success = true;
    reply->value = std::move(converted);
    std::move(callback).Run(std::move(reply));
    return;
  }

  auto* reply = new PromiseReplyContext{
      weak_factory_.GetWeakPtr(), std::move(callback), endpoint_id, serialized};
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
  const bool serialized = IsSerializedIpcValue(arguments);
  v8::Local<v8::Value> args_value;
  std::string decode_error;
  if (!IpcPayloadToV8(arguments, &decode_error).ToLocal(&args_value)) {
    return ErrorResult(decode_error.empty()
                           ? "Failed to decode synchronous IPC arguments"
                           : decode_error);
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
  if (!V8ToIpcPayload(result, serialized, &converted, &error)) {
    return ErrorResult(error);
  }
  auto reply = xenon::ipc::mojom::IpcResult::New();
  reply->success = true;
  reply->value = std::move(converted);
  return reply;
}

void XenonIpcMainContainer::NativeNetSend(gin::Arguments* args) {
  std::string channel;
  if (!args->GetNext(&channel) || !channel.starts_with("__xenon:net:")) {
    args->ThrowTypeError("netSend expects a net channel");
    return;
  }
  base::Value payload;
  std::string error;
  v8::Local<v8::Value> value = args->PeekNext();
  if (value.IsEmpty() || !V8ToValue(value, &payload, &error) ||
      !payload.is_dict()) {
    args->ThrowTypeError(error.empty() ? "Invalid net payload" : error);
    return;
  }
  if (!net_pipe_sender_) {
    args->ThrowTypeError("Named-pipe transport is unavailable");
    return;
  }
  net_pipe_sender_.Run(channel, std::move(payload));
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
  const bool serialized = !IsInternalIpcChannel(channel);
  const bool converted =
      !value.IsEmpty() &&
      (serialized
           ? SerializeIpcValue(isolate_, context_.Get(isolate_), value,
                               /*rethrow_exception=*/true, &arguments, &error)
           : V8ToValue(value, &arguments, &error));
  if (!converted) {
    if (!serialized) {
      args->ThrowTypeError(error.empty() ? "Invalid private IPC arguments"
                                         : error);
    }
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

void XenonIpcMainContainer::NativeDescribeExport(gin::Arguments* args) {
  std::string module_path;
  std::string export_path;
  if (!args->GetNext(&module_path) || !args->GetNext(&export_path) ||
      !native_addon_hooks_.describe) {
    args->ThrowTypeError("Native addon description hook is not bound");
    return;
  }
  base::Value description;
  std::string error;
  bool ok;
  {
    v8::Unlocker unlocker(isolate_);
    ok = native_addon_hooks_.describe.Run(module_path, export_path,
                                          &description, &error);
  }
  if (!ok) {
    args->ThrowTypeError(error.empty() ? "Native export inspection failed"
                                       : error);
    return;
  }
  v8::Local<v8::Value> out;
  if (!ValueToV8(description).ToLocal(&out)) {
    args->ThrowTypeError("Failed to convert native export description");
    return;
  }
  args->Return(out);
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
  base::Value instance;
  bool ok = false;
  {
    v8::Unlocker unlocker(isolate_);
    ok = native_addon_hooks_.construct.Run(module_path, export_path, converted,
                                           &instance, &error);
  }
  if (!ok) {
    args->ThrowTypeError(error.empty() ? "Native construct failed" : error);
    return;
  }
  v8::Local<v8::Value> out;
  if (!ValueToV8(instance).ToLocal(&out)) {
    args->ThrowTypeError("Failed to convert constructed native instance");
    return;
  }
  args->Return(out);
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
    const instance = {};
    Object.defineProperty(instance, '__instanceId', {value: instanceId});
    for (const name of Object.keys(data)) {
      Object.defineProperty(instance, name, {
        value: wrapResult(data[name]), enumerable: true,
        writable: true, configurable: true
      });
    }
    for (const member of (prototypeMembers || [])) {
      if (!member || typeof member.name !== 'string' ||
          (member.kind !== 'function' && member.kind !== 'class')) {
        continue;
      }
      Object.defineProperty(instance, member.name, {
        value: function(...args) {
          return wrapResult(__xenonNativeInvokeInstance(
              modulePath, instanceId, member.name, args));
        },
        enumerable: !!member.enumerable, writable: true, configurable: true
      });
    }
    return instance;
  }
  const cache = new Map();
  function unsupported(message) {
    const error = new TypeError(message);
    error.code = 'ERR_NOT_SUPPORTED';
    throw error;
  }
  function describeChild(name, childPath) {
    if (!name || name.includes('.')) {
      unsupported('Native property path cannot represent this name: ' + name);
    }
    return __xenonNativeDescribeExport(modulePath, childPath);
  }
  function build(info, exportPath, useCache = true) {
    if (!info || info.kind === 'undefined') return undefined;
    if (info.kind === 'null') return null;
    if (info.kind === 'bigint') return BigInt(info.value);
    if (info.hasValue) return wrapResult(info.value);
    if (!['object', 'array', 'function', 'class'].includes(info.kind)) {
      unsupported('Unsupported native export kind: ' + info.kind);
    }
    if (useCache && cache.has(exportPath)) return cache.get(exportPath);
    let target;
    if (info.kind === 'function' || info.kind === 'class') {
      // A bound function keeps the normal Function prototype while permitting
      // the addon's real own `prototype`, `name` and `length` descriptors.
      target = function(...args) {
        if (new.target) {
          const instance = wrapResult(__xenonNativeConstructExport(
              modulePath, exportPath, args));
          if (target.prototype && typeof target.prototype === 'object') {
            Object.setPrototypeOf(instance, target.prototype);
          }
          return instance;
        }
        return wrapResult(__xenonNativeInvokeExport(
            modulePath, exportPath, args));
      }.bind(null);
      Object.defineProperty(target, Symbol.hasInstance, {value(instance) {
        const prototype = target.prototype;
        if (!prototype || typeof prototype !== 'object') {
          throw new TypeError('Native constructor has no object prototype');
        }
        return Object.prototype.isPrototypeOf.call(prototype, instance);
      }});
    } else {
      target = info.kind === 'array' ? [] : {};
    }
    if (useCache) cache.set(exportPath, target);
    for (const child of (info.children || [])) {
      const name = child.name;
      const childPath = exportPath ? exportPath + '.' + name : name;
      const attributes = {enumerable: !!child.enumerable, configurable: true};
      if (child.kind === 'property') {
        Object.defineProperty(target, name, {...attributes, get() {
          const description = describeChild(name, childPath);
          if (['object', 'array', 'function', 'class'].includes(description.kind)) {
            unsupported('Native accessor object results require a retained handle');
          }
          return build(description, childPath, false);
        }, set() { unsupported('Native accessor writes are not supported'); }});
      } else if (['object', 'array', 'function', 'class'].includes(child.kind)) {
        Object.defineProperty(target, name, {...attributes, get() {
          const value = build(describeChild(name, childPath),
                              childPath, useCache);
          Object.defineProperty(target, name, {...attributes, value,
            writable: !!child.writable});
          return value;
        }, ...(child.writable ? {set(value) {
          Object.defineProperty(target, name, {...attributes, value, writable: true});
        }} : {})});
      } else {
        if (Array.isArray(target) && name === 'length') {
          target.length = child.value;
          continue;
        }
        Object.defineProperty(target, name, {...attributes,
          value: build(child, childPath, useCache), writable: !!child.writable});
      }
    }
    return target;
  }
  return build(__xenonNativeDescribeExport(modulePath, ''), '');
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
  if (!reply->callback) {
    delete reply;
    return;
  }
  if (success) {
    base::Value converted;
    std::string error;
    if (!V8ToIpcPayload(value, reply->serialized, &converted, &error)) {
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

void XenonIpcMainContainer::FailPendingPromisesForEndpoint(
    const std::string& endpoint_id,
    const std::string& error) {
  for (PromiseReplyContext* reply : pending_promise_replies_) {
    if (reply->endpoint_id == endpoint_id && reply->callback) {
      std::move(reply->callback).Run(ErrorResult(error));
    }
  }
}

void XenonIpcMainContainer::FailAllPendingPromises(const std::string& error) {
  std::set<PromiseReplyContext*> pending;
  pending.swap(pending_promise_replies_);
  for (PromiseReplyContext* reply : pending) {
    if (reply->callback) {
      std::move(reply->callback).Run(ErrorResult(error));
    }
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
