// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_ipc_main_container.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "base/auto_reset.h"
#include "base/base64.h"
#include "base/base_paths.h"
#include "base/check.h"
#include "base/command_line.h"
#include "base/compiler_specific.h"
#include "base/containers/fixed_flat_map.h"
#include "base/containers/span.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/memory/ref_counted.h"
#include "base/notreached.h"
#include "base/numerics/safe_conversions.h"
#include "base/path_service.h"
#include "base/process/process_handle.h"
#include "base/strings/escape.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/system/sys_info.h"
#include "base/task/sequenced_task_runner.h"
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
#include "content/public/common/content_switches.h"
#include "gin/arguments.h"
#include "gin/converter.h"
#include "gin/dictionary.h"
#include "gin/function_template.h"
#include "gin/public/isolate_holder.h"
#include "gin/try_catch.h"
#include "gin/v8_initializer.h"
#include "net/base/filename_util.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "ui/base/resource/resource_bundle.h"
#include "url/gurl.h"
#include "url/url_canon.h"
#include "v8/include/v8-array-buffer.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-external.h"
#include "v8/include/v8-function.h"
#include "v8/include/v8-initialization.h"
#include "v8/include/v8-json.h"
#include "v8/include/v8-locker.h"
#include "v8/include/v8-message.h"
#include "v8/include/v8-microtask-queue.h"
#include "v8/include/v8-object.h"
#include "v8/include/v8-primitive.h"
#include "v8/include/v8-promise.h"
#include "v8/include/v8-script.h"
#include "v8/include/v8-typed-array.h"
#include "v8/include/v8-value-serializer.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_app_paths.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_app_runtime.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_file_descriptors.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_file_system_bridge.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_message_port.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_network_request_bridge.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_os_bridge.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_udp_socket_bridge.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_worker_message.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_zlib_bridge.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_zlib_stream.h"
#include "xenon_overlay/chrome/browser/napi/js_native_api_v8.h"
#include "xenon_overlay/chrome/browser/napi/napi_loader.h"
#include "xenon_overlay/chrome/browser/napi/native_addon_event_loop.h"
#include "xenon_overlay/common/asar/archive.h"
#include "xenon_overlay/common/ipc/xenon_crypto_primitives.h"
#include "xenon_overlay/common/ipc/xenon_ipc_value_codec.h"
#include "xenon_overlay/common/ipc/xenon_privileged_schemes.h"
#include "xenon_overlay/common/ipc/xenon_runtime_platform.h"
#include "xenon_overlay/common/ipc/xenon_v8_statistics.h"
#include "xenon_overlay/common/ipc/xenon_x509_certificate.h"
#include "xenon_overlay/public/xenon_ipc_switches.h"
#include "xenon_overlay/resources/grit/xenon_resources.h"
#include "xenon_overlay/third_party/libuv/include/uv.h"

namespace xenon::ipc {

namespace {

constexpr int kMaxPackageMainDepth = 32;
constexpr int kMaxIpcValueDepth = 32;
constexpr uint64_t kMaxSafeInteger = 9007199254740991ULL;

void ThrowAppPathError(v8::Isolate* isolate,
                       const std::string& code,
                       const std::string& message) {
  auto error = v8::Exception::Error(gin::StringToV8(isolate, message));
  if (error.As<v8::Object>()
          ->Set(isolate->GetCurrentContext(), gin::StringToV8(isolate, "code"),
                gin::StringToV8(isolate, code))
          .IsNothing()) {
    return;
  }
  isolate->ThrowException(error);
}

// Node's v8.serialize writes views as host objects so pooled Buffers neither
// expose unrelated backing-store bytes nor inflate the serialized size.
class NodeValueSerializer : public v8::ValueSerializer::Delegate {
 public:
  NodeValueSerializer(v8::Isolate* isolate,
                      v8::Local<v8::Value> buffer_constructor,
                      v8::Local<v8::Function> data_view_byte_length)
      : isolate_(isolate),
        buffer_constructor_(buffer_constructor),
        data_view_byte_length_(data_view_byte_length),
        serializer_(isolate, this) {
    serializer_.SetTreatArrayBufferViewsAsHostObjects(true);
  }

  void ThrowDataCloneError(v8::Local<v8::String> message) override {
    isolate_->ThrowException(v8::Exception::Error(message));
  }

  v8::Maybe<bool> WriteHostObject(v8::Isolate* isolate,
                                  v8::Local<v8::Object> object) override {
    if (!object->IsArrayBufferView()) {
      ThrowDataCloneError(v8::String::NewFromUtf8Literal(
          isolate, "Unserializable host object"));
      return v8::Nothing<bool>();
    }
    auto context = isolate->GetCurrentContext();
    v8::Local<v8::Value> constructor;
    if (!object->Get(context, gin::StringToV8(isolate, "constructor"))
             .ToLocal(&constructor)) {
      return v8::Nothing<bool>();
    }
    // These tags are Node DefaultSerializer's stable ArrayBufferView indices.
    uint32_t tag;
    if (constructor->StrictEquals(buffer_constructor_)) {
      tag = 10;
    } else if (object->IsInt8Array()) {
      tag = 0;
    } else if (object->IsUint8Array()) {
      tag = 1;
    } else if (object->IsUint8ClampedArray()) {
      tag = 2;
    } else if (object->IsInt16Array()) {
      tag = 3;
    } else if (object->IsUint16Array()) {
      tag = 4;
    } else if (object->IsInt32Array()) {
      tag = 5;
    } else if (object->IsUint32Array()) {
      tag = 6;
    } else if (object->IsFloat32Array()) {
      tag = 7;
    } else if (object->IsFloat64Array()) {
      tag = 8;
    } else if (object->IsDataView()) {
      tag = 9;
    } else if (object->IsBigInt64Array()) {
      tag = 11;
    } else if (object->IsBigUint64Array()) {
      tag = 12;
    } else if (object->IsFloat16Array()) {
      tag = 13;
    } else {
      ThrowDataCloneError(v8::String::NewFromUtf8Literal(
          isolate, "Unserializable ArrayBufferView"));
      return v8::Nothing<bool>();
    }
    auto view = object.As<v8::ArrayBufferView>();
    // The C++ ByteLength API returns zero for an out-of-bounds DataView over a
    // resized buffer; Node's intrinsic getter throws instead.
    if (object->IsDataView() &&
        data_view_byte_length_->Call(context, object, 0, nullptr).IsEmpty()) {
      return v8::Nothing<bool>();
    }
    if (view->Buffer()->WasDetached()) {
      isolate->ThrowException(
          v8::Exception::TypeError(v8::String::NewFromUtf8Literal(
              isolate, "Cannot serialize detached ArrayBufferView")));
      return v8::Nothing<bool>();
    }
    const size_t size = view->ByteLength();
    if (!base::IsValueInRangeForNumericType<uint32_t>(size)) {
      ThrowDataCloneError(v8::String::NewFromUtf8Literal(
          isolate, "ArrayBufferView is too large to serialize"));
      return v8::Nothing<bool>();
    }
    std::vector<uint8_t> bytes(size);
    if (size && view->CopyContents(bytes.data(), size) != size) {
      ThrowDataCloneError(v8::String::NewFromUtf8Literal(
          isolate, "Cannot serialize detached ArrayBufferView"));
      return v8::Nothing<bool>();
    }
    serializer_.WriteUint32(tag);
    serializer_.WriteUint32(static_cast<uint32_t>(size));
    serializer_.WriteRawBytes(bytes.data(), size);
    return v8::Just(true);
  }

  v8::ValueSerializer& serializer() { return serializer_; }

 private:
  raw_ptr<v8::Isolate> isolate_;
  v8::Local<v8::Value> buffer_constructor_;
  v8::Local<v8::Function> data_view_byte_length_;
  v8::ValueSerializer serializer_;
};

// OS realpath/stat APIs cannot traverse an ASAR. Canonicalize the archive on
// disk, then resolve the member and ASAR links so equivalent CommonJS requests
// share one module record without changing __filename to an extracted file
// path.
bool NormalizeCommonJsPath(const base::FilePath& requested_path,
                           base::FilePath* normalized,
                           bool* is_directory = nullptr,
                           bool allow_missing_member = false) {
  // Node resolves dot segments lexically before realpath, including paths
  // through a missing directory or an ASAR. base::File deliberately rejects
  // ReferencesParent(), so passing a CJS "../file" directly to realpath would
  // reject an existing module. Work on filesystem components, not URL paths:
  // a Windows UNC share is part of the root and cannot be removed by "..".
  // The caller still checks the canonical result against the application root.
  if (!requested_path.IsAbsolute()) {
    return false;
  }
  const auto components =
      requested_path.NormalizePathSeparators().GetComponents();
  size_t root_components = 1;
  base::FilePath root(FILE_PATH_LITERAL("/"));
#if BUILDFLAG(IS_WIN)
  if (requested_path.IsNetwork()) {
    root_components = 3;
    if (components.size() >= 3 && components[1] == FILE_PATH_LITERAL("?") &&
        base::FilePath::CompareEqualIgnoreCase(components[2],
                                               FILE_PATH_LITERAL("UNC"))) {
      root_components = 5;
    }
    if (components.size() < root_components) {
      return false;
    }
    root = base::FilePath(components[0]);
    for (size_t i = 1; i < root_components; ++i) {
      root = root.Append(components[i]);
    }
    if (root_components == 3 &&
        (components[1] == FILE_PATH_LITERAL("?") ||
         components[1] == FILE_PATH_LITERAL(".")) &&
        components[2].size() == 2 && components[2].back() == L':') {
      root = root.AsEndingWithSeparator();
    }
  } else {
    root_components = 2;
    if (components.size() < root_components) {
      return false;
    }
    root = base::FilePath(components[0] + components[1]);
  }
#endif
  base::FilePath path = root;
  for (size_t i = root_components; i < components.size(); ++i) {
    if (components[i] == FILE_PATH_LITERAL(".")) {
      continue;
    }
    if (components[i] == FILE_PATH_LITERAL("..")) {
      if (path != root) {
        path = path.DirName();
      }
    } else {
      path = path.Append(components[i]);
    }
  }
  base::FilePath archive_path;
  base::FilePath relative_path;
  if (!asar::GetAsarArchivePath(path, &archive_path, &relative_path,
                                /*allow_root=*/true)) {
    if (!base::NormalizeFilePath(path, normalized)) {
      return false;
    }
    if (is_directory) {
      *is_directory = base::DirectoryExists(*normalized);
    }
    return true;
  }

  base::FilePath normalized_archive;
  if (!base::NormalizeFilePath(archive_path, &normalized_archive)) {
    return false;
  }
  base::FilePath member;
  const auto member_components = relative_path.GetComponents();
  for (size_t i = 0; i < member_components.size(); ++i) {
    const auto& component = member_components[i];
    if (component == FILE_PATH_LITERAL(".")) {
      continue;
    }
    if (component == FILE_PATH_LITERAL("..")) {
      if (member.empty()) {
        // require() resolves '..' lexically, including leaving an archive.
        // The caller still enforces the configured application module root.
        base::FilePath outside = normalized_archive.DirName();
        for (++i; i < member_components.size(); ++i) {
          outside = outside.Append(member_components[i]);
        }
        return NormalizeCommonJsPath(outside, normalized, is_directory,
                                     allow_missing_member);
      }
      member = member.DirName();
      if (member.value() == FILE_PATH_LITERAL(".")) {
        member.clear();
      }
    } else {
      member = member.Append(component);
    }
  }

  auto archive = asar::GetOrCreateAsarArchive(normalized_archive);
  bool directory = false;
  if (!archive) {
    return false;
  }
  base::FilePath resolved_member;
  if (archive->ResolvePath(member, &resolved_member)) {
    if (!archive->StatPath(resolved_member, nullptr, &directory)) {
      return false;
    }
    member = std::move(resolved_member);
  } else if (!allow_missing_member) {
    return false;
  }
  *normalized =
      member.empty() ? normalized_archive : normalized_archive.Append(member);
  if (is_directory) {
    *is_directory = directory;
  }
  return true;
}

bool IsCommonJsDirectory(const base::FilePath& path) {
  base::FilePath normalized;
  bool is_directory = false;
  return NormalizeCommonJsPath(path, &normalized, &is_directory) &&
         is_directory;
}

bool ReadCommonJsFile(const base::FilePath& path, std::string* source) {
  base::FilePath normalized;
  bool is_directory = false;
  if (!NormalizeCommonJsPath(path, &normalized, &is_directory) ||
      is_directory) {
    return false;
  }
  base::FilePath archive_path;
  base::FilePath member;
  if (!asar::GetAsarArchivePath(normalized, &archive_path, &member)) {
    return base::ReadFileToString(normalized, source);
  }
  auto archive = asar::GetOrCreateAsarArchive(archive_path);
  return archive && archive->ReadFile(member, source);
}

std::optional<base::FilePath> GetNativeCommonJsPath(const base::FilePath& path,
                                                    std::string* error) {
  base::FilePath archive_path;
  base::FilePath member;
  if (!asar::GetAsarArchivePath(path, &archive_path, &member)) {
    return path;
  }
  auto archive = asar::GetOrCreateAsarArchive(archive_path);
  asar::Archive::FileInfo info;
  if (!archive || !archive->GetFileInfo(member, &info)) {
    *error = "Cannot find native module: " + path.AsUTF8Unsafe();
    return std::nullopt;
  }
  if (!info.unpacked) {
    *error = "ERR_NOT_SUPPORTED: Native modules in ASAR must be unpacked: " +
             path.AsUTF8Unsafe();
    return std::nullopt;
  }
  base::FilePath unpacked_path;
  base::FilePath normalized;
  if (!archive->GetUnpackedPath(member, &unpacked_path) ||
      !base::NormalizeFilePath(unpacked_path, &normalized) ||
      base::DirectoryExists(normalized)) {
    *error = "Cannot find unpacked native module: " + path.AsUTF8Unsafe();
    return std::nullopt;
  }
  return normalized;
}

bool ConvertV8ToValue(v8::Isolate* isolate,
                      v8::Local<v8::Context> context,
                      v8::Local<v8::Value> value,
                      base::Value* output,
                      std::string* error,
                      int depth,
                      bool preserve_nested_binary = false) {
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
    *output = base::Value(base::Value::BlobStorage(slice.begin(), slice.end()));
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
                            depth + 1, preserve_nested_binary)) {
        return false;
      }
      list.Append(std::move(converted));
    }
    *output = base::Value(std::move(list));
    return true;
  }
  // Native addon arguments carry typed binary payloads inside wire envelopes.
  // JSON would turn their nested Uint8Arrays into numeric property objects.
  // Keep the existing JSON/toJSON behavior for non-native bridge callers.
  if (preserve_nested_binary && value->IsObject()) {
    const auto object = value.As<v8::Object>();
    v8::Local<v8::Array> keys;
    if (!object
             ->GetOwnPropertyNames(context, v8::ONLY_ENUMERABLE,
                                   v8::KeyConversionMode::kConvertToString)
             .ToLocal(&keys)) {
      *error = "Failed to enumerate native argument properties";
      return false;
    }
    base::DictValue dict;
    for (uint32_t i = 0; i < keys->Length(); ++i) {
      v8::Local<v8::Value> key;
      v8::Local<v8::Value> item;
      if (!keys->Get(context, i).ToLocal(&key) ||
          !object->Get(context, key).ToLocal(&item)) {
        *error = "Failed to read native argument property";
        return false;
      }
      if (item->IsUndefined() || item->IsFunction() || item->IsSymbol()) {
        continue;
      }
      std::string name;
      base::Value converted;
      if (!gin::ConvertFromV8(isolate, key, &name) ||
          !ConvertV8ToValue(isolate, context, item, &converted, error,
                            depth + 1, true)) {
        return false;
      }
      dict.Set(name, std::move(converted));
    }
    *output = base::Value(std::move(dict));
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
  // Generated-file fallback is for tests that do not initialize a bundle.
  // Never execute the source entry: its includes are expanded at build time.
  if (ui::ResourceBundle::HasSharedInstance()) {
    std::string resource =
        ui::ResourceBundle::GetSharedInstance().LoadDataResourceString(
            IDR_XENON_IPC_MAIN_BOOTSTRAP_JS);
    if (!resource.empty()) {
      return resource;
    }
  }
  base::FilePath root;
  if (base::PathService::Get(base::DIR_EXE, &root)) {
    base::FilePath file = root.AppendASCII("gen")
                              .AppendASCII("xenon_overlay")
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
  return out.empty() ? full.substr(0, std::min<size_t>(full.size(), 1024))
                     : out;
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
         base::FilePath::CompareEqualIgnoreCase(normalized.DirName().value(),
                                                executable_directory->value());
}

const char* BuiltinModuleGlobal(const std::string& request) {
  static constexpr auto kBuiltinModules =
      base::MakeFixedFlatMap<std::string_view, const char*>({
          {"assert", "__xenonAssert"},
          {"async_hooks", "__xenonAsyncHooks"},
          {"diagnostics_channel", "__xenonDiagnosticsChannel"},
          {"buffer", "__xenonBuffer"},
          {"child_process", "__xenonChildProcess"},
          {"console", "__xenonConsole"},
          {"constants", "__xenonConstants"},
          {"crypto", "__xenonCrypto"},
          {"dns", "__xenonDns"},
          {"dgram", "__xenonDgram"},
          {"electron", "__xenonElectron"},
          {"events", "__xenonEvents"},
          {"fs", "__xenonFs"},
          {"fs/promises", "__xenonFsPromises"},
          {"http", "__xenonHttp"},
          {"http2", "__xenonHttp2"},
          {"https", "__xenonHttps"},
          {"module", "__xenonModule"},
          {"net", "__xenonNet"},
          {"node:process", "process"},
          {"process", "process"},
          {"node:assert", "__xenonAssert"},
          {"node:async_hooks", "__xenonAsyncHooks"},
          {"node:diagnostics_channel", "__xenonDiagnosticsChannel"},
          {"node:buffer", "__xenonBuffer"},
          {"node:child_process", "__xenonChildProcess"},
          {"node:console", "__xenonConsole"},
          {"node:constants", "__xenonConstants"},
          {"node:crypto", "__xenonCrypto"},
          {"node:dns", "__xenonDns"},
          {"node:dgram", "__xenonDgram"},
          {"node:events", "__xenonEvents"},
          {"node:fs", "__xenonFs"},
          {"node:fs/promises", "__xenonFsPromises"},
          {"node:http", "__xenonHttp"},
          {"node:http2", "__xenonHttp2"},
          {"node:https", "__xenonHttps"},
          {"node:module", "__xenonModule"},
          {"node:net", "__xenonNet"},
          {"node:original-fs", "__xenonOriginalFs"},
          {"node:original-fs/promises", "__xenonOriginalFsPromises"},
          {"node:os", "__xenonOs"},
          {"node:path", "__xenonPath"},
          {"node:path/posix", "__xenonPathPosix"},
          {"node:path/win32", "__xenonPathWin32"},
          {"node:perf_hooks", "__xenonPerfHooks"},
          {"node:querystring", "__xenonQuerystring"},
          {"node:readline", "__xenonReadline"},
          {"node:stream", "__xenonStream"},
          {"node:stream/promises", "__xenonStreamPromises"},
          {"node:stream/web", "__xenonStreamWeb"},
          {"node:string_decoder", "__xenonStringDecoder"},
          {"node:timers", "__xenonTimers"},
          {"node:timers/promises", "__xenonTimersPromises"},
          {"node:tls", "__xenonTls"},
          {"node:tty", "__xenonTty"},
          {"node:url", "__xenonUrl"},
          {"node:util", "__xenonUtil"},
          {"node:util/types", "__xenonUtilTypes"},
          {"node:v8", "__xenonV8"},
          {"node:worker_threads", "__xenonWorkerThreads"},
          {"node:zlib", "__xenonZlib"},
          {"os", "__xenonOs"},
          {"original-fs", "__xenonOriginalFs"},
          {"original-fs/promises", "__xenonOriginalFsPromises"},
          {"path", "__xenonPath"},
          {"path/posix", "__xenonPathPosix"},
          {"path/win32", "__xenonPathWin32"},
          {"perf_hooks", "__xenonPerfHooks"},
          {"querystring", "__xenonQuerystring"},
          {"readline", "__xenonReadline"},
          {"stream", "__xenonStream"},
          {"stream/promises", "__xenonStreamPromises"},
          {"stream/web", "__xenonStreamWeb"},
          {"string_decoder", "__xenonStringDecoder"},
          {"timers", "__xenonTimers"},
          {"timers/promises", "__xenonTimersPromises"},
          {"tls", "__xenonTls"},
          {"tty", "__xenonTty"},
          {"url", "__xenonUrl"},
          {"util", "__xenonUtil"},
          {"util/types", "__xenonUtilTypes"},
          {"v8", "__xenonV8"},
          {"worker_threads", "__xenonWorkerThreads"},
          {"zlib", "__xenonZlib"},
      });

  auto found = kBuiltinModules.find(request);
  return found == kBuiltinModules.end() ? nullptr : found->second;
}

}  // namespace

// Each main/Worker/Utility container owns its isolate. Native Promise hooks
// therefore cannot replace Blink's hooks, and work without V8's optional
// JavaScript Promise-hook build feature. Keep the lookup on the owning thread;
// no embedder-data slot or continuation-preserved data is appropriated.
struct XenonIpcMainContainer::AsyncContextHooks
    : public base::RefCounted<AsyncContextHooks> {
  AsyncContextHooks(v8::Isolate* isolate,
                    v8::Local<v8::Context> context,
                    v8::Local<v8::Function> init,
                    v8::Local<v8::Function> before,
                    v8::Local<v8::Function> after)
      : isolate(isolate),
        context(isolate, context),
        init(isolate, init),
        before(isolate, before),
        after(isolate, after),
        next(head) {
    head = this;
    isolate->SetPromiseHook(&Dispatch);
  }

  void Detach() {
    if (!isolate) {
      return;
    }
    // Stop new callbacks before releasing any of their persistent handles.
    isolate->SetPromiseHook(nullptr);
    if (head == this) {
      head = next;
    } else {
      AsyncContextHooks* previous = head;
      while (previous && previous->next != this) {
        previous = previous->next;
      }
      CHECK(previous);
      previous->next = next;
    }
    next = nullptr;
    init.Reset();
    before.Reset();
    after.Reset();
    context.Reset();
    isolate = nullptr;
  }

  static void Dispatch(v8::PromiseHookType type,
                       v8::Local<v8::Promise> promise,
                       v8::Local<v8::Value> parent) {
    if (type == v8::PromiseHookType::kResolve) {
      return;
    }
    v8::Isolate* isolate = v8::Isolate::GetCurrent();
    AsyncContextHooks* current = head;
    while (current && current->isolate != isolate) {
      current = current->next;
    }
    if (!current || current->dispatching || isolate->IsExecutionTerminating()) {
      return;
    }
    // Shutdown may detach while JS is on the stack. Keep the guard alive and
    // take local handles before calling JS; never re-read detached handles.
    scoped_refptr<AsyncContextHooks> state(current);
    base::AutoReset<bool> guard(&state->dispatching, true);
    v8::HandleScope handle_scope(isolate);
    v8::Local<v8::Context> context = state->context.Get(isolate);
    v8::Local<v8::Function> callback;
    switch (type) {
      case v8::PromiseHookType::kInit:
        callback = state->init.Get(isolate);
        break;
      case v8::PromiseHookType::kBefore:
        callback = state->before.Get(isolate);
        break;
      case v8::PromiseHookType::kAfter:
        callback = state->after.Get(isolate);
        break;
      case v8::PromiseHookType::kResolve:
        return;
    }
    v8::Context::Scope context_scope(context);
    v8::MicrotasksScope microtasks(context,
                                   v8::MicrotasksScope::kDoNotRunMicrotasks);
    v8::TryCatch try_catch(isolate);
    // Match V8's context hooks: report hook exceptions without replacing the
    // Promise reaction's exception or propagating into the running job.
    try_catch.SetVerbose(true);
    v8::Local<v8::Value> argv[] = {promise, parent};
    if (callback
            ->Call(context, v8::Undefined(isolate),
                   type == v8::PromiseHookType::kInit ? 2 : 1, argv)
            .IsEmpty() &&
        !try_catch.HasTerminated()) {
      LOG(ERROR) << "ipcMain async context hook threw";
    }
  }

 private:
  friend class base::RefCounted<AsyncContextHooks>;
  ~AsyncContextHooks() { DCHECK(!isolate); }

  static thread_local AsyncContextHooks* head;
  raw_ptr<v8::Isolate> isolate;
  v8::Global<v8::Context> context;
  v8::Global<v8::Function> init;
  v8::Global<v8::Function> before;
  v8::Global<v8::Function> after;
  raw_ptr<AsyncContextHooks> next;
  bool dispatching = false;
};

thread_local XenonIpcMainContainer::AsyncContextHooks*
    XenonIpcMainContainer::AsyncContextHooks::head = nullptr;

struct XenonIpcMainContainer::PromiseReplyContext {
  base::WeakPtr<XenonIpcMainContainer> owner;
  InvokeCallback callback;
  std::string endpoint_id;
  bool serialized = false;
  bool preserve_nested_binary = false;
};

struct XenonIpcMainContainer::RendererEndpoint {
  int32_t process_id;
  int32_t frame_id;
  int32_t window_id;
  std::string document_url;
  bool is_main_frame;
  mojom::IpcServiceWorkerInfoPtr worker_info;
  mojo::Remote<xenon::ipc::mojom::IpcRenderer> remote;
};

XenonIpcMainContainer::XenonIpcMainContainer() = default;

XenonIpcMainContainer::~XenonIpcMainContainer() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  Shutdown();
  CHECK(native_shutdown_complete_)
      << "Destroying a busy main runtime: " << DescribeNativeShutdown();
}

bool XenonIpcMainContainer::Initialize() {
  return InitializeInternal(std::nullopt);
}

bool XenonIpcMainContainer::Initialize(EmbeddedMainModule main_module) {
  return InitializeInternal(std::move(main_module));
}

bool XenonIpcMainContainer::Initialize(AppMainModule main_module) {
  return InitializeInternal(std::nullopt, std::move(main_module));
}

void XenonIpcMainContainer::SetNativeAddonHooks(NativeAddonHooks hooks) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  CHECK(!initialized_);
  native_addon_hooks_ = std::move(hooks);
}

void XenonIpcMainContainer::SetNetworkLoaderFactory(
    scoped_refptr<network::SharedURLLoaderFactory> factory) {
  network_loader_factory_ = std::move(factory);
}

void XenonIpcMainContainer::SetElectronNetworkHooks(
    ElectronNetEngine::FactoryProvider factory,
    ElectronNetEngine::OnlineProvider online) {
  electron_net_online_ = std::move(online);
  electron_net_ = std::make_unique<ElectronNetEngine>(
      std::move(factory),
      base::BindRepeating(&XenonIpcMainContainer::DispatchElectronNetEvent,
                          weak_factory_.GetWeakPtr()));
}

void XenonIpcMainContainer::SetWindowHooks(WindowHooks hooks) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  window_hooks_ = std::move(hooks);
}

void XenonIpcMainContainer::SetAppExitHandler(
    base::RepeatingCallback<void(int)> handler) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  app_exit_handler_ = std::move(handler);
}

void XenonIpcMainContainer::SetMainEvaluationCompleteHandler(
    base::RepeatingClosure handler) {
  main_evaluation_complete_handler_ = std::move(handler);
}

void XenonIpcMainContainer::SetHostPaths(base::FilePath user_data,
                                         base::FilePath crash_dumps) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  CHECK(!initialized_);
  host_user_data_path_ = std::move(user_data);
  host_crash_dumps_path_ = std::move(crash_dumps);
}

void XenonIpcMainContainer::SetChildProcessCaller(
    base::RepeatingCallback<base::Value(const base::DictValue&)> caller) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  child_process_caller_ = std::move(caller);
}

void XenonIpcMainContainer::SetNetPipeSender(
    base::RepeatingCallback<void(const std::string&, base::Value)> sender) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  net_pipe_sender_ = std::move(sender);
}

bool XenonIpcMainContainer::InitializeInternal(
    std::optional<EmbeddedMainModule> main_module,
    std::optional<AppMainModule> app_module) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (shutting_down_) {
    startup_error_ = "Main JavaScript container is shutting down";
    return false;
  }
  if (initialized_) {
    return true;
  }

  startup_error_.clear();
  app_paths_.reset();
  embedded_main_source_.reset();
  main_script_path_.clear();
  module_root_.clear();
  app_path_.clear();
  executable_path_.clear();
  resources_directory_.clear();
  working_directory_.clear();
  app_argv_.clear();
  is_packaged_ = false;
  app_name_ = "Application";
  app_version_ = "0.0.0";
  default_user_agent_.clear();
  renderer_url_mappings_.clear();
  renderer_base_url_.clear();
  if (worker_options_) {
    main_script_path_ = worker_options_->filename;
    app_path_ = worker_options_->app_path;
    module_root_ = app_path_;
    executable_path_ = worker_options_->executable_path;
    resources_directory_ = worker_options_->resources_directory;
    working_directory_ = worker_options_->working_directory;
    app_argv_ = worker_options_->argv;
    base::FilePath normalized;
    if (worker_options_->eval) {
      // Eval has a virtual CommonJS filename; ordinary OS realpath requires
      // the file to exist, while the inherited application root is canonical.
      normalized = app_path_.AppendASCII("[worker eval].cjs");
    } else if (!NormalizeCommonJsPath(main_script_path_, &normalized)) {
      startup_error_ = "ERR_MODULE_NOT_FOUND: Worker entry does not exist: " +
                       main_script_path_.AsUTF8Unsafe();
      return false;
    }
    main_script_path_ = std::move(normalized);
    if (!module_root_.IsParent(main_script_path_)) {
      startup_error_ =
          "ERR_ACCESS_DENIED: Worker entry is outside its application";
      return false;
    }
    if (worker_options_->eval) {
      embedded_main_source_ = worker_options_->source;
    }
  } else if (main_module) {
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
    if (NormalizeCommonJsPath(main_script_path_, &normalized_main_script,
                              nullptr, /*allow_missing_member=*/true)) {
      main_script_path_ = std::move(normalized_main_script);
    } else if (!base::PathExists(main_script_path_) &&
               !base::IsLink(main_script_path_)) {
      // Embedded source need not have a physical file. Canonicalize its
      // existing parent too, so Windows short names/casing do not produce a
      // false root escape. A dangling symlink is not a virtual source path.
      base::FilePath normalized_parent;
      if (base::NormalizeFilePath(main_script_path_.DirName(),
                                  &normalized_parent) &&
          base::DirectoryExists(normalized_parent)) {
        main_script_path_ =
            normalized_parent.Append(main_script_path_.BaseName());
      }
    }
    if (NormalizeCommonJsPath(app_path_, &normalized_app_path)) {
      app_path_ = std::move(normalized_app_path);
    }
    if (!app_path_.IsAbsolute() || (main_script_path_ != app_path_ &&
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
    resources_directory_ = std::move(main_module->resources_directory);
    working_directory_ = std::move(main_module->working_directory);
    app_argv_ = std::move(main_module->app_argv);
    is_packaged_ = main_module->is_packaged.value_or(
        !renderer_base_url_.empty() || !renderer_url_mappings_.empty());
    embedded_main_source_ = std::move(main_module->source);
  } else if (app_module) {
    if (!ResolveApplicationMain(std::move(*app_module))) {
      return false;
    }
  } else if (!ResolveConfiguredMainScript()) {
    return false;
  }

  if (!ResolveAppExecutable(executable_path_, &executable_path_,
                            &startup_error_)) {
    return false;
  }
  if (app_argv_.empty()) {
    app_argv_.push_back(executable_path_.AsUTF8Unsafe());
  }
  for (const auto& argument : app_argv_) {
    if (argument.find('\0') != std::string::npos) {
      startup_error_ = "Application arguments must not contain NUL";
      return false;
    }
  }
  if (resources_directory_.empty()) {
    const auto executable_directory = executable_path_.DirName();
    resources_directory_ =
        executable_directory.BaseName().value() == FILE_PATH_LITERAL("MacOS")
            ? executable_directory.DirName().AppendASCII("Resources")
            : executable_directory.AppendASCII("resources");
  }
  if (working_directory_.empty() &&
      !base::GetCurrentDirectory(&working_directory_)) {
    startup_error_ = "Application working directory is unavailable";
    return false;
  }
  if (!resources_directory_.IsAbsolute() || !working_directory_.IsAbsolute() ||
      !base::DirectoryExists(working_directory_)) {
    startup_error_ =
        "Application resources and working directories are invalid";
    return false;
  }

  XenonAppPaths::StoragePathCallback storage_path;
  if (!worker_options_ && !utility_options_ && window_hooks_.call) {
    storage_path = base::BindRepeating(
        [](XenonIpcMainContainer* self, std::string name,
           std::optional<base::FilePath> path)
            -> base::expected<base::FilePath, XenonAppPaths::Error> {
          auto request = base::DictValue()
                             .Set("method", path ? "set" : "get")
                             .Set("name", std::move(name));
          if (path) {
            request.Set("path", path->AsUTF8Unsafe());
          }
          base::Value result;
          std::string error;
          bool success;
          {
            v8::Unlocker unlocker(self->isolate_);
            success = self->window_hooks_.call.Run(
                0, "electron-app-storage", base::Value(std::move(request)),
                &result, &error);
          }
          if (!success || !error.empty() || !result.is_string()) {
            const auto separator = error.find(':');
            return base::unexpected(XenonAppPaths::Error{
                separator == std::string::npos ? "ERR_FAILED"
                                               : error.substr(0, separator),
                error.empty() ? "Application storage path request failed"
                              : error});
          }
          auto resolved = base::FilePath::FromUTF8Unsafe(result.GetString());
          if (!resolved.IsAbsolute()) {
            return base::unexpected(XenonAppPaths::Error{
                "ERR_INVALID_STATE",
                "Host returned an invalid application storage path"});
          }
          return resolved;
        },
        base::Unretained(this));
  }
  app_paths_ = std::make_unique<XenonAppPaths>(
      app_path_, executable_path_, app_name_, host_user_data_path_,
      host_crash_dumps_path_, std::move(storage_path));
  if (!worker_options_) {
    const auto* command_line = base::CommandLine::ForCurrentProcess();
    const bool owns_process_directory =
        command_line->GetSwitchValueASCII(::switches::kProcessType) ==
            ::switches::kUtilityProcess &&
        (command_line->HasSwitch(switches::kIsolatedElectronService) ||
         command_line->HasSwitch(switches::kUtilityProcess));
    file_descriptors_ = base::MakeRefCounted<XenonFileDescriptors>(
        working_directory_, owns_process_directory);
    if (owns_process_directory && file_descriptors_->ChangeWorkingDirectory(
                                      working_directory_.AsUTF8Unsafe()) != 0) {
      startup_error_ = "Unable to set application process working directory";
      return false;
    }
  }

  if (!gin::IsolateHolder::Initialized()) {
#if defined(V8_USE_EXTERNAL_STARTUP_DATA)
    gin::V8Initializer::LoadV8Snapshot();
#endif
    gin::IsolateHolder::Initialize(gin::IsolateHolder::kNonStrictMode,
                                   gin::ArrayBufferAllocator::SharedInstance());
  }

  if (worker_params_) {
    // Supported host architectures have downward-growing native stacks. Keep
    // a reserve for unwinding and native calls after V8 raises stack overflow.
    uintptr_t stack_anchor = 0;
    const auto limit =
        reinterpret_cast<uintptr_t>(&stack_anchor) -
        static_cast<size_t>(worker_options_->stack_mb * 1024 * 1024) +
        128 * 1024;
    worker_params_->constraints.set_stack_limit(
        reinterpret_cast<uint32_t*>(limit));
    isolate_holder_ = std::make_unique<gin::IsolateHolder>(
        base::SingleThreadTaskRunner::GetCurrentDefault(),
        gin::IsolateHolder::kUseLocker,
        gin::IsolateHolder::IsolateType::kUtility, std::move(worker_params_));
  } else {
    isolate_holder_ = std::make_unique<gin::IsolateHolder>(
        base::SingleThreadTaskRunner::GetCurrentDefault(),
        gin::IsolateHolder::kUseLocker,
        gin::IsolateHolder::IsolateType::kUtility);
  }
  isolate_ = isolate_holder_->isolate();
  CHECK(!async_context_hooks_);
  if (!isolate_) {
    startup_error_ = "Failed to create the main JavaScript isolate";
    return false;
  }
  isolate_->SetPromiseRejectCallback(&PromiseRejectCallback);

  {
    v8::Locker locker(isolate_);
    v8::Isolate::Scope isolate_scope(isolate_);
    v8::HandleScope handle_scope(isolate_);
    if (worker_control_) {
      worker_control_->PublishIsolate(isolate_);
    }
    if (isolate_->IsExecutionTerminating()) {
      return false;
    }
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
        ->Set(context,
              v8::String::NewFromUtf8Literal(isolate_, "__xenonResourcesPath"),
              gin::StringToV8(isolate_, resources_directory_.AsUTF8Unsafe()))
        .Check();
    global
        ->Set(
            context,
            v8::String::NewFromUtf8Literal(isolate_, "__xenonWorkingDirectory"),
            gin::StringToV8(isolate_, working_directory_.AsUTF8Unsafe()))
        .Check();
    global
        ->Set(context, gin::StringToV8(isolate_, "__xenonFsConstants"),
              ValueToV8(base::Value(XenonFileDescriptors::Constants()))
                  .ToLocalChecked())
        .Check();
    global
        ->Set(context, v8::String::NewFromUtf8Literal(isolate_, "__xenonArgv"),
              gin::ConvertToV8(isolate_, app_argv_))
        .Check();
    global
        ->Set(context,
              v8::String::NewFromUtf8Literal(isolate_, "__xenonIsPackaged"),
              v8::Boolean::New(isolate_, is_packaged_))
        .Check();
    global
        ->Set(context, v8::String::NewFromUtf8Literal(isolate_, "__xenonPid"),
              v8::Integer::NewFromUnsigned(isolate_, base::GetCurrentProcId()))
        .Check();
    global
        ->Set(
            context,
            v8::String::NewFromUtf8Literal(isolate_, "__xenonPlatform"),
            v8::String::NewFromUtf8(isolate_, PlatformName()).ToLocalChecked())
        .Check();
    global
        ->Set(context, v8::String::NewFromUtf8Literal(isolate_, "__xenonArch"),
              v8::String::NewFromUtf8(isolate_, ArchitectureName())
                  .ToLocalChecked())
        .Check();
    global
        ->Set(context,
              v8::String::NewFromUtf8Literal(isolate_, "__xenonEndianness"),
              v8::String::NewFromUtf8(isolate_, EndiannessName())
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
                v8::String::NewFromUtf8(isolate_,
                                        renderer_url_mappings_[i].first.c_str())
                    .ToLocalChecked())
          .Check();
      mapping
          ->Set(context,
                v8::String::NewFromUtf8Literal(isolate_, "targetBaseUrl"),
                v8::String::NewFromUtf8(
                    isolate_, renderer_url_mappings_[i].second.c_str())
                    .ToLocalChecked())
          .Check();
      renderer_url_mappings->Set(context, static_cast<uint32_t>(i), mapping)
          .Check();
    }
    global
        ->Set(context,
              v8::String::NewFromUtf8Literal(isolate_,
                                             "__xenonRendererUrlMappings"),
              renderer_url_mappings)
        .Check();
    global
        ->Set(
            context,
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
    v8::Local<v8::Object> buffer_limits = v8::Object::New(isolate_);
    buffer_limits
        ->Set(context, gin::StringToV8(isolate_, "MAX_LENGTH"),
              v8::Number::New(isolate_,
                              static_cast<double>(v8::Uint8Array::kMaxLength)))
        .Check();
    buffer_limits
        ->Set(context, gin::StringToV8(isolate_, "MAX_STRING_LENGTH"),
              v8::Integer::New(isolate_, v8::String::kMaxLength))
        .Check();
    global
        ->Set(context, gin::StringToV8(isolate_, "__xenonBufferLimits"),
              buffer_limits)
        .Check();
    base::Value environment_values;
    if (worker_options_) {
      environment_values = base::Value(worker_options_->environment.Clone());
    } else if (utility_options_) {
      environment_values = base::Value(utility_options_->environment.Clone());
    } else {
      auto snapshot = GetProcessEnvironment();
      if (!snapshot->success) {
        startup_error_ =
            "Failed to read process environment: " + snapshot->error;
        return false;
      }
      environment_values = std::move(snapshot->value);
    }
    global
        ->Set(context, v8::String::NewFromUtf8Literal(isolate_, "__xenonEnv"),
              ValueToV8(environment_values).ToLocalChecked())
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
    bind_func("__xenonCreateRequire",
              &XenonIpcMainContainer::NativeCreateRequire);
    bind_func("__xenonProcessDirectory",
              &XenonIpcMainContainer::NativeProcessDirectory);
    bind_func("__xenonIsBuiltinModule",
              &XenonIpcMainContainer::NativeIsBuiltinModule);
    bind_func("__xenonIsPromise", &XenonIpcMainContainer::NativeIsPromise);
    bind_func("__xenonCanParseURL", &XenonIpcMainContainer::NativeCanParseURL);
    bind_func("__xenonIsNativeError",
              &XenonIpcMainContainer::NativeIsNativeError);
    bind_func("__xenonUtilType", &XenonIpcMainContainer::NativeUtilType);
    bind_func("__xenonV8Serialize", &XenonIpcMainContainer::NativeV8Serialize);
    bind_func("__xenonModuleHooksChanged",
              &XenonIpcMainContainer::NativeModuleHooksChanged);
    bind_func("__xenonDefaultModuleResolve",
              &XenonIpcMainContainer::NativeDefaultModuleResolve);
    bind_func("__xenonDefaultModuleLoad",
              &XenonIpcMainContainer::NativeDefaultModuleLoad);
    bind_func("__xenonStructuredClone",
              &XenonIpcMainContainer::NativeStructuredClone);
    bind_func("__xenonIsDataCloneError",
              &XenonIpcMainContainer::NativeIsDataCloneError);
    bind_func("__xenonInstallAsyncContextHooks",
              &XenonIpcMainContainer::NativeInstallAsyncContextHooks);
    bind_func("__xenonGetPath", &XenonIpcMainContainer::NativeGetPath);
    bind_func("__xenonSetPath", &XenonIpcMainContainer::NativeSetPath);
    bind_func("__xenonSetAppLogsPath",
              &XenonIpcMainContainer::NativeSetAppLogsPath);
    bind_func("__xenonNetworkInterfaces",
              &XenonIpcMainContainer::NativeNetworkInterfaces);
    bind_func("__xenonOsCall", &XenonIpcMainContainer::NativeOsCall);
    bind_func("__xenonCrashReporterCall",
              &XenonIpcMainContainer::NativeCrashReporterCall);
    bind_func("__xenonRegisterSchemesAsPrivileged",
              &XenonIpcMainContainer::NativeRegisterSchemesAsPrivileged);
    bind_func("__xenonSendToRenderer",
              &XenonIpcMainContainer::NativeSendToRenderer);
    bind_func("__xenonNetSend", &XenonIpcMainContainer::NativeNetSend);
    bind_func("__xenonDgramCall", &XenonIpcMainContainer::NativeDgramCall);
    bind_func("__xenonChildProcessCall",
              &XenonIpcMainContainer::NativeChildProcessCall);
    bind_func("__xenonFsCall", &XenonIpcMainContainer::NativeFsCall);
    bind_func("__xenonFsCallAsync", &XenonIpcMainContainer::NativeFsCallAsync);
    bind_func("__xenonCryptoCipher",
              &XenonIpcMainContainer::NativeCryptoCipher);
    bind_func("__xenonCryptoDigest",
              &XenonIpcMainContainer::NativeCryptoDigest);
    bind_func("__xenonCryptoRandom",
              &XenonIpcMainContainer::NativeCryptoRandom);
    InstallTimingSafeEqual(isolate_, context, global,
                           "__xenonCryptoTimingSafeEqual");
    InstallHeapCodeStatistics(isolate_, context, global,
                              "__xenonGetHeapCodeStatistics");
    InstallX509CertificateBridge(isolate_, context, global,
                                 "__xenonX509CertificateCall");
    bind_func("__xenonZlibCall", &XenonIpcMainContainer::NativeZlibCall);
    bind_func("__xenonZlibStreamCall",
              &XenonIpcMainContainer::NativeZlibStreamCall);
    bind_func("__xenonHttpRequest", &XenonIpcMainContainer::NativeHttpRequest);
    bind_func("__xenonHttpAbort", &XenonIpcMainContainer::NativeHttpAbort);
    bind_func("__xenonElectronNetStart",
              &XenonIpcMainContainer::NativeElectronNetStart);
    bind_func("__xenonElectronNetControl",
              &XenonIpcMainContainer::NativeElectronNetControl);
    bind_func("__xenonElectronNetAuth",
              &XenonIpcMainContainer::NativeElectronNetAuth);
    bind_func("__xenonElectronNetOnline",
              &XenonIpcMainContainer::NativeElectronNetOnline);
    if (!worker_control_) {
      bind_func("__xenonCreateBrowserWindow",
                &XenonIpcMainContainer::NativeCreateBrowserWindow);
      bind_func("__xenonLoadBrowserWindowURL",
                &XenonIpcMainContainer::NativeLoadBrowserWindowURL);
      bind_func("__xenonSetBrowserWindowVisible",
                &XenonIpcMainContainer::NativeSetBrowserWindowVisible);
      bind_func("__xenonBrowserWindowCall",
                &XenonIpcMainContainer::NativeBrowserWindowCall);
      bind_func("__xenonBrowserWindowCallAsync",
                &XenonIpcMainContainer::NativeBrowserWindowCallAsync);
      bind_func("__xenonCloseBrowserWindow",
                &XenonIpcMainContainer::NativeCloseBrowserWindow);
      bind_func("__xenonExitApp", &XenonIpcMainContainer::NativeExitApp);
      bind_func("__xenonCanExitApp", &XenonIpcMainContainer::NativeCanExitApp);
    }
    bind_func("__xenonNativeInvokeExport",
              &XenonIpcMainContainer::NativeInvokeExport);
    bind_func("__xenonNativeDescribeExport",
              &XenonIpcMainContainer::NativeDescribeExport);
    bind_func("__xenonNativeConstructExport",
              &XenonIpcMainContainer::NativeConstructExport);
    bind_func("__xenonNativeInvokeInstance",
              &XenonIpcMainContainer::NativeInvokeInstance);
    bind_func("__xenonSetTimerReferenced",
              &XenonIpcMainContainer::NativeSetTimerReferenced);
    if (!InitializeWorkerBindings(context)) {
      startup_error_ = "Failed to initialize Worker bindings";
      return false;
    }
    if (!InitializeUtilityBindings(context)) {
      startup_error_ = "Failed to initialize utilityProcess bindings";
      return false;
    }
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

  if (!worker_control_) {
    ScopedV8Context scope(isolate_, context_);
    message_ports_ = std::make_unique<MessagePortRuntime>(
        isolate_, context_.Get(isolate_),
        base::BindRepeating(
            [](XenonIpcMainContainer* self, const std::string& endpoint) {
              auto it = self->renderers_.find(endpoint);
              return !self->shutting_down_ && it != self->renderers_.end() &&
                     it->second.remote.is_bound() && !it->second.worker_info;
            },
            base::Unretained(this)),
        base::BindRepeating(
            [](XenonIpcMainContainer* self, const std::string& endpoint,
               const std::string& channel, blink::TransferableMessage message) {
              auto it = self->renderers_.find(endpoint);
              if (!self->shutting_down_ && it != self->renderers_.end() &&
                  !it->second.worker_info) {
                it->second.remote->DispatchPostMessage(channel,
                                                       std::move(message));
              }
            },
            base::Unretained(this)));
    if (!message_ports_->Install()) {
      startup_error_ = "Failed to install MessagePortMain transport";
      return false;
    }
    if (utility_options_) {
      message_ports_->SetExceptionHandler(base::BindRepeating(
          [](XenonIpcMainContainer* self, v8::Local<v8::Value> exception) {
            self->ReportNodeException(exception);
          },
          base::Unretained(this)));
    }
  }
  if (!RunBootstrap()) {
    return false;
  }
  if (!InstallUtilityParentPort()) {
    return false;
  }

  InitializeModuleLoaders();

  if (!MaybeLoadConfiguredMainScript()) {
    return false;
  }
  initialized_ = true;
  LOG(INFO) << "Xenon ipcMain container initialized in Utility process";
  return true;
}

void XenonIpcMainContainer::Shutdown() {
  BeginNativeShutdown();
  PollNativeShutdown();
}

void XenonIpcMainContainer::BeginNativeShutdown() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (shutting_down_) {
    return;
  }
  shutting_down_ = true;
#if BUILDFLAG(ENABLE_XENON_NODE_UV_COMPAT)
  native_event_loop_timer_.Stop();
#endif
  if (!utility_processes_.empty()) {
    ScopedV8Context scope(isolate_, context_);
    utility_processes_.clear();
  }
  if (worker_engine_) {
    worker_engine_->Shutdown();
  }

  if (!worker_control_ && initialized_ && !shutdown_app_.IsEmpty()) {
    ScopedV8Context scope(isolate_, context_);
    gin::TryCatch try_catch(isolate_);
    if (shutdown_app_.Get(isolate_)
            ->Call(scope.context(), scope.context()->Global(), 0, nullptr)
            .IsEmpty()) {
      LOG(ERROR) << "Electron app shutdown listener failed: "
                 << try_catch.GetStackTrace();
    }
    if (!shutdown_web_requests_.IsEmpty() &&
        shutdown_web_requests_.Get(isolate_)
            ->Call(scope.context(), scope.context()->Global(), 0, nullptr)
            .IsEmpty()) {
      LOG(ERROR) << "WebRequest shutdown failed: " << try_catch.GetStackTrace();
    }
    if (!shutdown_electron_net_.IsEmpty() &&
        shutdown_electron_net_.Get(isolate_)
            ->Call(scope.context(), scope.context()->Global(), 0, nullptr)
            .IsEmpty()) {
      LOG(ERROR) << "Electron network shutdown failed: "
                 << try_catch.GetStackTrace();
    }
    isolate_->PerformMicrotaskCheckpoint();
  }

  initialized_ = false;
  if (udp_sockets_) {
    udp_sockets_->Shutdown();
  }
  weak_factory_.InvalidateWeakPtrs();
  if (message_ports_) {
    message_ports_->Shutdown();
  }
  for (auto& [id, entry] : zlib_streams_) {
    entry.stream->Cancel();
  }
  zlib_streams_.clear();
  if (!context_.IsEmpty()) {
    ScopedV8Context scope(isolate_, context_);
    for (auto& [path, addon] : loaded_node_addons_) {
      if (addon->env) {
        addon->env->BeginShutdown();
      }
    }
    for (auto& addon : failed_node_addons_) {
      if (addon->env) {
        addon->env->BeginShutdown();
      }
    }
  }
  for (auto& [id, request] : pending_http_requests_) {
    if (request.cancel) {
      std::move(request.cancel).Run();
    }
  }
  // V8 promise reactions can still run from an addon completion while the
  // runtime drains. Reject their callers now but keep the External tokens
  // alive until no JavaScript can run; their weak owners are already invalid.
  for (auto* reply : pending_promise_replies_) {
    if (reply->callback) {
      std::move(reply->callback)
          .Run(ErrorResult("The Utility main JavaScript container stopped"));
    }
  }
  renderers_.clear();
}

bool XenonIpcMainContainer::PollNativeShutdown() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  CHECK(shutting_down_);
  if (native_shutdown_complete_) {
    return true;
  }
  // Worker threads own independent native loops and V8 isolates. Their exit
  // events arrive only after retirement; keep this parent and its shared file
  // descriptors alive until then instead of stopping threads with pending UV
  // requests or scheduled close callbacks.
  if (worker_engine_ && !worker_engine_->IsDrained()) {
    return false;
  }
  if (!context_.IsEmpty()) {
    ScopedV8Context scope(isolate_, context_);
    // Environments may finish work or run finalizers which still use libuv.
    // Close the shared runtime loop only after every environment is ready.
#if BUILDFLAG(ENABLE_XENON_NODE_UV_COMPAT)
    if (native_event_loop_ && !native_loop_draining_) {
      auto pumped = native_event_loop_->Pump();
      if (!pumped.has_value()) {
        native_shutdown_error_ = pumped.error();
        return false;
      }
    }
#endif
    auto poll_environments = [&] {
      bool environments_ready = true;
      for (auto& [path, addon] : loaded_node_addons_) {
        if (addon->env && !addon->env->PollShutdown()) {
          environments_ready = false;
        }
      }
      for (auto& addon : failed_node_addons_) {
        if (addon->env && !addon->env->PollShutdown()) {
          environments_ready = false;
        }
      }
      return environments_ready;
    };
    if (!poll_environments()) {
      return false;
    }
#if BUILDFLAG(ENABLE_XENON_NODE_UV_COMPAT)
    if (native_event_loop_) {
      native_loop_draining_ = true;
      native_event_loop_->BeginShutdown();
      auto drained = native_event_loop_->PollShutdown(poll_environments);
      if (!drained.has_value()) {
        native_shutdown_error_ = drained.error();
        return false;
      }
      if (!drained.value()) {
        return false;
      }
    }
#endif
  }
  // All callback stacks and V8 scopes have unwound before isolate disposal.
  FinalizeNativeShutdown();
  return true;
}

std::string XenonIpcMainContainer::DescribeNativeShutdown() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  size_t pending_environments = 0;
  for (const auto& [path, addon] : loaded_node_addons_) {
    if (addon->env && addon->env->HasPendingWork()) {
      ++pending_environments;
    }
  }
  for (const auto& addon : failed_node_addons_) {
    if (addon->env && addon->env->HasPendingWork()) {
      ++pending_environments;
    }
  }
  std::string result =
      "ipcMain: " + base::NumberToString(pending_environments) +
      " pending native environments";
#if BUILDFLAG(ENABLE_XENON_NODE_UV_COMPAT)
  if (native_event_loop_) {
    for (const auto& handle : native_event_loop_->InspectHandles()) {
      result += "; " + handle.type + (handle.active ? " active" : " inactive") +
                (handle.closing ? " closing" : " open") +
                (handle.referenced ? " referenced" : " unreferenced");
    }
  }
#endif
  if (!native_shutdown_error_.empty()) {
    result += "; " + native_shutdown_error_;
  }
  return result;
}

void XenonIpcMainContainer::ScheduleNativeEventLoopPump() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
#if BUILDFLAG(ENABLE_XENON_NODE_UV_COMPAT)
  if (!shutting_down_ && native_event_loop_ && !native_event_loop_pumping_ &&
      !native_event_loop_timer_.IsRunning()) {
    native_event_loop_timer_.Start(
        FROM_HERE, base::Milliseconds(10), this,
        &XenonIpcMainContainer::PumpNativeEventLoop);
  }
#endif
}

void XenonIpcMainContainer::PumpNativeEventLoop() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
#if BUILDFLAG(ENABLE_XENON_NODE_UV_COMPAT)
  if (shutting_down_ || !native_event_loop_ || context_.IsEmpty()) {
    return;
  }
  {
    // Native callbacks may issue another UDP call and enter a nested RunLoop.
    // Only schedule their next pump after this uv_run has unwound.
    base::AutoReset<bool> pumping(&native_event_loop_pumping_, true);
    ScopedV8Context scope(isolate_, context_);
    auto result = native_event_loop_->Pump();
    if (!result.has_value()) {
      LOG(ERROR) << "ipcMain native event loop: " << result.error();
    }
    isolate_->PerformMicrotaskCheckpoint();
  }
  if (shutting_down_ || !native_event_loop_) {
    return;
  }
  // Built-in UDP activity can be restarted by its next native call. Once its
  // handles and requests have drained, do not retain an empty 100 Hz pump.
  // Loaded addons may enqueue work later without another JS call, so keep
  // their conservative polling. uv_loop_alive alone also misses unreferenced
  // and inactive handles; neither is evidence that an addon has gone idle.
  if (loaded_node_addons_.empty() && failed_node_addons_.empty() &&
      !native_event_loop_->HasReferencedWork() &&
      native_event_loop_->InspectHandles().empty()) {
    native_event_loop_timer_.Stop();
    return;
  }
  ScheduleNativeEventLoopPump();
#endif
}

void XenonIpcMainContainer::FinalizeNativeShutdown() {
  if (async_context_hooks_) {
    v8::Locker locker(isolate_);
    v8::Isolate::Scope isolate_scope(isolate_);
    async_context_hooks_->Detach();
    async_context_hooks_.reset();
  }
  if (file_descriptors_) {
    if (!worker_options_) {
      file_descriptors_->CloseAll();
    } else if (worker_options_->track_unmanaged_fds) {
      file_descriptors_->CloseOwned(worker_options_->id);
    }
    file_descriptors_.reset();
  }
  electron_net_.reset();
  electron_net_online_.Reset();
  commonjs_parent_stack_.clear();
  module_resolution_cache_.clear();
  hooked_module_sources_.clear();
  has_resolve_hooks_ = false;
  has_load_hooks_ = false;
  if (!context_.IsEmpty()) {
    ScopedV8Context scope(isolate_, context_);
    esm_loader_.reset();
    module_resolver_.reset();
    pending_fs_calls_.clear();
    pending_window_calls_.clear();
    pending_zlib_calls_.clear();
    pending_http_requests_.clear();
    timers_.clear();
    worker_rejections_.clear();
    module_cache_.clear();
    loaded_node_addons_.clear();
    failed_node_addons_.clear();
  } else {
    esm_loader_.reset();
    module_resolver_.reset();
    pending_fs_calls_.clear();
    pending_window_calls_.clear();
    pending_zlib_calls_.clear();
    pending_http_requests_.clear();
    timers_.clear();
    worker_rejections_.clear();
    module_cache_.clear();
    loaded_node_addons_.clear();
    failed_node_addons_.clear();
  }
#if BUILDFLAG(ENABLE_XENON_NODE_UV_COMPAT)
  udp_sockets_.reset();
  native_event_loop_.reset();
#endif
  dispatch_send_.Reset();
  dispatch_invoke_.Reset();
  dispatch_protocol_.Reset();
  cancel_protocol_.Reset();
  dispatch_web_request_.Reset();
  cancel_web_request_.Reset();
  release_web_request_registration_.Reset();
  shutdown_web_requests_.Reset();
  dispatch_electron_net_event_.Reset();
  shutdown_electron_net_.Reset();
  dispatch_sync_.Reset();
  dispatch_window_event_.Reset();
  dispatch_app_event_.Reset();
  dispatch_renderer_event_.Reset();
  mark_app_ready_.Reset();
  shutdown_app_.Reset();
  dispatch_worker_event_.Reset();
  shutdown_workers_.Reset();
  message_ports_.reset();
  context_.Reset();
  if (worker_control_) {
    worker_control_->PublishIsolate(nullptr);
  }
  isolate_ = nullptr;
  isolate_holder_.reset();
  FailAllPendingPromises("The Utility main JavaScript container stopped");
  crash_reporter_annotations_.Clear();
  initialized_ = false;
  native_shutdown_complete_ = true;
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
         capture_function("__xenonDispatchWorkerEvent",
                          &dispatch_worker_event_) &&
         capture_function("__xenonShutdownWorkers", &shutdown_workers_) &&
         capture_function("__xenonDispatchInvoke", &dispatch_invoke_) &&
         capture_function("__xenonDispatchProtocol", &dispatch_protocol_) &&
         capture_function("__xenonCancelProtocol", &cancel_protocol_) &&
         capture_function("__xenonDispatchWebRequest",
                          &dispatch_web_request_) &&
         capture_function("__xenonCancelWebRequest", &cancel_web_request_) &&
         capture_function("__xenonReleaseWebRequestRegistration",
                          &release_web_request_registration_) &&
         capture_function("__xenonShutdownWebRequests",
                          &shutdown_web_requests_) &&
         capture_function("__xenonDispatchElectronNetEvent",
                          &dispatch_electron_net_event_) &&
         capture_function("__xenonShutdownElectronNet",
                          &shutdown_electron_net_) &&
         capture_function("__xenonDispatchSync", &dispatch_sync_) &&
         capture_function("__xenonDispatchBrowserWindowEvent",
                          &dispatch_window_event_) &&
         capture_function("__xenonDispatchAppEvent", &dispatch_app_event_) &&
         capture_function("__xenonDispatchRendererEvent",
                          &dispatch_renderer_event_) &&
         capture_function("__xenonMarkAppReady", &mark_app_ready_) &&
         capture_function("__xenonShutdownApp", &shutdown_app_);
}

bool XenonIpcMainContainer::ResolveApplicationMain(AppMainModule main_module) {
  ElectronApplicationInfo application;
  if (!ResolveElectronApplication(main_module.app_path, &application,
                                  &startup_error_)) {
    return false;
  }
  app_path_ = application.app_path;
  module_root_ = app_path_;
  base::FilePath candidate = main_module.main_script_path.empty()
                                 ? application.main_script_path
                                 : main_module.main_script_path;
  if (!candidate.IsAbsolute()) {
    startup_error_ = "Application main script override must be absolute";
    return false;
  }
  auto resolved = ResolveCommonJsPath(candidate, &startup_error_);
  if (!resolved ||
      (*resolved != module_root_ && !module_root_.IsParent(*resolved))) {
    if (resolved) {
      startup_error_ =
          "Configured main JavaScript is outside its application "
          "directory: " +
          resolved->AsUTF8Unsafe();
    }
    return false;
  }
  main_script_path_ = std::move(*resolved);
  app_name_ = !main_module.app_name.empty() ? std::move(main_module.app_name)
              : application.name.empty()    ? "Application"
                                            : application.name;
  app_version_ = !main_module.app_version.empty()
                     ? std::move(main_module.app_version)
                 : application.version.empty() ? "0.0.0"
                                               : application.version;
  executable_path_ = std::move(main_module.executable_path);
  resources_directory_ = main_module.resources_directory.empty()
                             ? application.resources_directory
                             : std::move(main_module.resources_directory);
  working_directory_ = std::move(main_module.working_directory);
  app_argv_ = std::move(main_module.app_argv);
  is_packaged_ = main_module.is_packaged.value_or(application.is_packaged);
  default_user_agent_ = std::move(main_module.default_user_agent);
  renderer_url_mappings_ = std::move(main_module.renderer_url_mappings);
  renderer_base_url_ = std::move(main_module.renderer_base_url);
  return true;
}

bool XenonIpcMainContainer::ResolveConfiguredMainScript() {
  const base::CommandLine* command_line =
      base::CommandLine::ForCurrentProcess();
  base::PathService::Get(base::DIR_EXE, &app_path_);

  if (command_line->HasSwitch(switches::kMainScript)) {
    main_script_path_ = command_line->GetSwitchValuePath(switches::kMainScript);
    if (main_script_path_.empty() || !main_script_path_.IsAbsolute()) {
      startup_error_ = "--xenon-main-js requires an absolute JavaScript path";
      return false;
    }
    app_path_ = main_script_path_.DirName();
    module_root_ = app_path_;
  } else if (command_line->HasSwitch(switches::kElectronApp)) {
    AppMainModule application;
    application.app_path =
        command_line->GetSwitchValuePath(switches::kElectronApp);
    return ResolveApplicationMain(std::move(application));
  }

  if (main_script_path_.empty()) {
    return true;
  }

  base::FilePath normalized_main_script;
  if (!NormalizeCommonJsPath(main_script_path_, &normalized_main_script)) {
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

void XenonIpcMainContainer::InitializeModuleLoaders() {
  ScopedV8Context scope(isolate_, context_);
  module_resolver_ = std::make_unique<XenonModuleResolver>(
      module_root_.empty() ? app_path_ : module_root_, executable_path_,
      base::BindRepeating([](const base::FilePath& path, base::FilePath* output,
                             bool* directory) {
        return NormalizeCommonJsPath(path, output, directory);
      }),
      base::BindRepeating(&ReadCommonJsFile),
      base::BindRepeating([](const std::string& request) {
        return BuiltinModuleGlobal(request) != nullptr;
      }),
      base::BindRepeating(&XenonIpcMainContainer::ResolvePackageMap,
                          base::Unretained(this)),
      base::BindRepeating(&XenonIpcMainContainer::ResolveCommonJsLookupPaths,
                          base::Unretained(this)));
  esm_loader_ = std::make_unique<XenonEsmModuleLoader>(
      isolate_, scope.context(),
      base::BindRepeating(&XenonIpcMainContainer::ResolveEsmModule,
                          base::Unretained(this)),
      base::BindRepeating(&XenonIpcMainContainer::ReadEsmModuleSource,
                          base::Unretained(this)),
      base::BindRepeating(&XenonIpcMainContainer::LoadEsmValue,
                          base::Unretained(this)),
      base::BindRepeating(&XenonIpcMainContainer::GetCommonJsExportNames,
                          base::Unretained(this)));
  esm_loader_->SetPrepareModuleCallback(base::BindRepeating(
      &XenonIpcMainContainer::PrepareHookedModule, base::Unretained(this)));
}

bool XenonIpcMainContainer::ReadModuleSourceFile(const base::FilePath& path,
                                                 std::string* source) {
  return ReadCommonJsFile(path, source);
}

bool XenonIpcMainContainer::IsBuiltinModuleRequest(
    const std::string& name) const {
  return BuiltinModuleGlobal(name) != nullptr;
}

v8::MaybeLocal<v8::Value> XenonIpcMainContainer::BuiltinModuleValue(
    const std::string& name,
    std::string* error) {
  const char* builtin = BuiltinModuleGlobal(name);
  if (!builtin) {
    *error = "ERR_UNKNOWN_BUILTIN_MODULE: No such built-in module: " + name;
    return {};
  }
  if (worker_control_ && name == "electron") {
    *error =
        "ERR_NOT_SUPPORTED: Electron modules are unavailable in a Node worker";
    return {};
  }
  return context_.Get(isolate_)->Global()->Get(
      context_.Get(isolate_), gin::StringToV8(isolate_, builtin));
}

bool XenonIpcMainContainer::ResolvePackageMap(const std::string& package_json,
                                              const std::string& key,
                                              bool importing,
                                              bool imports,
                                              std::string* target,
                                              std::string* error) {
  auto context = context_.Get(isolate_);
  v8::Local<v8::Value> package;
  v8::Local<v8::Value> resolver;
  if (!v8::JSON::Parse(context,
                       gin::StringToV8(isolate_, package_json).As<v8::String>())
           .ToLocal(&package) ||
      !context->Global()
           ->Get(context, gin::StringToV8(isolate_, "__xenonResolvePackageMap"))
           .ToLocal(&resolver) ||
      !resolver->IsFunction()) {
    *error = "ERR_INVALID_PACKAGE_CONFIG: Cannot read package map";
    return false;
  }
  const std::vector<std::string> defaults{
      "node", importing ? "import" : "require", "node-addons", "module-sync"};
  const auto* selected = module_resolver_->active_conditions();
  if (!selected) {
    selected = &defaults;
  }
  auto conditions = v8::Array::New(isolate_, selected->size());
  for (uint32_t index = 0; index < selected->size(); ++index) {
    conditions
        ->Set(context, index, gin::StringToV8(isolate_, (*selected)[index]))
        .Check();
  }
  v8::Local<v8::Value> argv[] = {package, gin::StringToV8(isolate_, key),
                                 conditions,
                                 v8::Boolean::New(isolate_, imports)};
  v8::Local<v8::Value> result;
  if (!resolver.As<v8::Function>()
           ->Call(context, context->Global(), std::size(argv), argv)
           .ToLocal(&result) ||
      !gin::ConvertFromV8(isolate_, result, target)) {
    *error = "ERR_INVALID_PACKAGE_TARGET: Cannot resolve " + key;
    return false;
  }
  return true;
}

bool XenonIpcMainContainer::ResolveEsmModule(
    const std::string& specifier,
    const std::string& parent_url,
    XenonEsmModuleLoader::ResolutionMode mode,
    XenonEsmModuleLoader::ModuleInfo* result,
    std::string* error) {
  if (has_resolve_hooks_ || has_load_hooks_) {
    return ResolveWithModuleHooks(
        specifier, parent_url,
        mode == XenonEsmModuleLoader::ResolutionMode::kMetaResolve
            ? XenonModuleResolver::Mode::kMetaResolve
            : XenonModuleResolver::Mode::kImport,
        esm_loader_->current_import_attributes(), result, error);
  }
  base::FilePath parent;
  if (!net::FileURLToFilePath(GURL(parent_url), &parent)) {
    parent = base::FilePath::FromUTF8Unsafe(parent_url);
  }
  if (!parent.IsAbsolute()) {
    *error =
        "ERR_INVALID_MODULE_SPECIFIER: Invalid module referrer " + parent_url;
    return false;
  }
  XenonModuleResolver::Resolution resolved;
  if (!module_resolver_->Resolve(
          specifier, parent,
          mode == XenonEsmModuleLoader::ResolutionMode::kMetaResolve
              ? XenonModuleResolver::Mode::kMetaResolve
              : XenonModuleResolver::Mode::kImport,
          &resolved, error)) {
    return false;
  }
  result->filename = resolved.filename;
  result->url = resolved.url;
  switch (resolved.format) {
    case XenonModuleResolver::Format::kModule:
      result->format = XenonEsmModuleLoader::Format::kModule;
      break;
    case XenonModuleResolver::Format::kCommonJs:
      result->format = XenonEsmModuleLoader::Format::kCommonJs;
      break;
    case XenonModuleResolver::Format::kJson:
      result->format = XenonEsmModuleLoader::Format::kJson;
      break;
    case XenonModuleResolver::Format::kBuiltin:
      result->format = XenonEsmModuleLoader::Format::kBuiltin;
      break;
  }
  if (mode == XenonEsmModuleLoader::ResolutionMode::kImport &&
      resolved.format != XenonModuleResolver::Format::kBuiltin) {
    const auto extension = resolved.filename.FinalExtension();
    if (!extension.empty() && extension != FILE_PATH_LITERAL(".js") &&
        extension != FILE_PATH_LITERAL(".mjs") &&
        extension != FILE_PATH_LITERAL(".cjs") &&
        extension != FILE_PATH_LITERAL(".json")) {
      *error = "ERR_UNKNOWN_FILE_EXTENSION: Unsupported module extension: " +
               resolved.url;
      return false;
    }
  }
  return true;
}

v8::MaybeLocal<v8::Value> XenonIpcMainContainer::LoadEsmValue(
    const XenonEsmModuleLoader::ModuleInfo& module,
    std::string* error) {
  if (module.format == XenonEsmModuleLoader::Format::kBuiltin) {
    return BuiltinModuleValue(module.url, error);
  }
  if (requiring_esm_ &&
      module.format == XenonEsmModuleLoader::Format::kCommonJs) {
    const auto cached = module_cache_.find(
        module.filename.empty() ? module.url : module.filename.AsUTF8Unsafe());
    if (cached != module_cache_.end()) {
      v8::Local<v8::Value> loaded;
      if (!cached->second.Get(isolate_)
               .As<v8::Object>()
               ->Get(context_.Get(isolate_),
                     gin::StringToV8(isolate_, "loaded"))
               .ToLocal(&loaded)) {
        return {};
      }
      if (loaded->IsFalse()) {
        *error =
            "ERR_REQUIRE_CYCLE_MODULE: Cannot import an executing CommonJS "
            "module while requiring ESM: " +
            module.url;
        return {};
      }
    }
  }
  if (module.use_module_hooks) {
    return LoadHookedModule(module, error);
  }
  return LoadResolvedCommonJsModule(module.filename, error);
}

bool XenonIpcMainContainer::GetCommonJsExportNames(
    const XenonEsmModuleLoader::ModuleInfo& module,
    std::vector<std::string>* names,
    std::string* error) {
  auto context = context_.Get(isolate_);
  v8::Local<v8::Value> lexer;
  if (!context->Global()
           ->Get(context,
                 gin::StringToV8(isolate_, "__xenonParseCommonJsExports"))
           .ToLocal(&lexer) ||
      !lexer->IsFunction()) {
    *error = "ERR_NOT_SUPPORTED: CommonJS export lexer is unavailable";
    return false;
  }
  std::set<std::string> visited;
  std::set<std::string> exports;
  auto visit = [&](auto&& self, XenonEsmModuleLoader::ModuleInfo info) -> bool {
    if (!visited.insert(info.url).second) {
      return true;
    }
    if (!PrepareHookedModule(&info, error)) {
      return false;
    }
    if (info.hook_format == "addon" ||
        info.filename.MatchesExtension(FILE_PATH_LITERAL(".node"))) {
      return true;
    }
    std::string source;
    if (!ReadEsmModuleSource(info, &source, error)) {
      return false;
    }
    v8::Local<v8::Value> argv[] = {gin::StringToV8(isolate_, source),
                                   gin::StringToV8(isolate_, info.url)};
    v8::Local<v8::Value> parsed;
    if (!lexer.As<v8::Function>()
             ->Call(context, context->Global(), std::size(argv), argv)
             .ToLocal(&parsed) ||
        !parsed->IsObject()) {
      *error = "ERR_MODULE_NOT_FOUND: Cannot inspect CommonJS exports";
      return false;
    }
    for (const char* property : {"exports", "reexports"}) {
      v8::Local<v8::Value> entries;
      if (!parsed.As<v8::Object>()
               ->Get(context, gin::StringToV8(isolate_, property))
               .ToLocal(&entries) ||
          !entries->IsArray()) {
        return false;
      }
      auto array = entries.As<v8::Array>();
      for (uint32_t index = 0; index < array->Length(); ++index) {
        v8::Local<v8::Value> value;
        std::string name;
        if (!array->Get(context, index).ToLocal(&value) ||
            !gin::ConvertFromV8(isolate_, value, &name)) {
          return false;
        }
        if (std::string_view(property) == "exports") {
          exports.insert(std::move(name));
          continue;
        }
        // A statically seen re-export may be in an unexecuted branch. Node's
        // pre-parser ignores unresolved re-exports; actual require still fails
        // normally if that branch executes during module evaluation.
        v8::TryCatch reexport_error(isolate_);
        XenonEsmModuleLoader::ModuleInfo dependency;
        std::string ignored_error;
        bool resolved = false;
        if (has_resolve_hooks_ || has_load_hooks_) {
          resolved = ResolveWithModuleHooks(name, info.url,
                                            XenonModuleResolver::Mode::kRequire,
                                            {}, &dependency, &ignored_error) &&
                     PrepareHookedModule(&dependency, &ignored_error);
        } else {
          XenonModuleResolver::Resolution raw;
          if (module_resolver_->Resolve(name, info.filename,
                                        XenonModuleResolver::Mode::kRequire,
                                        &raw, &ignored_error) &&
              raw.format == XenonModuleResolver::Format::kCommonJs) {
            dependency.filename = raw.filename;
            dependency.url = raw.url;
            dependency.format = XenonEsmModuleLoader::Format::kCommonJs;
            resolved = true;
          }
        }
        if (resolved &&
            dependency.format == XenonEsmModuleLoader::Format::kCommonJs &&
            !dependency.filename.MatchesExtension(FILE_PATH_LITERAL(".node"))) {
          if (!self(self, dependency)) {
            error->clear();
          }
        }
      }
    }
    return true;
  };
  if (!visit(visit, module)) {
    return false;
  }
  names->assign(exports.begin(), exports.end());
  return true;
}

void XenonIpcMainContainer::OnMainModuleEvaluated(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  auto* self =
      static_cast<XenonIpcMainContainer*>(info.Data().As<v8::External>()->Value(
          v8::kExternalPointerTypeTagDefault));
  self->main_evaluation_pending_ = false;
  if (self->app_exit_requested_ || self->shutting_down_) {
    return;
  }
  if (self->main_evaluation_complete_handler_) {
    self->main_evaluation_complete_handler_.Run();
  } else if (self->app_ready_requested_) {
    self->MarkAppReady();
  }
}

void XenonIpcMainContainer::OnMainModuleRejected(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  auto* self =
      static_cast<XenonIpcMainContainer*>(info.Data().As<v8::External>()->Value(
          v8::kExternalPointerTypeTagDefault));
  self->main_evaluation_pending_ = false;
  v8::String::Utf8Value message(info.GetIsolate(), info[0]);
  if (self->worker_control_ || self->utility_options_) {
    self->ReportNodeException(info[0]);
    return;
  }
  self->startup_error_ = "Failed to evaluate main ES module: " +
                         (*message ? std::string(*message, message.length())
                                   : "unknown rejection");
  LOG(ERROR) << self->startup_error_;
  if (!self->app_exit_requested_ && !self->shutting_down_) {
    self->app_exit_requested_ = true;
    // As with app.exit(), the owner may destroy the isolate in its callback.
    // Finish the current V8/IPC stack before notifying it.
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&XenonIpcMainContainer::NotifyAppExit,
                                  self->weak_factory_.GetWeakPtr(), 1));
  }
}

bool XenonIpcMainContainer::MaybeLoadConfiguredMainScript() {
  if (main_script_path_.empty()) {
    return true;
  }

  ScopedV8Context scope(isolate_, context_);
  v8::TryCatch try_catch(isolate_);
  std::string error;
  v8::Local<v8::Value> ignored;
  v8::MaybeLocal<v8::Value> loaded;
  XenonModuleResolver::Resolution entry;
  if (embedded_main_source_) {
    loaded =
        LoadCommonJsSource(main_script_path_, *embedded_main_source_, &error);
  } else if (module_resolver_->Describe(main_script_path_, &entry, &error,
                                        XenonModuleResolver::Mode::kImport) &&
             entry.format == XenonModuleResolver::Format::kModule) {
    XenonEsmModuleLoader::ModuleInfo module;
    module.url = entry.url;
    module.filename = entry.filename;
    module.format = XenonEsmModuleLoader::Format::kModule;
    v8::Local<v8::Promise> evaluation;
    if (esm_loader_->Evaluate(module, &error).ToLocal(&evaluation)) {
      // Resolve already queued await continuations before deciding whether
      // initialization must wait for native asynchronous work.
      isolate_->PerformMicrotaskCheckpoint();
      if (utility_options_ && app_exit_requested_ &&
          isolate_->IsExecutionTerminating()) {
        return true;
      }
      if (evaluation->State() == v8::Promise::kRejected) {
        evaluation->MarkAsHandled();
        isolate_->ThrowException(evaluation->Result());
      } else {
        loaded = evaluation;
        if (evaluation->State() == v8::Promise::kPending) {
          main_evaluation_pending_ = true;
          auto data = v8::External::New(isolate_, this,
                                        v8::kExternalPointerTypeTagDefault);
          auto fulfilled =
              v8::Function::New(scope.context(), &OnMainModuleEvaluated, data)
                  .ToLocalChecked();
          auto rejected =
              v8::Function::New(scope.context(), &OnMainModuleRejected, data)
                  .ToLocalChecked();
          if (evaluation->Then(scope.context(), fulfilled, rejected)
                  .IsEmpty()) {
            loaded = {};
          }
        }
      }
    }
  } else if (error.empty()) {
    loaded = LoadCommonJsModule(main_script_path_, &error);
  }
  if (!loaded.ToLocal(&ignored)) {
    if (utility_options_ && app_exit_requested_ &&
        isolate_->IsExecutionTerminating()) {
      return true;
    }
    if ((worker_control_ || utility_options_) && try_catch.HasCaught() &&
        ReportNodeException(try_catch.Exception())) {
      return true;
    }
    if (utility_options_ && app_exit_requested_) {
      return true;
    }
    if (try_catch.HasCaught()) {
      v8::Local<v8::Value> details = try_catch.Exception();
      v8::Local<v8::Value> stack;
      if (try_catch.StackTrace(scope.context()).ToLocal(&stack) &&
          stack->IsString()) {
        details = stack;
      }
      v8::String::Utf8Value message(isolate_, details);
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

bool XenonIpcMainContainer::MarkAppReady() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  app_ready_requested_ = true;
  if (!initialized_ || shutting_down_ || app_exit_requested_ ||
      mark_app_ready_.IsEmpty() || main_evaluation_pending_ ||
      !startup_error_.empty()) {
    return false;
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
  return true;
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

  XenonModuleResolver::Resolution description;
  if (!module_resolver_->Describe(normalized, &description, error)) {
    return {};
  }
  if (description.format == XenonModuleResolver::Format::kModule) {
    XenonEsmModuleLoader::ModuleInfo module;
    module.url = description.url;
    module.filename = normalized;
    module.format = XenonEsmModuleLoader::Format::kModule;
    v8::Local<v8::Value> exports;
    base::AutoReset<bool> requiring_esm(&requiring_esm_, true);
    if (!esm_loader_->Require(module, error).ToLocal(&exports)) {
      return {};
    }
    // Cache only successful synchronous evaluation, using the same require
    // record as CJS. Repeated require must not re-read package metadata or fail
    // after a loaded file is removed; the ESM namespace retains live bindings.
    cache_exports(exports);
    return exports;
  }

  if (normalized.FinalExtension() == FILE_PATH_LITERAL(".node")) {
    if (shutting_down_) {
      *error = "ERR_DLOPEN_FAILED: native runtime is shutting down";
      return {};
    }
    if (worker_control_) {
      *error =
          "ERR_NOT_SUPPORTED: native addons are unavailable in a Node worker";
      return {};
    }
    const auto native_path = GetNativeCommonJsPath(normalized, error);
    if (!native_path) {
      return {};
    }
    if (native_addon_hooks_.load) {
      std::string load_error;
      bool loaded = false;
      {
        v8::Unlocker unlocker(isolate_);
        loaded = native_addon_hooks_.load.Run(native_path->AsUTF8Unsafe(),
                                              &load_error);
      }
      if (!loaded) {
        *error = load_error.empty()
                     ? "ERR_DLOPEN_FAILED: Failed to load Node-API addon: " +
                           cache_key
                     : load_error;
        // The filename was resolved successfully. A loader/policy failure must
        // not be mislabeled MODULE_NOT_FOUND: bindings treats that as a reason
        // to ignore the real failure and continue searching other filenames.
        if (!error->starts_with("ERR_")) {
          error->insert(0, "ERR_DLOPEN_FAILED: ");
        }
        return {};
      }
      v8::Local<v8::Value> forwarder;
      if (!CreateNativeAddonForwarder(native_path->AsUTF8Unsafe())
               .ToLocal(&forwarder)) {
        *error =
            "ERR_DLOPEN_FAILED: Failed to create native addon forwarder: " +
            cache_key;
        return {};
      }
      cache_exports(forwarder);
      return forwarder;
    }
    uv_loop_s* runtime_loop = nullptr;
#if BUILDFLAG(ENABLE_XENON_NODE_UV_COMPAT)
    runtime_loop = EnsureNativeEventLoop(error);
    if (!runtime_loop) {
      *error = "ERR_DLOPEN_FAILED: " + *error;
      return {};
    }
#endif
    auto loaded_addon = std::make_unique<xenon::LoadedNodeAddon>();
    v8::Local<v8::Value> exports = xenon::LoadAndInitializeNodeAddon(
        isolate_, context_.Get(isolate_), *native_path, loaded_addon.get(),
        /*allow_default_uv_loop=*/false, runtime_loop);
    // Initialization may run a nested loop before the addon is retained in
    // either map below. Re-arm after it returns, including failed initializers
    // whose environments still own callbacks.
    ScheduleNativeEventLoopPump();
    if (exports.IsEmpty()) {
      if (loaded_addon->env) {
        failed_node_addons_.push_back(std::move(loaded_addon));
      }
      *error = "ERR_DLOPEN_FAILED: Failed to load Node-API addon: " + cache_key;
      return {};
    }
    cache_exports(exports);
    loaded_node_addons_.emplace(cache_key, std::move(loaded_addon));
    return exports;
  }

  std::string source_text;
  if (!ReadCommonJsFile(normalized, &source_text)) {
    *error = "Failed to read module: " + cache_key;
    return {};
  }

  if (description.format == XenonModuleResolver::Format::kJson) {
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

v8::MaybeLocal<v8::Value> XenonIpcMainContainer::CallModuleMethod(
    const char* name,
    int argc,
    v8::Local<v8::Value> argv[]) {
  const auto context = context_.Get(isolate_);
  v8::Local<v8::Value> module, method;
  if (!context->Global()
           ->Get(context, gin::StringToV8(isolate_, "__xenonModule"))
           .ToLocal(&module)) {
    return {};
  }
  if (!module->IsObject()) {
    isolate_->ThrowException(v8::Exception::TypeError(
        gin::StringToV8(isolate_, "Module API is unavailable")));
    return {};
  }
  if (!module.As<v8::Object>()
           ->Get(context, gin::StringToV8(isolate_, name))
           .ToLocal(&method)) {
    return {};
  }
  if (!method->IsFunction()) {
    isolate_->ThrowException(v8::Exception::TypeError(gin::StringToV8(
        isolate_, std::string("Module.") + name + " must be a function")));
    return {};
  }
  return method.As<v8::Function>()->Call(context, module, argc, argv);
}

v8::MaybeLocal<v8::Object> XenonIpcMainContainer::CreateCommonJsParent(
    const base::FilePath& filename) {
  const auto context = context_.Get(isolate_);
  const auto directory =
      gin::StringToV8(isolate_, filename.DirName().AsUTF8Unsafe());
  v8::Local<v8::Value> argv[] = {directory};
  v8::Local<v8::Value> paths;
  if (!CallModuleMethod("_nodeModulePaths", std::size(argv), argv)
           .ToLocal(&paths)) {
    return {};
  }
  auto parent = v8::Object::New(isolate_);
  const auto name = gin::StringToV8(isolate_, filename.AsUTF8Unsafe());
  for (const char* property : {"id", "filename"}) {
    parent
        ->CreateDataProperty(context, gin::StringToV8(isolate_, property), name)
        .Check();
  }
  parent
      ->CreateDataProperty(context, gin::StringToV8(isolate_, "path"),
                           directory)
      .Check();
  parent->CreateDataProperty(context, gin::StringToV8(isolate_, "paths"), paths)
      .Check();
  return parent;
}

v8::MaybeLocal<v8::Value> XenonIpcMainContainer::CallCommonJsLookupPaths(
    const std::string& request,
    v8::Local<v8::Object> parent) {
  v8::Local<v8::Value> argv[] = {gin::StringToV8(isolate_, request), parent};
  return CallModuleMethod("_resolveLookupPaths", std::size(argv), argv);
}

bool XenonIpcMainContainer::ResolveCommonJsLookupPaths(
    const std::string& request,
    const base::FilePath& parent_file,
    std::vector<base::FilePath>* paths,
    std::string* error) {
  v8::Local<v8::Object> parent;
  for (auto active = commonjs_parent_stack_.rbegin();
       active != commonjs_parent_stack_.rend(); ++active) {
    if (active->first == parent_file) {
      parent = active->second.Get(isolate_);
      break;
    }
  }
  if (parent.IsEmpty()) {
    const auto cached = module_cache_.find(parent_file.AsUTF8Unsafe());
    if (cached != module_cache_.end()) {
      parent = cached->second.Get(isolate_).As<v8::Object>();
    } else if (!CreateCommonJsParent(parent_file).ToLocal(&parent)) {
      return false;
    }
  }
  v8::Local<v8::Value> result;
  if (!CallCommonJsLookupPaths(request, parent).ToLocal(&result)) {
    return false;
  }
  if (result->IsNull()) {
    return true;
  }
  std::vector<std::string> entries;
  if (!gin::ConvertFromV8(isolate_, result, &entries)) {
    *error =
        "ERR_INVALID_ARG_VALUE: Module lookup paths must be an array of "
        "strings or null";
    return false;
  }
  for (const auto& entry : entries) {
    const auto path = base::FilePath::FromUTF8Unsafe(entry);
    if (entry.find('\0') != std::string::npos || !path.IsAbsolute()) {
      *error =
          "ERR_INVALID_ARG_VALUE: Module lookup paths must be absolute paths";
      return false;
    }
    paths->push_back(path);
  }
  return true;
}

v8::MaybeLocal<v8::Function> XenonIpcMainContainer::CreateRequireFunction(
    const base::FilePath& parent_path,
    v8::Local<v8::Object> parent_module) {
  v8::Local<v8::Context> context = context_.Get(isolate_);
  const std::string parent_file = parent_path.AsUTF8Unsafe();
  if (parent_module.IsEmpty() &&
      !CreateCommonJsParent(parent_path).ToLocal(&parent_module)) {
    return {};
  }
  auto require_callback = [](const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Local<v8::Array> data = info.Data().As<v8::Array>();
    v8::Isolate* isolate = info.GetIsolate();
    v8::Local<v8::Context> context = isolate->GetCurrentContext();
    v8::Local<v8::Value> host_value;
    v8::Local<v8::Value> parent_value;
    v8::Local<v8::Value> operation;
    v8::Local<v8::Value> parent_record;
    if (!data->Get(context, 0).ToLocal(&host_value) ||
        !data->Get(context, 1).ToLocal(&parent_value) ||
        !data->Get(context, 2).ToLocal(&operation) ||
        !data->Get(context, 3).ToLocal(&parent_record) ||
        !host_value->IsExternal() || !parent_record->IsObject()) {
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
    if (info[0].As<v8::String>()->Length() == 0 &&
        operation.As<v8::Int32>()->Value() != 2) {
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
      const auto filename = base::FilePath::FromUTF8Unsafe(*parent);
      host->commonjs_parent_stack_.emplace_back(
          filename,
          v8::Global<v8::Object>(isolate, parent_record.As<v8::Object>()));
      base::ScopedClosureRunner pop_parent(base::BindOnce(
          [](XenonIpcMainContainer* owner) {
            owner->commonjs_parent_stack_.pop_back();
          },
          base::Unretained(host)));
      const auto request_text = std::string(*request, request.length());
      const auto loaded =
          operation.As<v8::Int32>()->Value() == 2
              ? host->CallCommonJsLookupPaths(request_text,
                                              parent_record.As<v8::Object>())
              : host->RequireModule(request_text, filename, &require_error,
                                    operation.As<v8::Int32>()->Value() == 1);
      if (!loaded.ToLocal(&result) && require_try_catch.HasCaught()) {
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
  auto create_require = [&](int operation) {
    v8::Local<v8::Array> data = v8::Array::New(isolate_, 4);
    data->Set(context, 0,
              v8::External::New(isolate_, this,
                                v8::kExternalPointerTypeTagDefault))
        .Check();
    data->Set(context, 1, gin::StringToV8(isolate_, parent_file)).Check();
    data->Set(context, 2, v8::Integer::New(isolate_, operation)).Check();
    data->Set(context, 3, parent_module).Check();
    return v8::Function::New(context, require_callback, data).ToLocalChecked();
  };
  v8::Local<v8::Function> require_function = create_require(0);
  const auto resolve = create_require(1);
  resolve->Set(context, gin::StringToV8(isolate_, "paths"), create_require(2))
      .Check();
  require_function
      ->Set(context, v8::String::NewFromUtf8Literal(isolate_, "resolve"),
            resolve)
      .Check();

  return require_function;
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

  std::string body = source_text;
  if (body.starts_with("\xEF\xBB\xBF")) {
    body.erase(0, 3);
  }
  if (body.starts_with("#!")) {
    const auto end = body.find_first_of("\r\n");
    body.replace(0, end == std::string::npos ? body.size() : end, "");
  }
  std::string wrapped =
      "(function(exports, require, module, __filename, __dirname) {\n" + body +
      "\n})";
  v8::TryCatch try_catch(isolate_);
  v8::Local<v8::String> source =
      v8::String::NewFromUtf8(isolate_, wrapped.c_str()).ToLocalChecked();
  // Resource name must be set so Error.stack CallSite#getFileName() works.
  // Node's `bindings` package crashes on undefined.getFileName() with
  // `Cannot read properties of undefined (reading 'indexOf')`.
  const std::string resource_name = virtual_path.AsUTF8Unsafe();
  v8::ScriptOrigin origin(
      v8::String::NewFromUtf8(isolate_, resource_name.c_str())
          .ToLocalChecked());
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
  v8::Local<v8::Object> module;
  if (!CreateCommonJsParent(virtual_path).ToLocal(&module)) {
    if (try_catch.HasCaught()) {
      try_catch.ReThrow();
    }
    return {};
  }
  module
      ->Set(context, v8::String::NewFromUtf8Literal(isolate_, "exports"),
            exports)
      .Check();
  module
      ->Set(context, gin::StringToV8(isolate_, "filename"),
            gin::StringToV8(isolate_, cache_key))
      .Check();
  module
      ->Set(context, gin::StringToV8(isolate_, "id"),
            gin::StringToV8(isolate_, cache_key))
      .Check();
  module->Set(context, gin::StringToV8(isolate_, "loaded"), v8::False(isolate_))
      .Check();
  // Keep the module record: a circular dependency can observe a replacement
  // module.exports before this wrapper returns, or an exported function can
  // replace it after evaluation. An exports snapshot is stale in both cases.
  module_cache_.emplace(cache_key, v8::Global<v8::Value>(isolate_, module));

  v8::Local<v8::Function> require_function;
  if (!CreateRequireFunction(virtual_path, module).ToLocal(&require_function)) {
    module_cache_.erase(cache_key);
    if (try_catch.HasCaught()) {
      try_catch.ReThrow();
    }
    return {};
  }
  module->Set(context, gin::StringToV8(isolate_, "require"), require_function)
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
  module->Set(context, gin::StringToV8(isolate_, "loaded"), v8::True(isolate_))
      .Check();
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
    base::FilePath normalized;
    bool is_directory = false;
    if (!NormalizeCommonJsPath(candidate, &normalized, &is_directory) ||
        is_directory) {
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

  if (IsCommonJsDirectory(requested_path)) {
    const base::FilePath package_path =
        requested_path.AppendASCII("package.json");
    std::string package_text;
    if (ReadCommonJsFile(package_path, &package_text)) {
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
            NormalizeCommonJsPath(requested_path, &normalized_directory) &&
            NormalizeCommonJsPath(main_path, &normalized_main) &&
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

  if (worker_control_ && request == "electron") {
    *error =
        "ERR_NOT_SUPPORTED: Electron modules are unavailable in a Node worker";
    return {};
  }
  if (has_resolve_hooks_ || has_load_hooks_) {
    const auto parent_url = parent_file.IsAbsolute()
                                ? net::FilePathToFileURL(parent_file).spec()
                                : parent_file.AsUTF8Unsafe();
    XenonEsmModuleLoader::ModuleInfo module;
    if (!ResolveWithModuleHooks(request, parent_url,
                                XenonModuleResolver::Mode::kRequire, {},
                                &module, error)) {
      return {};
    }
    if (resolve_only) {
      std::string resolved =
          module.filename.empty() ? module.url : module.filename.AsUTF8Unsafe();
      if (resolved.starts_with("node:") && !request.starts_with("node:")) {
        resolved.erase(0, 5);
      }
      return gin::StringToV8(isolate_, resolved);
    }
    return LoadHookedModule(std::move(module), error);
  }

  if (const char* builtin = BuiltinModuleGlobal(request)) {
    if (resolve_only) {
      return gin::StringToV8(isolate_, request);
    }
    return context->Global()->Get(context, gin::StringToV8(isolate_, builtin));
  }

  const auto requested_path = base::FilePath::FromUTF8Unsafe(request);
  bool use_lookup = !requested_path.IsAbsolute() && request != "." &&
                    request != ".." && !request.starts_with("./") &&
                    !request.starts_with("../") && !request.starts_with('#');
#if BUILDFLAG(IS_WIN)
  use_lookup =
      use_lookup && !request.starts_with(".\\") && !request.starts_with("..\\");
#endif
  std::vector<base::FilePath> lookup_paths;
  std::string lookup_key = request;
  bool cache_resolution = true;
  if (use_lookup) {
    v8::Local<v8::Value> module, current_lookup, default_lookup;
    if (!context->Global()
             ->Get(context, gin::StringToV8(isolate_, "__xenonModule"))
             .ToLocal(&module) ||
        !module->IsObject() ||
        !module.As<v8::Object>()
             ->Get(context, gin::StringToV8(isolate_, "_resolveLookupPaths"))
             .ToLocal(&current_lookup) ||
        !context->Global()
             ->Get(context,
                   gin::StringToV8(isolate_, "__xenonDefaultModuleLookupPaths"))
             .ToLocal(&default_lookup)) {
      return {};
    }
    cache_resolution = current_lookup->IsFunction() &&
                       current_lookup->StrictEquals(default_lookup);
    if (cache_resolution) {
      // Compute the default path list once. This retains the hot module cache
      // without ignoring application changes to parent.paths.
      if (!ResolveCommonJsLookupPaths(request, parent_file, &lookup_paths,
                                      error)) {
        return {};
      }
      for (const auto& path : lookup_paths) {
        lookup_key += '\0';
        lookup_key += path.AsUTF8Unsafe();
      }
    }
  }
  const auto resolution_key =
      std::make_pair(parent_file.AsUTF8Unsafe(), lookup_key);
  auto resolution = module_resolution_cache_.find(resolution_key);
  if (cache_resolution && resolution != module_resolution_cache_.end()) {
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
    if (cache_resolution) {
      module_resolution_cache_.insert_or_assign(resolution_key,
                                                path.AsUTF8Unsafe());
    }
    return exports;
  };

  XenonModuleResolver::Resolution resolved;
  if (!module_resolver_->Resolve(
          request, parent_file, XenonModuleResolver::Mode::kRequire, &resolved,
          error, nullptr,
          use_lookup && cache_resolution ? &lookup_paths : nullptr)) {
    return {};
  }
  if (resolved.format == XenonModuleResolver::Format::kBuiltin) {
    return RequireModule(resolved.url, parent_file, error, resolve_only);
  }
  return load_resolved(resolved.filename);
}

void XenonIpcMainContainer::NativeInstallAsyncContextHooks(
    gin::Arguments* args) {
  v8::Local<v8::Function> init;
  v8::Local<v8::Function> before;
  v8::Local<v8::Function> after;
  if (async_context_hooks_ || !args->GetNext(&init) ||
      !args->GetNext(&before) || !args->GetNext(&after)) {
    args->ThrowTypeError(
        "Async context hooks require three functions and one installation");
    return;
  }
  async_context_hooks_ = base::MakeRefCounted<AsyncContextHooks>(
      isolate_, context_.Get(isolate_), init, before, after);
}

void XenonIpcMainContainer::NativeIsPromise(gin::Arguments* args) {
  v8::Local<v8::Value> value;
  args->Return(args->GetNext(&value) && value->IsPromise());
}

void XenonIpcMainContainer::NativeCanParseURL(gin::Arguments* args) {
  const auto is_valid = [](const GURL& url) {
    if (!url.is_valid()) {
      return false;
    }
    if (!url.has_host() ||
        !(url.SchemeIsHTTPOrHTTPS() || url.SchemeIsWSOrWSS() ||
          url.SchemeIsFile() || url.SchemeIs("ftp"))) {
      return true;
    }
    // GURL has already parsed the authority and canonicalized IDNA/IP hosts.
    // Its special-host parser can still accept spaces (a feature flag, with a
    // separate file: exception). Validate the decoded, canonical host with
    // Chromium's existing forbidden-host-code-point checker, independently of
    // those browser compatibility settings. Opaque hosts must not be decoded.
    const std::string host = base::UnescapeBinaryURLComponent(url.host());
    url::RawCanonOutput<256> output;
    url::Component component;
    return url::CanonicalizeNonSpecialHost(
        host, url::Component(0, base::checked_cast<int>(host.size())), output,
        component);
  };
  std::string input;
  if (!args->GetNext(&input)) {
    args->ThrowTypeError("URL validation requires a string");
    return;
  }
  v8::Local<v8::Value> base;
  if (args->Length() < 2 || (args->GetNext(&base) && base->IsUndefined())) {
    args->Return(is_valid(GURL(input)));
    return;
  }
  std::string base_string;
  if (base.IsEmpty() || !gin::ConvertFromV8(isolate_, base, &base_string)) {
    args->ThrowTypeError("URL validation base must be a string");
    return;
  }
  const GURL base_url(base_string);
  // WHATWG URL parsing rejects an invalid supplied base even if the input is
  // absolute. Use Chromium's parser; the compatibility URL constructor alone
  // does not establish validity.
  args->Return(is_valid(base_url) && is_valid(base_url.Resolve(input)));
}

void XenonIpcMainContainer::NativeIsNativeError(gin::Arguments* args) {
  v8::Local<v8::Value> value;
  args->Return(args->GetNext(&value) && value->IsNativeError());
}

void XenonIpcMainContainer::NativeUtilType(gin::Arguments* args) {
  std::string name;
  v8::Local<v8::Value> value;
  if (!args->GetNext(&name)) {
    args->ThrowTypeError("A native type predicate name is required");
    return;
  }
  if (!args->GetNext(&value)) {
    value = v8::Undefined(args->isolate());
  }
  // V8 brands work across realms and do not invoke Proxy traps or user-defined
  // Symbol.toStringTag getters. Node's util/types uses these same predicates.
  using Predicate = bool (v8::Value::*)() const;
  static constexpr auto predicates =
      base::MakeFixedFlatMap<std::string_view, Predicate>({
          {"isArgumentsObject", &v8::Value::IsArgumentsObject},
          {"isArrayBuffer", &v8::Value::IsArrayBuffer},
          {"isArrayBufferView", &v8::Value::IsArrayBufferView},
          {"isAsyncFunction", &v8::Value::IsAsyncFunction},
          {"isBigInt64Array", &v8::Value::IsBigInt64Array},
          {"isBigIntObject", &v8::Value::IsBigIntObject},
          {"isBigUint64Array", &v8::Value::IsBigUint64Array},
          {"isBooleanObject", &v8::Value::IsBooleanObject},
          {"isDataView", &v8::Value::IsDataView},
          {"isDate", &v8::Value::IsDate},
          {"isExternal", &v8::Value::IsExternal},
          {"isFloat16Array", &v8::Value::IsFloat16Array},
          {"isFloat32Array", &v8::Value::IsFloat32Array},
          {"isFloat64Array", &v8::Value::IsFloat64Array},
          {"isGeneratorFunction", &v8::Value::IsGeneratorFunction},
          {"isGeneratorObject", &v8::Value::IsGeneratorObject},
          {"isInt16Array", &v8::Value::IsInt16Array},
          {"isInt32Array", &v8::Value::IsInt32Array},
          {"isInt8Array", &v8::Value::IsInt8Array},
          {"isMap", &v8::Value::IsMap},
          {"isMapIterator", &v8::Value::IsMapIterator},
          {"isModuleNamespaceObject", &v8::Value::IsModuleNamespaceObject},
          {"isNativeError", &v8::Value::IsNativeError},
          {"isNumberObject", &v8::Value::IsNumberObject},
          {"isPromise", &v8::Value::IsPromise},
          {"isProxy", &v8::Value::IsProxy},
          {"isRegExp", &v8::Value::IsRegExp},
          {"isSet", &v8::Value::IsSet},
          {"isSetIterator", &v8::Value::IsSetIterator},
          {"isSharedArrayBuffer", &v8::Value::IsSharedArrayBuffer},
          {"isStringObject", &v8::Value::IsStringObject},
          {"isSymbolObject", &v8::Value::IsSymbolObject},
          {"isTypedArray", &v8::Value::IsTypedArray},
          {"isUint16Array", &v8::Value::IsUint16Array},
          {"isUint32Array", &v8::Value::IsUint32Array},
          {"isUint8Array", &v8::Value::IsUint8Array},
          {"isUint8ClampedArray", &v8::Value::IsUint8ClampedArray},
          {"isWeakMap", &v8::Value::IsWeakMap},
          {"isWeakSet", &v8::Value::IsWeakSet},
      });
  if (name == "isAnyArrayBuffer") {
    args->Return(value->IsArrayBuffer() || value->IsSharedArrayBuffer());
  } else if (name == "isBoxedPrimitive") {
    args->Return(value->IsNumberObject() || value->IsStringObject() ||
                 value->IsBooleanObject() || value->IsBigIntObject() ||
                 value->IsSymbolObject());
  } else if (const auto predicate = predicates.find(name);
             predicate != predicates.end()) {
    args->Return((value.operator->()->*(predicate->second))());
  } else {
    args->ThrowTypeError("Unknown native type predicate");
  }
}

void XenonIpcMainContainer::NativeIsBuiltinModule(gin::Arguments* args) {
  std::string name;
  // Electron is a separate loader entry, not a Node builtin. Query the same
  // registry used by require(), including the original-fs aliases.
  args->Return(args->GetNext(&name) && name != "electron" &&
               BuiltinModuleGlobal(name) != nullptr);
}

void XenonIpcMainContainer::NativeCreateRequire(gin::Arguments* args) {
  std::string filename;
  if (!args->GetNext(&filename) || filename.find('\0') != std::string::npos) {
    args->ThrowTypeError("createRequire requires an absolute filename");
    return;
  }
  auto parent = base::FilePath::FromUTF8Unsafe(filename);
#if BUILDFLAG(IS_WIN)
  if (!parent.IsAbsolute() && !parent.empty() &&
      base::FilePath::IsSeparator(parent.value().front())) {
    // Node accepts drive-relative rooted paths such as /app/entry.js. Resolve
    // their device using this container's cwd, without consulting the host cwd
    // or requiring the synthetic parent file to exist.
    base::FilePath current_directory;
    if (file_descriptors_->GetWorkingDirectory(&current_directory) != 0) {
      isolate_->ThrowException(v8::Exception::Error(gin::StringToV8(
          isolate_, "Process working directory is unavailable")));
      return;
    }
    const auto cwd = current_directory.NormalizePathSeparators();
    const auto components = cwd.GetComponents();
    if (cwd.IsNetwork() && components.size() >= 3) {
      parent = base::FilePath(components[0] + components[1] +
                              FILE_PATH_LITERAL("\\") + components[2] +
                              parent.value());
    } else if (!components.empty()) {
      parent = base::FilePath(components.front() + parent.value());
    }
  }
#endif
  if (!parent.IsAbsolute()) {
    args->ThrowTypeError("createRequire requires an absolute filename");
    return;
  }
  v8::Local<v8::Function> require;
  if (CreateRequireFunction(parent).ToLocal(&require)) {
    args->Return(require);
  }
}

void XenonIpcMainContainer::NativeProcessDirectory(gin::Arguments* args) {
  std::string directory;
  const bool changing = args->GetNext(&directory);
  int status = 0;
  base::FilePath current;
  if (changing && worker_options_) {
    status = UV_ENOTSUP;
  } else if (changing) {
    status = file_descriptors_->ChangeWorkingDirectory(directory);
  } else {
    status = file_descriptors_->GetWorkingDirectory(&current);
  }
  base::DictValue result;
  result.Set("ok", status == 0);
  if (status == 0) {
    if (!changing) {
      result.Set("directory", current.AsUTF8Unsafe());
    }
  } else {
    result.Set("code", uv_err_name(status));
    result.Set("errno", status);
    result.Set("message", uv_strerror(status));
  }
  v8::Local<v8::Value> value;
  if (ValueToV8(base::Value(std::move(result))).ToLocal(&value)) {
    args->Return(value);
  }
}

void XenonIpcMainContainer::NativeV8Serialize(gin::Arguments* args) {
  v8::Local<v8::Value> value;
  if (!args->GetNext(&value)) {
    value = v8::Undefined(isolate_);
  }
  v8::Local<v8::Value> buffer_constructor;
  if (!args->GetNext(&buffer_constructor) ||
      !buffer_constructor->IsFunction()) {
    args->ThrowTypeError("serialize requires the runtime Buffer constructor");
    return;
  }
  v8::Local<v8::Function> data_view_byte_length;
  if (!args->GetNext(&data_view_byte_length)) {
    args->ThrowTypeError("serialize requires the runtime DataView getter");
    return;
  }
  NodeValueSerializer delegate(isolate_, buffer_constructor,
                               data_view_byte_length);
  auto& serializer = delegate.serializer();
  serializer.WriteHeader();
  if (!serializer.WriteValue(context_.Get(isolate_), value).FromMaybe(false)) {
    return;
  }
  auto [data, size] = serializer.Release();
  // V8 sandbox builds require ArrayBuffer memory from the isolate allocator;
  // the serializer's malloc allocation cannot be adopted as a backing store.
  auto backing = v8::ArrayBuffer::NewBackingStore(isolate_, size);
  if (size) {
    UNSAFE_BUFFERS(base::span(static_cast<uint8_t*>(backing->Data()), size))
        .copy_from(UNSAFE_BUFFERS(base::span(data, size)));
  }
  std::free(data);
  auto buffer = v8::ArrayBuffer::New(isolate_, std::move(backing));
  args->Return(v8::Uint8Array::New(buffer, 0, size).As<v8::Value>());
}

void XenonIpcMainContainer::NativeIsDataCloneError(gin::Arguments* args) {
  v8::Local<v8::Value> value;
  args->Return(args->GetNext(&value) &&
               IsWorkerDataCloneError(context_.Get(isolate_), value));
}

void XenonIpcMainContainer::NativeStructuredClone(gin::Arguments* args) {
  v8::Local<v8::Value> value;
  v8::Local<v8::Value> transfers;
  if (!args->GetNext(&value)) {
    args->ThrowTypeError("structuredClone requires a value");
    return;
  }
  if (!args->GetNext(&transfers)) {
    transfers = v8::Undefined(isolate_);
  }
  auto context = context_.Get(isolate_);
  auto prepared = PreparedWorkerMessage::Serialize(context, value, transfers);
  if (!prepared) {
    return;
  }
  auto message = prepared->Commit(isolate_);
  if (!message) {
    return;
  }
  v8::Local<v8::Value> cloned;
  if (message->Deserialize(context).ToLocal(&cloned)) {
    args->Return(cloned);
  }
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
  auto path = app_paths_->Get(name);
  if (!path.has_value()) {
    ThrowAppPathError(isolate_, path.error().code, path.error().message);
    return;
  }
  args->Return(path->AsUTF8Unsafe());
}

void XenonIpcMainContainer::NativeSetPath(gin::Arguments* args) {
  std::string name;
  std::string path;
  if (!args->GetNext(&name) || !args->GetNext(&path)) {
    args->ThrowTypeError("setPath expects name and path strings");
    return;
  }
  // FilePath truncates at NUL; reject before constructing it.
  if (path.find('\0') != std::string::npos) {
    ThrowAppPathError(isolate_, "ERR_INVALID_ARG_VALUE",
                      "Path must not contain NUL");
    return;
  }
  auto result = app_paths_->Set(name, base::FilePath::FromUTF8Unsafe(path));
  if (!result.has_value()) {
    ThrowAppPathError(isolate_, result.error().code, result.error().message);
  }
}

void XenonIpcMainContainer::NativeSetAppLogsPath(gin::Arguments* args) {
  std::optional<base::FilePath> path;
  if (args->Length() > 0) {
    std::string text;
    if (!args->GetNext(&text)) {
      args->ThrowTypeError("setAppLogsPath expects an optional path string");
      return;
    }
    if (text.find('\0') != std::string::npos) {
      ThrowAppPathError(isolate_, "ERR_INVALID_ARG_VALUE",
                        "Path must not contain NUL");
      return;
    }
    path = base::FilePath::FromUTF8Unsafe(text);
  }
  auto result = app_paths_->SetLogsPath(std::move(path));
  if (!result.has_value()) {
    ThrowAppPathError(isolate_, result.error().code, result.error().message);
  }
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

void XenonIpcMainContainer::NativeRegisterSchemesAsPrivileged(
    gin::Arguments* args) {
  v8::Local<v8::Value> input;
  base::Value schemes;
  std::string error;
  if (args->Length() != 1 || !args->GetNext(&input) ||
      !V8ToValue(input, &schemes, &error) || !schemes.is_list()) {
    args->ThrowTypeError(
        "ERR_INVALID_ARG_TYPE: privileged schemes must be an array");
    return;
  }
  // Chromium locks its scheme tables before any application main is loaded.
  // Never mutate those process-wide tables from a live Utility V8 context.
  if (!ValidatePrivilegedSchemesAgainstStartup(schemes.GetList(), &error)) {
    isolate_->ThrowException(
        v8::Exception::Error(gin::StringToV8(isolate_, error)));
  }
}

void XenonIpcMainContainer::NativeOsCall(gin::Arguments* args) {
  v8::Local<v8::Value> request_value;
  base::Value request;
  std::string error;
  if (args->Length() != 1 || !args->GetNext(&request_value) ||
      !V8ToValue(request_value, &request, &error) || !request.is_dict()) {
    args->ThrowTypeError(
        "ERR_INVALID_ARG_TYPE: os call expects a request object");
    return;
  }
  auto result = PerformOsCall(std::move(request));
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

void XenonIpcMainContainer::NativeCrashReporterCall(gin::Arguments* args) {
  v8::Local<v8::Value> request_value;
  base::Value request;
  std::string error;
  if (args->Length() != 1 || !args->GetNext(&request_value) ||
      !V8ToValue(request_value, &request, &error)) {
    args->ThrowTypeError(
        "ERR_INVALID_ARG_TYPE: crashReporter expects a request object");
    return;
  }
  auto result = crash_reporter_annotations_.Call(std::move(request));
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
  auto result = file_descriptors_->CallOwned(
      std::move(arguments),
      worker_options_ && worker_options_->track_unmanaged_fds
          ? worker_options_->id
          : 0);
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
          base::BindOnce(&XenonFileDescriptors::CallOwned, file_descriptors_,
                         std::move(arguments),
                         worker_options_ && worker_options_->track_unmanaged_fds
                             ? worker_options_->id
                             : 0),
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
      !ConvertV8ToValue(isolate_, context_.Get(isolate_), options, &settings,
                        &error, 0, true) ||
      !settings.is_dict()) {
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

void XenonIpcMainContainer::NativeZlibStreamCall(gin::Arguments* args) {
  v8::Local<v8::Value> input;
  bool asynchronous = false;
  std::string owner, error;
  base::Value value;
  if (!args->GetNext(&input) || !args->GetNext(&asynchronous) ||
      !args->GetNext(&owner) ||
      !ConvertV8ToValue(isolate_, context_.Get(isolate_), input, &value, &error,
                        0, true) ||
      !value.is_dict()) {
    args->ThrowTypeError(
        "Compression stream expects a request, mode and owner");
    return;
  }
  auto fail = [&](const std::string& message) {
    isolate_->ThrowException(
        v8::Exception::Error(gin::StringToV8(isolate_, message)));
  };
  if (shutting_down_ || (!owner.empty() && !renderers_.contains(owner))) {
    fail("ERR_STREAM_DESTROYED: Compression owner is unavailable");
    return;
  }
  const auto& request = value.GetDict();
  const auto* action = request.FindString("action");
  if (!action) {
    fail("ERR_INVALID_ARG_TYPE: Missing compression action");
    return;
  }
  if (*action == "create") {
    const auto* operation = request.FindString("operation");
    const auto* options = request.FindDict("options");
    if (!operation || !options) {
      fail("ERR_INVALID_ARG_TYPE: Missing compression format or options");
      return;
    }
    if (zlib_streams_.size() >= 128 ||
        next_zlib_stream_id_ == std::numeric_limits<int>::max()) {
      fail("ERR_RESOURCE_BUSY: Too many compression streams");
      return;
    }
    auto stream = ZlibStream::Create(*operation, *options, &error);
    if (!stream) {
      fail(error);
      return;
    }
    const int id = next_zlib_stream_id_++;
    zlib_streams_.emplace(id, ZlibStreamEntry{owner, std::move(stream)});
    args->Return(id);
    return;
  }
  const auto id = request.FindInt("id");
  auto entry = id ? zlib_streams_.find(*id) : zlib_streams_.end();
  if (entry == zlib_streams_.end() || entry->second.owner != owner) {
    if (*action == "close") {
      args->Return(false);
      return;
    }
    fail("ERR_STREAM_DESTROYED: Compression stream is unavailable");
    return;
  }
  auto stream = entry->second.stream;
  if (*action == "close") {
    stream->Cancel();
    zlib_streams_.erase(entry);
    args->Return(true);
    return;
  }
  const auto* bytes = request.FindBlob("data");
  const auto flush = request.FindInt("flush");
  const auto capacity = request.FindInt("outputSize");
  if (*action != "process" || !bytes || !flush || !capacity ||
      bytes->size() > ZlibStream::kMaximumChunk || *capacity < 64 ||
      *capacity > static_cast<int>(ZlibStream::kMaximumChunk)) {
    fail("ERR_INVALID_ARG_TYPE: Invalid compression chunk request");
    return;
  }
  if (asynchronous && pending_zlib_calls_.size() >= 128) {
    fail("ERR_RESOURCE_BUSY: Compression worker queue is full");
    return;
  }
  if (!stream->TryBegin()) {
    fail("ERR_RESOURCE_BUSY: Compression stream has an active operation");
    return;
  }
  if (!asynchronous) {
    auto result = stream->Process(*bytes, *flush, *capacity);
    if (!result->success) {
      fail(result->error);
      return;
    }
    v8::Local<v8::Value> converted;
    if (ValueToV8(result->value).ToLocal(&converted)) {
      args->Return(converted);
    }
    return;
  }
  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(context_.Get(isolate_)).ToLocal(&resolver)) {
    stream->Cancel();
    zlib_streams_.erase(entry);
    return;
  }
  const uint64_t request_id = next_zlib_request_id_++;
  pending_zlib_calls_.emplace(
      request_id, v8::Global<v8::Promise::Resolver>(isolate_, resolver));
  args->Return(resolver->GetPromise());
  if (!base::ThreadPool::PostTaskAndReplyWithResult(
          FROM_HERE, {base::TaskPriority::USER_VISIBLE},
          base::BindOnce(&ZlibStream::Process, stream, *bytes, *flush,
                         *capacity),
          base::BindOnce(&XenonIpcMainContainer::OnZlibCallComplete,
                         weak_factory_.GetWeakPtr(), request_id))) {
    stream->Cancel();
    zlib_streams_.erase(entry);
    pending_zlib_calls_.erase(request_id);
    resolver
        ->Reject(context_.Get(isolate_),
                 v8::Exception::Error(gin::StringToV8(
                     isolate_,
                     "ERR_RESOURCE_BUSY: Cannot schedule compression task")))
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
  if (uses_iv &&
      iv.size() != static_cast<size_t>(EVP_CIPHER_iv_length(cipher))) {
    args->ThrowTypeError("Invalid initialization vector for " + algorithm);
    return;
  }

  crypto::OpenSSLErrStackTracer err_tracer(FROM_HERE);
  bssl::ScopedEVP_CIPHER_CTX context;
  const uint8_t* iv_data =
      uses_iv ? reinterpret_cast<const uint8_t*>(iv.data()) : nullptr;
  const uint8_t* key_data = reinterpret_cast<const uint8_t*>(key.data());
  const uint8_t* input_data = reinterpret_cast<const uint8_t*>(input.data());
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
  args->GetFunctionCallbackInfo()->GetReturnValue().Set(
      gin::StringToV8(args->isolate(), base::Base64Encode(output_view)));
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
    int32_t window_id,
    const std::string& document_url,
    bool is_main_frame) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  const std::string endpoint_id = base::NumberToString(next_endpoint_id_++);
  AddRenderer(endpoint_id, std::move(renderer), process_id, frame_id, window_id,
              document_url, is_main_frame);
  return endpoint_id;
}

void XenonIpcMainContainer::AddRenderer(
    const std::string& endpoint_id,
    mojo::PendingRemote<xenon::ipc::mojom::IpcRenderer> renderer,
    int32_t process_id,
    int32_t frame_id,
    int32_t window_id,
    const std::string& document_url,
    bool is_main_frame,
    mojom::IpcServiceWorkerInfoPtr worker_info) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  RemoveRenderer(endpoint_id);
  mojo::Remote<xenon::ipc::mojom::IpcRenderer> remote(std::move(renderer));
  remote.set_disconnect_handler(
      base::BindOnce(&XenonIpcMainContainer::RemoveRenderer,
                     weak_factory_.GetWeakPtr(), endpoint_id));
  renderers_.emplace(
      endpoint_id, RendererEndpoint{process_id, frame_id, window_id,
                                    document_url, is_main_frame,
                                    std::move(worker_info), std::move(remote)});
  DispatchRendererEvent(endpoint_id, true);
}

void XenonIpcMainContainer::UpdateRendererFrameUrl(
    const std::string& endpoint_id,
    const std::string& document_url) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto endpoint = renderers_.find(endpoint_id);
  if (endpoint == renderers_.end()) {
    return;
  }
  endpoint->second.document_url = document_url;
  DispatchRendererEvent(endpoint_id, true);
}

void XenonIpcMainContainer::RemoveRenderer(const std::string& endpoint_id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  for (auto entry = zlib_streams_.begin(); entry != zlib_streams_.end();) {
    if (entry->second.owner != endpoint_id) {
      ++entry;
      continue;
    }
    entry->second.stream->Cancel();
    entry = zlib_streams_.erase(entry);
  }
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
  v8::Local<v8::Value> argv[] = {CreateSenderMetadata(endpoint_id),
                                 v8::Boolean::New(isolate_, attached)};
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

void XenonIpcMainContainer::DispatchToRenderer(const std::string& endpoint_id,
                                               const std::string& channel,
                                               base::Value arguments) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto renderer = renderers_.find(endpoint_id);
  if (renderer != renderers_.end()) {
    renderer->second.remote->Dispatch(channel, std::move(arguments));
  }
}

void XenonIpcMainContainer::DispatchWindowEvent(int32_t window_id,
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

bool XenonIpcMainContainer::DispatchBeforeInputEvent(int32_t window_id,
                                                     base::Value input) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!initialized_ || shutting_down_ || dispatch_window_event_.IsEmpty()) {
    return true;
  }
  ScopedV8Context scope(isolate_, context_);
  gin::TryCatch try_catch(isolate_);
  v8::Local<v8::Value> converted;
  if (!ValueToV8(input).ToLocal(&converted)) {
    return true;
  }
  v8::Local<v8::Value> argv[] = {
      v8::Integer::New(isolate_, window_id),
      gin::StringToV8(isolate_, "web-contents-before-input-event"), converted};
  v8::Local<v8::Value> result;
  if (!dispatch_window_event_.Get(isolate_)
           ->Call(scope.context(), scope.context()->Global(), std::size(argv),
                  argv)
           .ToLocal(&result)) {
    LOG(ERROR) << "Electron before-input-event failed: "
               << try_catch.GetStackTrace();
    return true;
  }
  const bool prevented = result->IsTrue();
  isolate_->PerformMicrotaskCheckpoint();
  return prevented;
}

base::expected<bool, std::string> XenonIpcMainContainer::CheckPermission(
    base::Value request) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!initialized_ || shutting_down_) {
    return base::unexpected("ERR_ABORTED: permission container is unavailable");
  }
  ScopedV8Context scope(isolate_, context_);
  gin::TryCatch try_catch(isolate_);
  v8::Local<v8::Value> function;
  v8::Local<v8::Value> argument;
  if (!scope.context()
           ->Global()
           ->Get(scope.context(),
                 gin::StringToV8(isolate_, "__xenonCheckPermission"))
           .ToLocal(&function) ||
      !function->IsFunction() || !ValueToV8(request).ToLocal(&argument)) {
    return base::unexpected(
        "ERR_ABORTED: permission dispatcher is unavailable");
  }
  v8::Local<v8::Value> result;
  if (!function.As<v8::Function>()
           ->Call(scope.context(), scope.context()->Global(), 1, &argument)
           .ToLocal(&result)) {
    return base::unexpected("ERR_FAILED: " + try_catch.GetStackTrace());
  }
  if (!result->IsBoolean()) {
    return base::unexpected(
        "ERR_INVALID_ARG_TYPE: permission check must return boolean");
  }
  return result->IsTrue();
}

void XenonIpcMainContainer::DispatchAppEvent(const std::string& event_name,
                                             base::Value arguments) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!initialized_ || shutting_down_ || dispatch_app_event_.IsEmpty()) {
    return;
  }
  ScopedV8Context scope(isolate_, context_);
  gin::TryCatch try_catch(isolate_);
  v8::Local<v8::Value> converted_arguments;
  if (!ValueToV8(arguments).ToLocal(&converted_arguments)) {
    LOG(ERROR) << "Electron app event arguments could not be converted: "
               << try_catch.GetStackTrace();
    return;
  }
  v8::Local<v8::Value> argv[] = {gin::StringToV8(isolate_, event_name),
                                 converted_arguments};
  if (dispatch_app_event_.Get(isolate_)
          ->Call(scope.context(), scope.context()->Global(), std::size(argv),
                 argv)
          .IsEmpty()) {
    LOG(ERROR) << "Electron app event failed: " << try_catch.GetStackTrace();
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
  // Native callbacks can contain nested binary values (for example a SQLite
  // row containing a BLOB). JSON cannot represent those bytes.
  const auto context = context_.Get(isolate_);
  switch (value.type()) {
    case base::Value::Type::NONE:
      return v8::Null(isolate_);
    case base::Value::Type::BOOLEAN:
      return v8::Boolean::New(isolate_, value.GetBool());
    case base::Value::Type::INTEGER:
      return v8::Integer::New(isolate_, value.GetInt());
    case base::Value::Type::DOUBLE:
      return v8::Number::New(isolate_, value.GetDouble());
    case base::Value::Type::STRING:
      return gin::StringToV8(isolate_, value.GetString());
    case base::Value::Type::LIST: {
      auto array = v8::Array::New(
          isolate_, base::checked_cast<int>(value.GetList().size()));
      uint32_t index = 0;
      for (const auto& item : value.GetList()) {
        v8::Local<v8::Value> converted;
        if (!ValueToV8(item).ToLocal(&converted) ||
            !array->CreateDataProperty(context, index++, converted)
                 .FromMaybe(false)) {
          return {};
        }
      }
      return array;
    }
    case base::Value::Type::DICT: {
      auto object = v8::Object::New(isolate_);
      for (const auto [key, item] : value.GetDict()) {
        v8::Local<v8::Value> converted;
        if (!ValueToV8(item).ToLocal(&converted) ||
            !object
                 ->CreateDataProperty(context, gin::StringToV8(isolate_, key),
                                      converted)
                 .FromMaybe(false)) {
          return {};
        }
      }
      return object;
    }
    case base::Value::Type::BINARY:
      NOTREACHED();
  }
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
  // Only native main-process events have no renderer. All renderer entry
  // points validate registration before constructing frame/worker identities.
  CHECK(endpoint != renderers_.end() || endpoint_id == "@main");
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
  sender
      ->Set(context, v8::String::NewFromUtf8Literal(isolate_, "url"),
            gin::StringToV8(isolate_, endpoint == renderers_.end()
                                          ? std::string()
                                          : endpoint->second.document_url))
      .Check();
  sender
      ->Set(context, v8::String::NewFromUtf8Literal(isolate_, "isMainFrame"),
            v8::Boolean::New(isolate_, endpoint != renderers_.end() &&
                                           endpoint->second.is_main_frame))
      .Check();
  if (endpoint != renderers_.end() && endpoint->second.worker_info) {
    const auto& worker = *endpoint->second.worker_info;
    auto set = [&](const char* key, v8::Local<v8::Value> value) {
      sender->Set(context, gin::StringToV8(isolate_, key), value).Check();
    };
    set("type", gin::StringToV8(isolate_, "service-worker"));
    set("workerToken", gin::StringToV8(isolate_, endpoint_id));
    set("versionId", v8::Number::New(isolate_, worker.version_id));
    set("scope", gin::StringToV8(isolate_, worker.scope));
    set("scriptUrl", gin::StringToV8(isolate_, worker.script_url));
    set("partition", gin::StringToV8(isolate_, worker.partition));
  }
  return sender;
}

void XenonIpcMainContainer::ReceivePortMessage(
    const std::string& endpoint_id,
    const std::string& channel,
    blink::TransferableMessage message) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto it = renderers_.find(endpoint_id);
  if (!initialized_ || shutting_down_ || !message_ports_ ||
      it == renderers_.end() || it->second.worker_info) {
    return;
  }
  ScopedV8Context scope(isolate_, context_);
  message_ports_->Receive(CreateSenderMetadata(endpoint_id), channel,
                          std::move(message));
}

void XenonIpcMainContainer::Send(const std::string& endpoint_id,
                                 const std::string& channel,
                                 base::Value arguments) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!initialized_) {
    return;
  }
  const bool native_main_event =
      endpoint_id == "@main" && (channel.starts_with("__xenon:net:") ||
                                 channel == "__xenon:child-process:event");
  if (!native_main_event && !renderers_.contains(endpoint_id)) {
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

  if (!renderers_.contains(endpoint_id)) {
    std::move(callback).Run(
        ErrorResult("Renderer IPC endpoint is unavailable"));
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

void XenonIpcMainContainer::HandleProtocol(const std::string& partition,
                                           const std::string& request_id,
                                           base::Value request,
                                           InvokeCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!initialized_) {
    std::move(callback).Run(ErrorResult("Protocol container is unavailable"));
    return;
  }
  ScopedV8Context scope(isolate_, context_);
  v8::Local<v8::Value> payload;
  if (!ValueToV8(request).ToLocal(&payload)) {
    std::move(callback).Run(ErrorResult("Invalid protocol request"));
    return;
  }
  v8::Local<v8::Value> argv[] = {gin::StringToV8(isolate_, partition),
                                 gin::StringToV8(isolate_, request_id),
                                 payload};
  gin::TryCatch try_catch(isolate_);
  v8::Local<v8::Value> result;
  if (!dispatch_protocol_.Get(isolate_)
           ->Call(scope.context(), scope.context()->Global(), std::size(argv),
                  argv)
           .ToLocal(&result) ||
      !result->IsPromise()) {
    std::move(callback).Run(
        ErrorResult("Protocol dispatch failed: " + try_catch.GetStackTrace()));
    return;
  }
  auto* reply =
      new PromiseReplyContext{weak_factory_.GetWeakPtr(), std::move(callback),
                              "protocol:" + request_id, false, true};
  pending_promise_replies_.insert(reply);
  auto data =
      v8::External::New(isolate_, reply, v8::kExternalPointerTypeTagDefault);
  auto resolved = v8::Function::New(scope.context(), OnPromiseResolved, data)
                      .ToLocalChecked();
  auto rejected = v8::Function::New(scope.context(), OnPromiseRejected, data)
                      .ToLocalChecked();
  if (result.As<v8::Promise>()
          ->Then(scope.context(), resolved, rejected)
          .IsEmpty()) {
    pending_promise_replies_.erase(reply);
    std::move(reply->callback)
        .Run(ErrorResult("Cannot await protocol handler"));
    delete reply;
  }
  isolate_->PerformMicrotaskCheckpoint();
}

void XenonIpcMainContainer::CancelProtocol(const std::string& request_id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!initialized_) {
    return;
  }
  ScopedV8Context scope(isolate_, context_);
  v8::Local<v8::Value> argv[] = {gin::StringToV8(isolate_, request_id)};
  gin::TryCatch try_catch(isolate_);
  v8::Local<v8::Value> ignored;
  if (!cancel_protocol_.Get(isolate_)
           ->Call(scope.context(), scope.context()->Global(), std::size(argv),
                  argv)
           .ToLocal(&ignored)) {
    LOG(ERROR) << "Protocol cancellation failed: " << try_catch.GetStackTrace();
  }
  isolate_->PerformMicrotaskCheckpoint();
}

void XenonIpcMainContainer::HandleWebRequest(base::Value request,
                                             InvokeCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!initialized_ || shutting_down_) {
    std::move(callback).Run(ErrorResult("WebRequest container is unavailable"));
    return;
  }
  const auto* details = request.GetIfDict();
  const auto* request_id = details ? details->FindString("requestId") : nullptr;
  if (!request_id || request_id->empty()) {
    std::move(callback).Run(ErrorResult("Invalid WebRequest identity"));
    return;
  }
  ScopedV8Context scope(isolate_, context_);
  v8::Local<v8::Value> payload;
  if (!ValueToV8(request).ToLocal(&payload)) {
    std::move(callback).Run(ErrorResult("Invalid WebRequest payload"));
    return;
  }
  v8::Local<v8::Value> argv[] = {payload};
  gin::TryCatch try_catch(isolate_);
  v8::Local<v8::Value> result;
  if (!dispatch_web_request_.Get(isolate_)
           ->Call(scope.context(), scope.context()->Global(), std::size(argv),
                  argv)
           .ToLocal(&result) ||
      !result->IsPromise()) {
    std::move(callback).Run(ErrorResult("WebRequest dispatch failed: " +
                                        try_catch.GetStackTrace()));
    return;
  }
  auto* reply =
      new PromiseReplyContext{weak_factory_.GetWeakPtr(), std::move(callback),
                              "web-request:" + *request_id, false, true};
  pending_promise_replies_.insert(reply);
  auto data =
      v8::External::New(isolate_, reply, v8::kExternalPointerTypeTagDefault);
  auto resolved = v8::Function::New(scope.context(), OnPromiseResolved, data)
                      .ToLocalChecked();
  auto rejected = v8::Function::New(scope.context(), OnPromiseRejected, data)
                      .ToLocalChecked();
  if (result.As<v8::Promise>()
          ->Then(scope.context(), resolved, rejected)
          .IsEmpty()) {
    pending_promise_replies_.erase(reply);
    std::move(reply->callback)
        .Run(ErrorResult("Cannot await WebRequest callback"));
    delete reply;
  }
  isolate_->PerformMicrotaskCheckpoint();
}

void XenonIpcMainContainer::CancelWebRequest(const std::string& request_id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!initialized_ || shutting_down_) {
    return;
  }
  ScopedV8Context scope(isolate_, context_);
  v8::Local<v8::Value> argv[] = {gin::StringToV8(isolate_, request_id)};
  gin::TryCatch try_catch(isolate_);
  if (cancel_web_request_.Get(isolate_)
          ->Call(scope.context(), scope.context()->Global(), std::size(argv),
                 argv)
          .IsEmpty()) {
    LOG(ERROR) << "WebRequest cancellation failed: "
               << try_catch.GetStackTrace();
  }
  isolate_->PerformMicrotaskCheckpoint();
}

void XenonIpcMainContainer::ReleaseWebRequestRegistration(
    const std::string& registration_id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!initialized_ || shutting_down_) {
    return;
  }
  ScopedV8Context scope(isolate_, context_);
  v8::Local<v8::Value> argv[] = {gin::StringToV8(isolate_, registration_id)};
  gin::TryCatch try_catch(isolate_);
  if (release_web_request_registration_.Get(isolate_)
          ->Call(scope.context(), scope.context()->Global(), std::size(argv),
                 argv)
          .IsEmpty()) {
    LOG(ERROR) << "WebRequest registration release failed: "
               << try_catch.GetStackTrace();
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
  if (!renderers_.contains(endpoint_id)) {
    return ErrorResult("Renderer IPC endpoint is unavailable");
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

void XenonIpcMainContainer::NativeChildProcessCall(gin::Arguments* args) {
  base::Value request;
  std::string error;
  v8::Local<v8::Value> value = args->PeekNext();
  if (value.IsEmpty() || !V8ToValue(value, &request, &error) ||
      !request.is_dict()) {
    args->ThrowTypeError("childProcessCall expects a request object");
    return;
  }
  if (!child_process_caller_) {
    args->ThrowTypeError("Native child process transport is unavailable");
    return;
  }
  v8::Local<v8::Value> result;
  if (ValueToV8(child_process_caller_.Run(request.GetDict()))
          .ToLocal(&result)) {
    args->Return(result);
  }
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

void XenonIpcMainContainer::NativeSetBrowserWindowVisible(
    gin::Arguments* args) {
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

bool XenonIpcMainContainer::AddNativeRuntimeArguments(
    int32_t window_id,
    const std::string& command,
    base::Value* arguments,
    std::string* error) {
  auto* request = arguments->GetIfDict();
  const auto* method = request ? request->FindString("method") : nullptr;
  if (window_id != 0 || command != "app-single-instance" || !method ||
      *method != "request") {
    return true;
  }
  base::FilePath directory;
  if (!file_descriptors_ ||
      file_descriptors_->GetWorkingDirectory(&directory) != 0 ||
      !directory.IsAbsolute()) {
    *error = "ERR_CWD_UNAVAILABLE: native working directory is unavailable";
    return false;
  }
  // This is a native snapshot after argument conversion. Application code can
  // replace process.cwd or submit the reserved key, but cannot replace the
  // actual process state with either of those JavaScript values.
  request->Set(
      "__xenonRuntime",
      base::DictValue().Set("workingDirectory", directory.AsUTF8Unsafe()));
  return true;
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
      !(command == "mac-player"
            ? ConvertV8ToValue(isolate_, context_.Get(isolate_), value,
                               &arguments, &error, 0, true)
            : V8ToValue(value, &arguments, &error))) {
    args->ThrowTypeError(error.empty() ? "Invalid BrowserWindow arguments"
                                       : error);
    return;
  }
  if (!AddNativeRuntimeArguments(window_id, command, &arguments, &error)) {
    args->ThrowTypeError(error);
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

void XenonIpcMainContainer::NativeCanExitApp(gin::Arguments* args) {
  args->Return(static_cast<bool>(app_exit_handler_));
}

void XenonIpcMainContainer::NativeExitApp(gin::Arguments* args) {
  int exit_code = 0;
  if (!args->GetNext(&exit_code)) {
    args->ThrowTypeError("exit expects an integer exit code");
    return;
  }
  if (app_exit_requested_ || shutting_down_) {
    return;
  }
  if (!app_exit_handler_) {
    v8::Local<v8::Object> error =
        v8::Exception::Error(
            gin::StringToV8(isolate_, "Native application exit is unavailable")
                .As<v8::String>())
            .As<v8::Object>();
    if (error
            ->Set(isolate_->GetCurrentContext(),
                  gin::StringToV8(isolate_, "code"),
                  gin::StringToV8(isolate_, "ERR_NOT_SUPPORTED"))
            .IsJust()) {
      isolate_->ThrowException(error);
    }
    return;
  }
  app_exit_requested_ = true;
  // The owner may disconnect the Utility and destroy this isolate. Never
  // perform that teardown from inside a running JavaScript/IPC stack.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&XenonIpcMainContainer::NotifyAppExit,
                                weak_factory_.GetWeakPtr(), exit_code));
}

void XenonIpcMainContainer::NotifyAppExit(int exit_code) {
  if (utility_options_ && isolate_ && isolate_->IsExecutionTerminating()) {
    // The terminating JS stack has unwound. Allow exit listeners and addon
    // cleanup to run only now, immediately before the owner begins retirement.
    isolate_->CancelTerminateExecution();
  }
  if (!shutting_down_ && app_exit_handler_) {
    app_exit_handler_.Run(exit_code);
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
  } else if (!ConvertV8ToValue(isolate_, context_.Get(isolate_), args_val,
                               &converted, &error, 0, true)) {
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
  } else if (!ConvertV8ToValue(isolate_, context_.Get(isolate_), args_val,
                               &converted, &error, 0, true)) {
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
  } else if (!ConvertV8ToValue(isolate_, context_.Get(isolate_), args_val,
                               &converted, &error, 0, true)) {
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
  const typedArrayNames = [
    'Int8Array', 'Uint8Array', 'Uint8ClampedArray', 'Int16Array', 'Uint16Array',
    'Int32Array', 'Uint32Array', 'Float16Array', 'Float32Array', 'Float64Array',
    'BigInt64Array', 'BigUint64Array'
  ];
  const typedArrays = new Map(typedArrayNames.filter(name =>
      typeof globalThis[name] === 'function').map(name => [name, globalThis[name]]));
  function prepareArgument(value, seen = new Set()) {
    if (value === undefined) return {__xenon_node_wire_type__: 'undefined'};
    if (typeof value === 'bigint') {
      return {__xenon_node_wire_type__: 'bigint', value: String(value)};
    }
    if (!value || typeof value !== 'object') return value;
    if (value instanceof ArrayBuffer || ArrayBuffer.isView(value)) {
      const kind = Buffer.isBuffer(value) ? 'Buffer' :
          value instanceof ArrayBuffer ? 'ArrayBuffer' :
          value instanceof DataView ? 'DataView' :
          Object.prototype.toString.call(value).slice(8, -1);
      if (!['Buffer', 'ArrayBuffer', 'DataView'].includes(kind) &&
          !typedArrays.has(kind)) {
        unsupported('Unsupported native binary kind: ' + kind);
      }
      const bytes = value instanceof ArrayBuffer ? new Uint8Array(value) :
          new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
      return {__xenon_node_wire_type__: 'binary', kind, value: bytes};
    }
    if (seen.has(value)) throw new TypeError('Circular native addon argument');
    seen.add(value);
    try {
      if (typeof value.toJSON === 'function') {
        const json = value.toJSON();
        if (json !== value) return prepareArgument(json, seen);
      }
      if (Array.isArray(value)) return value.map(item => prepareArgument(item, seen));
      const result = {};
      for (const key of Object.keys(value)) {
        Object.defineProperty(result, key, {
          value: prepareArgument(value[key], seen), enumerable: true,
          writable: true, configurable: true
        });
      }
      return result;
    } finally {
      seen.delete(value);
    }
  }
  function unwrapBinary(value) {
    const bytes = value.value;
    if (!ArrayBuffer.isView(bytes)) {
      throw new TypeError('Invalid native binary payload');
    }
    // Only the selected bytes cross the native boundary, so the resulting
    // view owns an exact-sized buffer independent of the sender's allocation.
    const buffer = bytes.byteOffset === 0 &&
        bytes.byteLength === bytes.buffer.byteLength ? bytes.buffer :
        bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength);
    if (value.kind === 'Buffer') return Buffer.from(buffer);
    if (value.kind === 'ArrayBuffer') return buffer;
    if (value.kind === 'DataView') return new DataView(buffer);
    const TypedArray = typedArrays.get(value.kind);
    if (!TypedArray || buffer.byteLength % TypedArray.BYTES_PER_ELEMENT) {
      throw new TypeError('Invalid native binary kind or byte length');
    }
    return new TypedArray(buffer);
  }
  function wrapResult(value) {
    if (Array.isArray(value)) {
      return value.map(wrapResult);
    }
    if (!value || typeof value !== 'object') {
      return value;
    }
    if (value instanceof ArrayBuffer || ArrayBuffer.isView(value)) return value;
    const wireType = value.__xenon_node_wire_type__;
    if (wireType === 'binary') return unwrapBinary(value);
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
    const result = {};
    for (const key of Object.keys(value)) {
      Object.defineProperty(result, key, {
        value: wrapResult(value[key]), enumerable: true,
        writable: true, configurable: true
      });
    }
    return result;
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
              modulePath, instanceId, member.name, prepareArgument(args)));
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
              modulePath, exportPath, prepareArgument(args)));
          if (target.prototype && typeof target.prototype === 'object') {
            Object.setPrototypeOf(instance, target.prototype);
          }
          return instance;
        }
        return wrapResult(__xenonNativeInvokeExport(
            modulePath, exportPath, prepareArgument(args)));
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
    const bool converted_ok =
        reply->preserve_nested_binary
            ? ConvertV8ToValue(isolate_, context_.Get(isolate_), value,
                               &converted, &error, 0, true)
            : V8ToIpcPayload(value, reply->serialized, &converted, &error);
    if (!converted_ok) {
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

void XenonIpcMainContainer::NativeSetTimerReferenced(int id, bool referenced) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto it = timers_.find(id);
  if (it != timers_.end()) {
    it->second->referenced = referenced;
  }
}

void XenonIpcMainContainer::OnTimerTriggered(int id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (shutting_down_ || !initialized_ ||
      (utility_options_ && app_exit_requested_)) {
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
      if (worker_control_ || utility_options_) {
        ReportNodeException(try_catch.Exception());
      }
    }
    if (worker_control_ || utility_options_) {
      isolate_->PerformMicrotaskCheckpoint();
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
