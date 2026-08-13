// Copyright 2026 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifdef UNSAFE_BUFFERS_BUILD
// N-API C ABI shim: many callbacks take raw pointer+length pairs. Spanify
// incrementally; keep allowlist until the C surface is fully wrapped.
#pragma allow_unsafe_buffers
#endif

#include "js_native_api_v8.h"

#include <string.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <limits>
#include <mutex>
#include <new>
#include <thread>
#include <unordered_map>
#include <vector>

#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/task/single_thread_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "xenon_overlay/buildflags/buildflags.h"

#if BUILDFLAG(ENABLE_XENON_NODE_UV_COMPAT)
#include "uv.h"
#endif

struct CallbackData {
  napi_callback cb;
  NAPI_RAW_PTR_EXCLUSION void* data;
  napi_env env;
};

struct NapiFinalizerData {
  napi_env env;
  NAPI_RAW_PTR_EXCLUSION void* data;
  napi_finalize callback;
  NAPI_RAW_PTR_EXCLUSION void* hint;
  v8::Global<v8::Value> persistent;
};

namespace {

class NapiValueScope {
 public:
  explicit NapiValueScope(napi_env env)
      : env_(env), start_value_index_(env->allocated_values.size()) {}

  ~NapiValueScope() {
    if (start_value_index_ < env_->allocated_values.size()) {
      env_->allocated_values.resize(start_value_index_);
    }
  }

 private:
  napi_env env_;
  size_t start_value_index_;
};

v8::Local<v8::Private> GetWrapPrivateKey(napi_env env) {
  return v8::Private::ForApi(
      env->isolate, v8::String::NewFromUtf8Literal(env->isolate, "napi::wrap"));
}

NapiFinalizerData* GetWrapData(napi_env env, v8::Local<v8::Object> object) {
  v8::Local<v8::Value> value;
  if (!object->GetPrivate(env->GetContext(), GetWrapPrivateKey(env))
           .ToLocal(&value) ||
      !value->IsExternal()) {
    return nullptr;
  }
  return static_cast<NapiFinalizerData*>(
      v8::Local<v8::External>::Cast(value)->Value(
          v8::kExternalPointerTypeTagDefault));
}

std::unique_ptr<NapiFinalizerData> TakeFinalizer(NapiFinalizerData* finalizer) {
  auto& finalizers = finalizer->env->finalizers;
  auto item = std::find_if(finalizers.begin(), finalizers.end(),
                           [finalizer](const auto& candidate) {
                             return candidate.get() == finalizer;
                           });
  if (item == finalizers.end()) {
    return nullptr;
  }
  std::unique_ptr<NapiFinalizerData> owned = std::move(*item);
  finalizers.erase(item);
  return owned;
}

void RunNapiFinalizer(NapiFinalizerData* finalizer) {
  std::unique_ptr<NapiFinalizerData> owned = TakeFinalizer(finalizer);
  if (!owned) {
    return;
  }
  owned->persistent.Reset();
  if (owned->callback) {
    NapiValueScope value_scope(owned->env);
    owned->callback(owned->env, owned->data, owned->hint);
  }
}

void OnNapiFinalizerFirstPass(
    const v8::WeakCallbackInfo<NapiFinalizerData>& info) {
  info.GetParameter()->persistent.Reset();
  info.SetSecondPassCallback(
      [](const v8::WeakCallbackInfo<NapiFinalizerData>& second_pass_info) {
        RunNapiFinalizer(second_pass_info.GetParameter());
      });
}

void OnReferenceCollected(const v8::WeakCallbackInfo<napi_ref__>& info) {
  napi_ref ref = info.GetParameter();
  ref->global_value.Reset();
  ref->is_weak = false;
}

}  // namespace

napi_value__::napi_value__() = default;
napi_value__::~napi_value__() {
  persistent.Reset();
}

void napi_value__::Reset(v8::Isolate* isolate, v8::Local<v8::Value> local) {
  isolate_ = isolate;
  persistent.Reset(isolate, local);
}

v8::Local<v8::Value> napi_value__::Get() const {
  return persistent.Get(isolate_);
}

static void CallbackDispatcher(const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Local<v8::Value> data_val = info.Data();
  if (data_val.IsEmpty() || !data_val->IsExternal()) {
    return;
  }
  CallbackData* cbd = static_cast<CallbackData*>(
      v8::Local<v8::External>::Cast(data_val)->Value(
          v8::kExternalPointerTypeTagDefault));

  NapiValueScope value_scope(cbd->env);
  napi_callback_info__ cb_info{info, cbd->data};
  napi_value res = cbd->cb(cbd->env, &cb_info);
  v8::Local<v8::Value> return_value =
      res ? res->Get() : v8::Undefined(cbd->env->isolate);
  info.GetReturnValue().Set(return_value);
}

napi_env__::napi_env__(v8::Isolate* iso, v8::Local<v8::Context> ctx)
    : isolate(iso) {
  context.Reset(isolate, ctx);
}

napi_env__::~napi_env__() {
  while (!finalizers.empty()) {
    std::unique_ptr<NapiFinalizerData> finalizer = std::move(finalizers.back());
    finalizers.pop_back();
    finalizer->persistent.Reset();
    if (finalizer->callback) {
      NapiValueScope value_scope(this);
      finalizer->callback(this, finalizer->data, finalizer->hint);
    }
  }
  callbacks.clear();
  allocated_values.clear();
  context.Reset();
  last_exception.Reset();
}

napi_value napi_env__::CreateValue(v8::Local<v8::Value> local_val) {
  auto val = std::make_unique<napi_value__>();
  val->Reset(isolate, local_val);
  napi_value res = val.get();
  allocated_values.push_back(std::move(val));
  return res;
}

napi_ref__::napi_ref__(v8::Isolate* isolate) : isolate(isolate) {}

napi_ref__::~napi_ref__() {
  global_value.Reset();
}

void napi_ref__::SetWeak() {
  if (global_value.IsEmpty() || is_weak) {
    return;
  }
  global_value.SetWeak(this, OnReferenceCollected,
                       v8::WeakCallbackType::kParameter);
  is_weak = true;
}

void napi_ref__::ClearWeak() {
  if (!global_value.IsEmpty() && is_weak) {
    global_value.ClearWeak();
  }
  is_weak = false;
}

napi_deferred__::napi_deferred__(napi_env env) : env(env) {}
napi_deferred__::~napi_deferred__() {
  resolver.Reset();
}

napi_handle_scope__::napi_handle_scope__(v8::Isolate* isolate, size_t index)
    : v8_scope(isolate), start_value_index(index) {}
napi_handle_scope__::~napi_handle_scope__() = default;

napi_escapable_handle_scope__::napi_escapable_handle_scope__(v8::Isolate* isolate, size_t index)
    : v8_scope(isolate), start_value_index(index) {}
napi_escapable_handle_scope__::~napi_escapable_handle_scope__() = default;

namespace v8impl {
}  // namespace v8impl

// --- Scope ---

napi_status napi_open_handle_scope(napi_env env, napi_handle_scope* result) {
  if (!env || !result) return napi_invalid_arg;
  auto* scope = new napi_handle_scope__(env->isolate, env->allocated_values.size());
  *result = scope;
  return napi_ok;
}

napi_status napi_close_handle_scope(napi_env env, napi_handle_scope scope) {
  if (!env || !scope) return napi_invalid_arg;
  if (scope->start_value_index < env->allocated_values.size()) {
    env->allocated_values.resize(scope->start_value_index);
  }
  delete scope;
  return napi_ok;
}

napi_status napi_open_escapable_handle_scope(napi_env env, napi_escapable_handle_scope* result) {
  if (!env || !result) return napi_invalid_arg;
  auto* scope = new napi_escapable_handle_scope__(env->isolate, env->allocated_values.size());
  *result = scope;
  return napi_ok;
}

napi_status napi_close_escapable_handle_scope(napi_env env, napi_escapable_handle_scope scope) {
  if (!env || !scope) return napi_invalid_arg;
  if (scope->start_value_index < env->allocated_values.size()) {
    env->allocated_values.resize(scope->start_value_index);
  }
  if (scope->escaped_value) {
    env->allocated_values.push_back(std::move(scope->escaped_value));
  }
  delete scope;
  return napi_ok;
}

napi_status napi_escape_handle(napi_env env, napi_escapable_handle_scope scope, napi_value escapee, napi_value* result) {
  if (!env || !scope || !escapee || !result) return napi_invalid_arg;
  if (scope->escaped_value) {
    return napi_escape_called_twice;
  }
  v8::Local<v8::Value> escaped = scope->v8_scope.Escape(escapee->Get());
  scope->escaped_value = std::make_unique<napi_value__>();
  scope->escaped_value->Reset(env->isolate, escaped);
  *result = scope->escaped_value.get();
  return napi_ok;
}

// --- Type Creation ---

napi_status napi_get_undefined(napi_env env, napi_value* result) {
  if (!env || !result) return napi_invalid_arg;
  *result = env->CreateValue(v8::Undefined(env->isolate));
  return napi_ok;
}

napi_status napi_get_null(napi_env env, napi_value* result) {
  if (!env || !result) return napi_invalid_arg;
  *result = env->CreateValue(v8::Null(env->isolate));
  return napi_ok;
}

napi_status napi_get_global(napi_env env, napi_value* result) {
  if (!env || !result) return napi_invalid_arg;
  *result = env->CreateValue(env->GetContext()->Global());
  return napi_ok;
}

napi_status napi_get_boolean(napi_env env, bool value, napi_value* result) {
  if (!env || !result) return napi_invalid_arg;
  *result = env->CreateValue(v8::Boolean::New(env->isolate, value));
  return napi_ok;
}

napi_status napi_create_bigint_int64(napi_env env,
                                     int64_t value,
                                     napi_value* result) {
  if (!env || !result) {
    return napi_invalid_arg;
  }
  *result = env->CreateValue(v8::BigInt::New(env->isolate, value));
  return napi_ok;
}

napi_status napi_create_bigint_uint64(napi_env env, uint64_t value, napi_value* result) {
  if (!env || !result) return napi_invalid_arg;
  *result = env->CreateValue(v8::BigInt::NewFromUnsigned(env->isolate, value));
  return napi_ok;
}

napi_status napi_create_double(napi_env env, double value, napi_value* result) {
  if (!env || !result) return napi_invalid_arg;
  *result = env->CreateValue(v8::Number::New(env->isolate, value));
  return napi_ok;
}

napi_status napi_create_int32(napi_env env, int32_t value, napi_value* result) {
  if (!env || !result) return napi_invalid_arg;
  *result = env->CreateValue(v8::Integer::New(env->isolate, value));
  return napi_ok;
}

napi_status napi_create_uint32(napi_env env, uint32_t value, napi_value* result) {
  if (!env || !result) return napi_invalid_arg;
  *result = env->CreateValue(v8::Integer::NewFromUnsigned(env->isolate, value));
  return napi_ok;
}

napi_status napi_create_int64(napi_env env, int64_t value, napi_value* result) {
  if (!env || !result) return napi_invalid_arg;
  *result = env->CreateValue(v8::Number::New(env->isolate, static_cast<double>(value)));
  return napi_ok;
}

napi_status napi_create_string_latin1(napi_env env,
                                      const char* str,
                                      size_t length,
                                      napi_value* result) {
  if (!env || !str || !result) {
    return napi_invalid_arg;
  }
  if (length != NAPI_AUTO_LENGTH &&
      length > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return napi_invalid_arg;
  }
  v8::Local<v8::String> value;
  if (!v8::String::NewFromOneByte(
           env->isolate, reinterpret_cast<const uint8_t*>(str),
           v8::NewStringType::kNormal,
           length == NAPI_AUTO_LENGTH ? -1 : static_cast<int>(length))
           .ToLocal(&value)) {
    return napi_generic_failure;
  }
  *result = env->CreateValue(value);
  return napi_ok;
}

napi_status napi_create_string_utf8(napi_env env, const char* str, size_t length, napi_value* result) {
  if (!env || !str || !result) {
    return napi_invalid_arg;
  }
  if (length != NAPI_AUTO_LENGTH &&
      length > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return napi_invalid_arg;
  }
  v8::MaybeLocal<v8::String> v8_str = v8::String::NewFromUtf8(
      env->isolate, str, v8::NewStringType::kNormal,
      length == NAPI_AUTO_LENGTH ? -1 : static_cast<int>(length));
  v8::Local<v8::String> local_str;
  if (!v8_str.ToLocal(&local_str)) {
    return napi_generic_failure;
  }
  *result = env->CreateValue(local_str);
  return napi_ok;
}

napi_status napi_create_string_utf16(napi_env env,
                                     const char16_t* str,
                                     size_t length,
                                     napi_value* result) {
  if (!env || !str || !result) {
    return napi_invalid_arg;
  }
  if (length != NAPI_AUTO_LENGTH &&
      length > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return napi_invalid_arg;
  }
  const int string_length =
      length == NAPI_AUTO_LENGTH ? -1 : static_cast<int>(length);
  v8::Local<v8::String> value;
  if (!v8::String::NewFromTwoByte(env->isolate,
                                  reinterpret_cast<const uint16_t*>(str),
                                  v8::NewStringType::kNormal, string_length)
           .ToLocal(&value)) {
    return napi_generic_failure;
  }
  *result = env->CreateValue(value);
  return napi_ok;
}

napi_status napi_create_symbol(napi_env env, napi_value description, napi_value* result) {
  if (!env || !result) return napi_invalid_arg;
  v8::Local<v8::String> desc_str;
  if (description) {
    if (!description->Get()->IsString()) {
      return napi_string_expected;
    }
    desc_str = description->Get().As<v8::String>();
  }
  *result = env->CreateValue(v8::Symbol::New(env->isolate, desc_str));
  return napi_ok;
}

napi_status napi_create_object(napi_env env, napi_value* result) {
  if (!env || !result) return napi_invalid_arg;
  *result = env->CreateValue(v8::Object::New(env->isolate));
  return napi_ok;
}

napi_status napi_create_array(napi_env env, napi_value* result) {
  if (!env || !result) return napi_invalid_arg;
  *result = env->CreateValue(v8::Array::New(env->isolate));
  return napi_ok;
}

napi_status napi_create_array_with_length(napi_env env, size_t length, napi_value* result) {
  if (!env || !result) return napi_invalid_arg;
  if (length > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return napi_invalid_arg;
  }
  *result = env->CreateValue(v8::Array::New(env->isolate, static_cast<int>(length)));
  return napi_ok;
}

napi_status napi_create_arraybuffer(napi_env env, size_t byte_length, void** data, napi_value* result) {
  if (!env || !result) return napi_invalid_arg;
  std::unique_ptr<v8::BackingStore> backing_store =
      v8::ArrayBuffer::NewBackingStore(env->isolate, byte_length);
  void* backing_data = backing_store->Data();
  v8::Local<v8::ArrayBuffer> array_buffer =
      v8::ArrayBuffer::New(env->isolate, std::move(backing_store));
  if (data) {
    *data = backing_data;
  }
  *result = env->CreateValue(array_buffer);
  return napi_ok;
}

namespace {

size_t TypedArrayElementSize(napi_typedarray_type type) {
  switch (type) {
    case napi_int8_array:
    case napi_uint8_array:
    case napi_uint8_clamped_array:
      return 1;
    case napi_int16_array:
    case napi_uint16_array:
      return 2;
    case napi_int32_array:
    case napi_uint32_array:
    case napi_float32_array:
      return 4;
    case napi_float64_array:
    case napi_bigint64_array:
    case napi_biguint64_array:
      return 8;
  }
  return 0;
}

v8::Local<v8::TypedArray> CreateTypedArray(v8::Local<v8::ArrayBuffer> buffer,
                                           napi_typedarray_type type,
                                           size_t byte_offset,
                                           size_t length) {
  switch (type) {
    case napi_int8_array:
      return v8::Int8Array::New(buffer, byte_offset, length);
    case napi_uint8_array:
      return v8::Uint8Array::New(buffer, byte_offset, length);
    case napi_uint8_clamped_array:
      return v8::Uint8ClampedArray::New(buffer, byte_offset, length);
    case napi_int16_array:
      return v8::Int16Array::New(buffer, byte_offset, length);
    case napi_uint16_array:
      return v8::Uint16Array::New(buffer, byte_offset, length);
    case napi_int32_array:
      return v8::Int32Array::New(buffer, byte_offset, length);
    case napi_uint32_array:
      return v8::Uint32Array::New(buffer, byte_offset, length);
    case napi_float32_array:
      return v8::Float32Array::New(buffer, byte_offset, length);
    case napi_float64_array:
      return v8::Float64Array::New(buffer, byte_offset, length);
    case napi_bigint64_array:
      return v8::BigInt64Array::New(buffer, byte_offset, length);
    case napi_biguint64_array:
      return v8::BigUint64Array::New(buffer, byte_offset, length);
  }
  return {};
}

bool GetTypedArrayType(v8::Local<v8::Value> value, napi_typedarray_type* type) {
  if (value->IsInt8Array()) {
    *type = napi_int8_array;
  } else if (value->IsUint8Array()) {
    *type = napi_uint8_array;
  } else if (value->IsUint8ClampedArray()) {
    *type = napi_uint8_clamped_array;
  } else if (value->IsInt16Array()) {
    *type = napi_int16_array;
  } else if (value->IsUint16Array()) {
    *type = napi_uint16_array;
  } else if (value->IsInt32Array()) {
    *type = napi_int32_array;
  } else if (value->IsUint32Array()) {
    *type = napi_uint32_array;
  } else if (value->IsFloat32Array()) {
    *type = napi_float32_array;
  } else if (value->IsFloat64Array()) {
    *type = napi_float64_array;
  } else if (value->IsBigInt64Array()) {
    *type = napi_bigint64_array;
  } else if (value->IsBigUint64Array()) {
    *type = napi_biguint64_array;
  } else {
    return false;
  }
  return true;
}

}  // namespace

napi_status napi_create_typedarray(napi_env env,
                                   napi_typedarray_type type,
                                   size_t length,
                                   napi_value arraybuffer,
                                   size_t byte_offset,
                                   napi_value* result) {
  if (!env || !arraybuffer || !result) {
    return napi_invalid_arg;
  }
  if (!arraybuffer->Get()->IsArrayBuffer()) {
    return napi_arraybuffer_expected;
  }
  const size_t element_size = TypedArrayElementSize(type);
  if (element_size == 0) {
    return napi_invalid_arg;
  }
  v8::Local<v8::ArrayBuffer> buffer = arraybuffer->Get().As<v8::ArrayBuffer>();
  if (byte_offset % element_size != 0 || byte_offset > buffer->ByteLength() ||
      length > (buffer->ByteLength() - byte_offset) / element_size) {
    return napi_invalid_arg;
  }
  *result =
      env->CreateValue(CreateTypedArray(buffer, type, byte_offset, length));
  return napi_ok;
}

napi_status napi_create_dataview(napi_env env,
                                 size_t length,
                                 napi_value arraybuffer,
                                 size_t byte_offset,
                                 napi_value* result) {
  if (!env || !arraybuffer || !result) {
    return napi_invalid_arg;
  }
  if (!arraybuffer->Get()->IsArrayBuffer()) {
    return napi_arraybuffer_expected;
  }
  v8::Local<v8::ArrayBuffer> buffer = arraybuffer->Get().As<v8::ArrayBuffer>();
  if (byte_offset > buffer->ByteLength() ||
      length > buffer->ByteLength() - byte_offset) {
    return napi_invalid_arg;
  }
  *result = env->CreateValue(v8::DataView::New(buffer, byte_offset, length));
  return napi_ok;
}

napi_status napi_create_date(napi_env env, double time, napi_value* result) {
  if (!env || !result) {
    return napi_invalid_arg;
  }
  v8::Local<v8::Value> date;
  if (!v8::Date::New(env->GetContext(), time).ToLocal(&date)) {
    return napi_generic_failure;
  }
  *result = env->CreateValue(date);
  return napi_ok;
}

napi_status napi_create_promise(napi_env env, napi_deferred* deferred, napi_value* promise) {
  if (!env || !deferred || !promise) return napi_invalid_arg;
  v8::Local<v8::Context> context = env->GetContext();
  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(context).ToLocal(&resolver)) {
    return napi_generic_failure;
  }

  auto* local_deferred = new napi_deferred__(env);
  local_deferred->resolver.Reset(env->isolate, resolver);
  *deferred = local_deferred;
  *promise = env->CreateValue(resolver->GetPromise());
  return napi_ok;
}

napi_status napi_resolve_deferred(napi_env env, napi_deferred deferred, napi_value resolution) {
  if (!env || !deferred) return napi_invalid_arg;
  v8::Local<v8::Context> context = env->GetContext();
  v8::Local<v8::Promise::Resolver> resolver =
      deferred->resolver.Get(env->isolate);
  v8::Local<v8::Value> value =
      resolution ? resolution->Get() : v8::Undefined(env->isolate);
  bool resolved = resolver->Resolve(context, value).FromMaybe(false);
  delete deferred;
  return resolved ? napi_ok : napi_generic_failure;
}

napi_status napi_reject_deferred(napi_env env, napi_deferred deferred, napi_value rejection) {
  if (!env || !deferred) return napi_invalid_arg;
  v8::Local<v8::Context> context = env->GetContext();
  v8::Local<v8::Promise::Resolver> resolver =
      deferred->resolver.Get(env->isolate);
  v8::Local<v8::Value> value =
      rejection ? rejection->Get() : v8::Undefined(env->isolate);
  bool rejected = resolver->Reject(context, value).FromMaybe(false);
  delete deferred;
  return rejected ? napi_ok : napi_generic_failure;
}

// --- Type Checking ---

napi_status napi_typeof(napi_env env, napi_value value, napi_valuetype* result) {
  if (!env || !value || !result) return napi_invalid_arg;
  v8::Local<v8::Value> val = value->Get();
  if (val->IsUndefined()) *result = napi_undefined;
  else if (val->IsNull()) *result = napi_null;
  else if (val->IsBoolean()) *result = napi_boolean;
  else if (val->IsNumber()) *result = napi_number;
  else if (val->IsString()) *result = napi_string;
  else if (val->IsSymbol()) *result = napi_symbol;
  else if (val->IsFunction()) *result = napi_function;
  else if (val->IsArray()) *result = napi_object;
  else if (val->IsExternal()) *result = napi_external;
  else if (val->IsBigInt()) *result = napi_bigint;
  else if (val->IsObject()) *result = napi_object;
  else *result = napi_object;
  return napi_ok;
}

napi_status napi_coerce_to_bool(napi_env env, napi_value value, napi_value* result) {
  if (!env || !value || !result) return napi_invalid_arg;
  *result = env->CreateValue(value->Get()->ToBoolean(env->isolate));
  return napi_ok;
}

napi_status napi_coerce_to_number(napi_env env, napi_value value, napi_value* result) {
  if (!env || !value || !result) return napi_invalid_arg;
  v8::Local<v8::Context> context = env->GetContext();
  v8::Local<v8::Number> val;
  if (!value->Get()->ToNumber(context).ToLocal(&val)) return napi_generic_failure;
  *result = env->CreateValue(val);
  return napi_ok;
}

napi_status napi_coerce_to_object(napi_env env, napi_value value, napi_value* result) {
  if (!env || !value || !result) return napi_invalid_arg;
  v8::Local<v8::Context> context = env->GetContext();
  v8::Local<v8::Object> val;
  if (!value->Get()->ToObject(context).ToLocal(&val)) return napi_generic_failure;
  *result = env->CreateValue(val);
  return napi_ok;
}

napi_status napi_coerce_to_string(napi_env env, napi_value value, napi_value* result) {
  if (!env || !value || !result) return napi_invalid_arg;
  v8::Local<v8::Context> context = env->GetContext();
  v8::Local<v8::String> val;
  if (!value->Get()->ToString(context).ToLocal(&val)) return napi_generic_failure;
  *result = env->CreateValue(val);
  return napi_ok;
}

napi_status napi_is_error(napi_env env, napi_value value, bool* result) {
  if (!env || !value || !result) {
    return napi_invalid_arg;
  }
  *result = value->Get()->IsNativeError();
  return napi_ok;
}

napi_status napi_is_promise(napi_env env, napi_value value, bool* result) {
  if (!env || !value || !result) {
    return napi_invalid_arg;
  }
  *result = value->Get()->IsPromise();
  return napi_ok;
}

napi_status napi_is_date(napi_env env, napi_value value, bool* result) {
  if (!env || !value || !result) {
    return napi_invalid_arg;
  }
  *result = value->Get()->IsDate();
  return napi_ok;
}

napi_status napi_is_typedarray(napi_env env, napi_value value, bool* result) {
  if (!env || !value || !result) {
    return napi_invalid_arg;
  }
  *result = value->Get()->IsTypedArray();
  return napi_ok;
}

napi_status napi_is_dataview(napi_env env, napi_value value, bool* result) {
  if (!env || !value || !result) {
    return napi_invalid_arg;
  }
  *result = value->Get()->IsDataView();
  return napi_ok;
}

// --- Get Value From N-API representation ---

napi_status napi_get_value_double(napi_env env, napi_value value, double* result) {
  if (!env || !value || !result) return napi_invalid_arg;
  if (!value->Get()->IsNumber()) {
    return napi_number_expected;
  }
  *result = value->Get().As<v8::Number>()->Value();
  return napi_ok;
}

napi_status napi_get_value_int32(napi_env env, napi_value value, int32_t* result) {
  if (!env || !value || !result) return napi_invalid_arg;
  if (!value->Get()->IsNumber()) {
    return napi_number_expected;
  }
  *result = value->Get()->Int32Value(env->GetContext()).FromMaybe(0);
  return napi_ok;
}

napi_status napi_get_value_uint32(napi_env env, napi_value value, uint32_t* result) {
  if (!env || !value || !result) return napi_invalid_arg;
  if (!value->Get()->IsNumber()) {
    return napi_number_expected;
  }
  *result = value->Get()->Uint32Value(env->GetContext()).FromMaybe(0);
  return napi_ok;
}

napi_status napi_get_value_int64(napi_env env, napi_value value, int64_t* result) {
  if (!env || !value || !result) return napi_invalid_arg;
  if (!value->Get()->IsNumber()) {
    return napi_number_expected;
  }
  *result = static_cast<int64_t>(value->Get().As<v8::Number>()->Value());
  return napi_ok;
}

napi_status napi_get_value_bool(napi_env env, napi_value value, bool* result) {
  if (!env || !value || !result) return napi_invalid_arg;
  if (!value->Get()->IsBoolean()) {
    return napi_boolean_expected;
  }
  *result = value->Get().As<v8::Boolean>()->Value();
  return napi_ok;
}

napi_status napi_get_value_bigint_int64(napi_env env, napi_value value, int64_t* result, bool* lossless) {
  if (!env || !value || !result) return napi_invalid_arg;
  if (!value->Get()->IsBigInt()) return napi_bigint_expected;
  bool local_lossless = false;
  *result = v8::Local<v8::BigInt>::Cast(value->Get())->Int64Value(&local_lossless);
  if (lossless) {
    *lossless = local_lossless;
  }
  return napi_ok;
}

napi_status napi_get_value_bigint_uint64(napi_env env, napi_value value, uint64_t* result, bool* lossless) {
  if (!env || !value || !result) return napi_invalid_arg;
  if (!value->Get()->IsBigInt()) return napi_bigint_expected;
  bool local_lossless = false;
  *result = v8::Local<v8::BigInt>::Cast(value->Get())->Uint64Value(&local_lossless);
  if (lossless) {
    *lossless = local_lossless;
  }
  return napi_ok;
}

napi_status napi_get_value_string_utf8(napi_env env, napi_value value, char* buf, size_t bufsize, size_t* result) {
  if (!env || !value) return napi_invalid_arg;
  if (!value->Get()->IsString()) {
    return napi_string_expected;
  }
  v8::Local<v8::String> str = value->Get().As<v8::String>();

  if (!buf) {
    if (result) {
      *result = str->Utf8LengthV2(env->isolate);
    }
    return napi_ok;
  }
  if (bufsize == 0) {
    if (result) {
      *result = 0;
    }
    return napi_ok;
  }

  const size_t written =
      str->WriteUtf8V2(env->isolate, buf, bufsize,
                       v8::String::WriteFlags::kReplaceInvalidUtf8 |
                           v8::String::WriteFlags::kNullTerminate);
  if (result) {
    *result = written > 0 ? written - 1 : 0;
  }
  return napi_ok;
}

napi_status napi_get_value_string_latin1(napi_env env,
                                         napi_value value,
                                         char* buf,
                                         size_t bufsize,
                                         size_t* result) {
  if (!env || !value) {
    return napi_invalid_arg;
  }
  if (!value->Get()->IsString()) {
    return napi_string_expected;
  }
  v8::Local<v8::String> string = value->Get().As<v8::String>();
  if (!buf) {
    if (result) {
      *result = string->Length();
    }
    return napi_ok;
  }
  const uint32_t written = static_cast<uint32_t>(std::min(
      {bufsize > 0 ? bufsize - 1 : 0, static_cast<size_t>(string->Length()),
       static_cast<size_t>(std::numeric_limits<uint32_t>::max())}));
  string->WriteOneByteV2(env->isolate, 0, written,
                         reinterpret_cast<uint8_t*>(buf));
  if (bufsize > 0) {
    buf[written] = '\0';
  }
  if (result) {
    *result = written;
  }
  return napi_ok;
}

napi_status napi_get_value_string_utf16(napi_env env,
                                        napi_value value,
                                        char16_t* buf,
                                        size_t bufsize,
                                        size_t* result) {
  if (!env || !value) {
    return napi_invalid_arg;
  }
  if (!value->Get()->IsString()) {
    return napi_string_expected;
  }
  v8::Local<v8::String> string = value->Get().As<v8::String>();
  if (!buf) {
    if (result) {
      *result = string->Length();
    }
    return napi_ok;
  }
  const uint32_t written = static_cast<uint32_t>(std::min(
      {bufsize > 0 ? bufsize - 1 : 0, static_cast<size_t>(string->Length()),
       static_cast<size_t>(std::numeric_limits<uint32_t>::max())}));
  string->WriteV2(env->isolate, 0, written, reinterpret_cast<uint16_t*>(buf));
  if (bufsize > 0) {
    buf[written] = 0;
  }
  if (result) {
    *result = written;
  }
  return napi_ok;
}

napi_status napi_get_date_value(napi_env env,
                                napi_value value,
                                double* result) {
  if (!env || !value || !result) {
    return napi_invalid_arg;
  }
  if (!value->Get()->IsDate()) {
    return napi_date_expected;
  }
  *result = value->Get().As<v8::Date>()->ValueOf();
  return napi_ok;
}

// --- Object Properties ---

napi_status napi_get_property_names(napi_env env, napi_value object, napi_value* result) {
  if (!env || !object || !result) return napi_invalid_arg;
  v8::Local<v8::Context> context = env->GetContext();
  v8::Local<v8::Object> obj;
  if (!object->Get()->ToObject(context).ToLocal(&obj)) return napi_object_expected;
  v8::Local<v8::Array> keys;
  if (!obj->GetPropertyNames(context).ToLocal(&keys)) return napi_generic_failure;
  *result = env->CreateValue(keys);
  return napi_ok;
}

napi_status napi_set_property(napi_env env, napi_value object, napi_value key, napi_value value) {
  if (!env || !object || !key || !value) return napi_invalid_arg;
  v8::Local<v8::Context> context = env->GetContext();
  v8::Local<v8::Object> obj;
  if (!object->Get()->ToObject(context).ToLocal(&obj)) return napi_object_expected;
  if (!obj->Set(context, key->Get(), value->Get()).FromMaybe(false)) return napi_generic_failure;
  return napi_ok;
}

napi_status napi_has_property(napi_env env, napi_value object, napi_value key, bool* result) {
  if (!env || !object || !key || !result) return napi_invalid_arg;
  v8::Local<v8::Context> context = env->GetContext();
  v8::Local<v8::Object> obj;
  if (!object->Get()->ToObject(context).ToLocal(&obj)) return napi_object_expected;
  *result = obj->Has(context, key->Get()).FromMaybe(false);
  return napi_ok;
}

napi_status napi_get_property(napi_env env, napi_value object, napi_value key, napi_value* result) {
  if (!env || !object || !key || !result) return napi_invalid_arg;
  v8::Local<v8::Context> context = env->GetContext();
  v8::Local<v8::Object> obj;
  if (!object->Get()->ToObject(context).ToLocal(&obj)) return napi_object_expected;
  v8::Local<v8::Value> val;
  if (!obj->Get(context, key->Get()).ToLocal(&val)) return napi_generic_failure;
  *result = env->CreateValue(val);
  return napi_ok;
}

napi_status napi_delete_property(napi_env env, napi_value object, napi_value key, bool* result) {
  if (!env || !object || !key) return napi_invalid_arg;
  v8::Local<v8::Context> context = env->GetContext();
  v8::Local<v8::Object> obj;
  if (!object->Get()->ToObject(context).ToLocal(&obj)) return napi_object_expected;
  bool deleted = obj->Delete(context, key->Get()).FromMaybe(false);
  if (result) *result = deleted;
  return napi_ok;
}

napi_status napi_has_own_property(napi_env env, napi_value object, napi_value key, bool* result) {
  if (!env || !object || !key || !result) return napi_invalid_arg;
  v8::Local<v8::Context> context = env->GetContext();
  v8::Local<v8::Object> obj;
  if (!object->Get()->ToObject(context).ToLocal(&obj)) return napi_object_expected;
  if (!key->Get()->IsName()) {
    return napi_name_expected;
  }
  *result =
      obj->HasOwnProperty(context, key->Get().As<v8::Name>()).FromMaybe(false);
  return napi_ok;
}

napi_status napi_set_named_property(napi_env env, napi_value object, const char* utf8name, napi_value value) {
  if (!env || !object || !utf8name || !value) return napi_invalid_arg;
  v8::Local<v8::Context> context = env->GetContext();
  v8::Local<v8::Object> obj;
  if (!object->Get()->ToObject(context).ToLocal(&obj)) return napi_object_expected;
  v8::Local<v8::String> key = v8::String::NewFromUtf8(env->isolate, utf8name, v8::NewStringType::kNormal).ToLocalChecked();
  if (!obj->Set(context, key, value->Get()).FromMaybe(false)) return napi_generic_failure;
  return napi_ok;
}

napi_status napi_has_named_property(napi_env env, napi_value object, const char* utf8name, bool* result) {
  if (!env || !object || !utf8name || !result) return napi_invalid_arg;
  v8::Local<v8::Context> context = env->GetContext();
  v8::Local<v8::Object> obj;
  if (!object->Get()->ToObject(context).ToLocal(&obj)) return napi_object_expected;
  v8::Local<v8::String> key = v8::String::NewFromUtf8(env->isolate, utf8name, v8::NewStringType::kNormal).ToLocalChecked();
  *result = obj->Has(context, key).FromMaybe(false);
  return napi_ok;
}

napi_status napi_get_named_property(napi_env env, napi_value object, const char* utf8name, napi_value* result) {
  if (!env || !object || !utf8name || !result) return napi_invalid_arg;
  v8::Local<v8::Context> context = env->GetContext();
  v8::Local<v8::Object> obj;
  if (!object->Get()->ToObject(context).ToLocal(&obj)) return napi_object_expected;
  v8::Local<v8::String> key = v8::String::NewFromUtf8(env->isolate, utf8name, v8::NewStringType::kNormal).ToLocalChecked();
  v8::Local<v8::Value> val;
  if (!obj->Get(context, key).ToLocal(&val)) return napi_generic_failure;
  *result = env->CreateValue(val);
  return napi_ok;
}

napi_status napi_set_element(napi_env env, napi_value object, uint32_t index, napi_value value) {
  if (!env || !object || !value) return napi_invalid_arg;
  v8::Local<v8::Context> context = env->GetContext();
  v8::Local<v8::Object> obj;
  if (!object->Get()->ToObject(context).ToLocal(&obj)) return napi_object_expected;
  if (!obj->Set(context, index, value->Get()).FromMaybe(false)) return napi_generic_failure;
  return napi_ok;
}

napi_status napi_has_element(napi_env env, napi_value object, uint32_t index, bool* result) {
  if (!env || !object || !result) return napi_invalid_arg;
  v8::Local<v8::Context> context = env->GetContext();
  v8::Local<v8::Object> obj;
  if (!object->Get()->ToObject(context).ToLocal(&obj)) return napi_object_expected;
  *result = obj->Has(context, index).FromMaybe(false);
  return napi_ok;
}

napi_status napi_get_element(napi_env env, napi_value object, uint32_t index, napi_value* result) {
  if (!env || !object || !result) return napi_invalid_arg;
  v8::Local<v8::Context> context = env->GetContext();
  v8::Local<v8::Object> obj;
  if (!object->Get()->ToObject(context).ToLocal(&obj)) return napi_object_expected;
  v8::Local<v8::Value> val;
  if (!obj->Get(context, index).ToLocal(&val)) return napi_generic_failure;
  *result = env->CreateValue(val);
  return napi_ok;
}

napi_status napi_delete_element(napi_env env, napi_value object, uint32_t index, bool* result) {
  if (!env || !object) return napi_invalid_arg;
  v8::Local<v8::Context> context = env->GetContext();
  v8::Local<v8::Object> obj;
  if (!object->Get()->ToObject(context).ToLocal(&obj)) return napi_object_expected;
  bool deleted = obj->Delete(context, index).FromMaybe(false);
  if (result) *result = deleted;
  return napi_ok;
}

napi_status napi_define_properties(napi_env env, napi_value object, size_t property_count, const napi_property_descriptor* properties) {
  if (!env || !object || (property_count > 0 && !properties)) {
    return napi_invalid_arg;
  }
  v8::Local<v8::Context> context = env->GetContext();
  v8::Local<v8::Object> obj;
  if (!object->Get()->ToObject(context).ToLocal(&obj)) return napi_object_expected;

  for (size_t i = 0; i < property_count; ++i) {
    const napi_property_descriptor& desc = properties[i];
    v8::Local<v8::Name> name;
    if (desc.name) {
      name = v8::Local<v8::Name>::Cast(desc.name->Get());
    } else if (desc.utf8name) {
      name = v8::String::NewFromUtf8(env->isolate, desc.utf8name, v8::NewStringType::kNormal).ToLocalChecked();
    } else {
      return napi_invalid_arg;
    }

    v8::PropertyAttribute attributes = v8::None;
    if (!(desc.attributes & napi_writable)) attributes = static_cast<v8::PropertyAttribute>(attributes | v8::ReadOnly);
    if (!(desc.attributes & napi_enumerable)) attributes = static_cast<v8::PropertyAttribute>(attributes | v8::DontEnum);
    if (!(desc.attributes & napi_configurable)) attributes = static_cast<v8::PropertyAttribute>(attributes | v8::DontDelete);

    if (desc.method) {
      auto cbd = std::make_unique<CallbackData>(CallbackData{desc.method, desc.data, env});
      v8::Local<v8::External> ext = v8::External::New(
          env->isolate, cbd.get(), v8::kExternalPointerTypeTagDefault);
      env->callbacks.push_back(std::move(cbd));
      
      v8::Local<v8::Function> fn;
      if (!v8::Function::New(context, CallbackDispatcher, ext).ToLocal(&fn)) return napi_generic_failure;
      if (!obj->DefineOwnProperty(context, name, fn, attributes).FromMaybe(false)) return napi_generic_failure;
    } else if (desc.getter || desc.setter) {
      v8::Local<v8::Function> local_getter;
      v8::Local<v8::Function> local_setter;

      if (desc.getter) {
        auto cbd = std::make_unique<CallbackData>(CallbackData{desc.getter, desc.data, env});
        v8::Local<v8::External> ext = v8::External::New(
            env->isolate, cbd.get(), v8::kExternalPointerTypeTagDefault);
        env->callbacks.push_back(std::move(cbd));
        if (!v8::Function::New(context, CallbackDispatcher, ext).ToLocal(&local_getter)) return napi_generic_failure;
      }
      if (desc.setter) {
        auto cbd = std::make_unique<CallbackData>(CallbackData{desc.setter, desc.data, env});
        v8::Local<v8::External> ext = v8::External::New(
            env->isolate, cbd.get(), v8::kExternalPointerTypeTagDefault);
        env->callbacks.push_back(std::move(cbd));
        if (!v8::Function::New(context, CallbackDispatcher, ext).ToLocal(&local_setter)) return napi_generic_failure;
      }

      obj->SetAccessorProperty(name, local_getter, local_setter, attributes);
    } else if (desc.value) {
      if (!obj->DefineOwnProperty(context, name, desc.value->Get(), attributes).FromMaybe(false)) return napi_generic_failure;
    }
  }
  return napi_ok;
}

napi_status napi_get_prototype(napi_env env,
                               napi_value object,
                               napi_value* result) {
  if (!env || !object || !result) {
    return napi_invalid_arg;
  }
  if (!object->Get()->IsObject()) {
    return napi_object_expected;
  }
  *result = env->CreateValue(object->Get().As<v8::Object>()->GetPrototype());
  return napi_ok;
}

// --- Array Methods ---

napi_status napi_is_array(napi_env env, napi_value value, bool* result) {
  if (!env || !value || !result) return napi_invalid_arg;
  *result = value->Get()->IsArray();
  return napi_ok;
}

napi_status napi_get_array_length(napi_env env, napi_value value, uint32_t* result) {
  if (!env || !value || !result) return napi_invalid_arg;
  if (!value->Get()->IsArray()) return napi_array_expected;
  *result = v8::Local<v8::Array>::Cast(value->Get())->Length();
  return napi_ok;
}

napi_status napi_is_arraybuffer(napi_env env, napi_value value, bool* result) {
  if (!env || !value || !result) return napi_invalid_arg;
  *result = value->Get()->IsArrayBuffer();
  return napi_ok;
}

napi_status napi_get_arraybuffer_info(napi_env env, napi_value arraybuffer, void** data, size_t* byte_length) {
  if (!env || !arraybuffer) return napi_invalid_arg;
  if (!arraybuffer->Get()->IsArrayBuffer()) return napi_arraybuffer_expected;
  v8::Local<v8::ArrayBuffer> buffer =
      v8::Local<v8::ArrayBuffer>::Cast(arraybuffer->Get());
  std::shared_ptr<v8::BackingStore> backing_store = buffer->GetBackingStore();
  if (data) {
    *data = backing_store->Data();
  }
  if (byte_length) {
    *byte_length = buffer->ByteLength();
  }
  return napi_ok;
}

napi_status napi_get_typedarray_info(napi_env env,
                                     napi_value typedarray,
                                     napi_typedarray_type* type,
                                     size_t* length,
                                     void** data,
                                     napi_value* arraybuffer,
                                     size_t* byte_offset) {
  if (!env || !typedarray) {
    return napi_invalid_arg;
  }
  napi_typedarray_type detected_type;
  if (!GetTypedArrayType(typedarray->Get(), &detected_type)) {
    return napi_invalid_arg;
  }
  v8::Local<v8::TypedArray> array = typedarray->Get().As<v8::TypedArray>();
  if (type) {
    *type = detected_type;
  }
  if (length) {
    *length = array->Length();
  }
  if (byte_offset) {
    *byte_offset = array->ByteOffset();
  }
  if (data) {
    *data = static_cast<uint8_t*>(array->Buffer()->GetBackingStore()->Data()) +
            array->ByteOffset();
  }
  if (arraybuffer) {
    *arraybuffer = env->CreateValue(array->Buffer());
  }
  return napi_ok;
}

napi_status napi_get_dataview_info(napi_env env,
                                   napi_value dataview,
                                   size_t* byte_length,
                                   void** data,
                                   napi_value* arraybuffer,
                                   size_t* byte_offset) {
  if (!env || !dataview) {
    return napi_invalid_arg;
  }
  if (!dataview->Get()->IsDataView()) {
    return napi_invalid_arg;
  }
  v8::Local<v8::DataView> view = dataview->Get().As<v8::DataView>();
  if (byte_length) {
    *byte_length = view->ByteLength();
  }
  if (byte_offset) {
    *byte_offset = view->ByteOffset();
  }
  if (data) {
    *data = static_cast<uint8_t*>(view->Buffer()->GetBackingStore()->Data()) +
            view->ByteOffset();
  }
  if (arraybuffer) {
    *arraybuffer = env->CreateValue(view->Buffer());
  }
  return napi_ok;
}

napi_status napi_get_version(napi_env env, uint32_t* result) {
  if (!env || !result) {
    return napi_invalid_arg;
  }
  *result = NAPI_VERSION;
  return napi_ok;
}

// --- Functions ---

napi_status napi_create_function(napi_env env, const char* utf8name, size_t length, napi_callback cb, void* data, napi_value* result) {
  if (!env || !cb || !result) return napi_invalid_arg;
  v8::Local<v8::Context> context = env->GetContext();

  auto cbd = std::make_unique<CallbackData>(CallbackData{cb, data, env});
  v8::Local<v8::External> ext = v8::External::New(
      env->isolate, cbd.get(), v8::kExternalPointerTypeTagDefault);
  env->callbacks.push_back(std::move(cbd));

  v8::Local<v8::Function> fn;
  if (!v8::Function::New(context, CallbackDispatcher, ext).ToLocal(&fn)) return napi_generic_failure;
  
  if (utf8name) {
    const size_t name_length =
        length == NAPI_AUTO_LENGTH ? strlen(utf8name) : length;
    if (name_length > static_cast<size_t>(std::numeric_limits<int>::max())) {
      return napi_invalid_arg;
    }
    v8::Local<v8::String> name_str;
    if (!v8::String::NewFromUtf8(env->isolate, utf8name,
                                 v8::NewStringType::kNormal,
                                 static_cast<int>(name_length))
             .ToLocal(&name_str)) {
      return napi_generic_failure;
    }
    fn->SetName(name_str);
  }
  *result = env->CreateValue(fn);
  return napi_ok;
}

napi_status napi_call_function(napi_env env, napi_value recv, napi_value func, size_t argc, const napi_value* argv, napi_value* result) {
  if (!env || !func || (argc > 0 && !argv)) return napi_invalid_arg;
  if (argc > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return napi_invalid_arg;
  }
  if (!func->Get()->IsFunction()) {
    return napi_function_expected;
  }
  v8::Local<v8::Context> context = env->GetContext();

  v8::Local<v8::Function> fn = func->Get().As<v8::Function>();
  v8::Local<v8::Value> local_recv;
  if (recv) {
    local_recv = recv->Get();
  } else {
    local_recv = v8::Undefined(env->isolate);
  }

  std::vector<v8::Local<v8::Value>> v8_argv(argc);
  for (size_t i = 0; i < argc; ++i) {
    v8_argv[i] = argv[i]->Get();
  }

  v8::Local<v8::Value> ret;
  v8::TryCatch try_catch(env->isolate);
  if (!fn->Call(context, local_recv, static_cast<int>(argc), v8_argv.data()).ToLocal(&ret)) {
    if (try_catch.HasCaught()) {
      env->last_exception.Reset(env->isolate, try_catch.Exception());
    }
    return napi_pending_exception;
  }

  if (result) {
    *result = env->CreateValue(ret);
  }
  return napi_ok;
}

napi_status napi_strict_equals(napi_env env,
                               napi_value lhs,
                               napi_value rhs,
                               bool* result) {
  if (!env || !lhs || !rhs || !result) {
    return napi_invalid_arg;
  }
  *result = lhs->Get()->StrictEquals(rhs->Get());
  return napi_ok;
}

napi_status napi_instanceof(napi_env env,
                            napi_value object,
                            napi_value constructor,
                            bool* result) {
  if (!env || !object || !constructor || !result) {
    return napi_invalid_arg;
  }
  if (!object->Get()->IsObject()) {
    return napi_object_expected;
  }
  if (!constructor->Get()->IsFunction()) {
    return napi_function_expected;
  }
  *result =
      object->Get()
          .As<v8::Object>()
          ->InstanceOf(env->GetContext(), constructor->Get().As<v8::Function>())
          .FromMaybe(false);
  return napi_ok;
}

napi_status napi_get_cb_info(napi_env env, napi_callback_info cbinfo, size_t* argc, napi_value* argv, napi_value* this_arg, void** data) {
  if (!env || !cbinfo) return napi_invalid_arg;
  const auto& v8_info = cbinfo->v8_info;

  if (data) *data = cbinfo->data;
  if (this_arg) *this_arg = env->CreateValue(v8_info.This());

  if (argc && argv) {
    size_t count = *argc;
    size_t actual_count = static_cast<size_t>(v8_info.Length());
    size_t copy_count = actual_count < count ? actual_count : count;
    for (size_t i = 0; i < copy_count; ++i) {
      argv[i] = env->CreateValue(v8_info[static_cast<int>(i)]);
    }
    *argc = actual_count;
  } else if (argc) {
    *argc = static_cast<size_t>(v8_info.Length());
  }
  return napi_ok;
}

napi_status napi_get_new_target(napi_env env, napi_callback_info cbinfo, napi_value* result) {
  if (!env || !cbinfo || !result) return napi_invalid_arg;
  const auto& v8_info = cbinfo->v8_info;
  if (!v8_info.IsConstructCall()) {
    *result = nullptr;
    return napi_ok;
  }
  *result = env->CreateValue(v8_info.NewTarget());
  return napi_ok;
}

napi_status napi_new_instance(napi_env env, napi_value constructor, size_t argc, const napi_value* argv, napi_value* result) {
  if (!env || !constructor || (argc > 0 && !argv) || !result) return napi_invalid_arg;
  if (argc > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return napi_invalid_arg;
  }
  if (!constructor->Get()->IsFunction()) {
    return napi_function_expected;
  }
  v8::Local<v8::Context> context = env->GetContext();
  v8::Local<v8::Function> fn = constructor->Get().As<v8::Function>();

  std::vector<v8::Local<v8::Value>> v8_argv(argc);
  for (size_t i = 0; i < argc; ++i) {
    v8_argv[i] = argv[i]->Get();
  }

  v8::Local<v8::Object> inst;
  if (!fn->NewInstance(context, static_cast<int>(argc), v8_argv.data()).ToLocal(&inst)) return napi_generic_failure;
  *result = env->CreateValue(inst);
  return napi_ok;
}

// --- References ---

napi_status napi_create_reference(napi_env env, napi_value value, uint32_t initial_refcount, napi_ref* result) {
  if (!env || !value || !result) return napi_invalid_arg;
  auto ref = std::make_unique<napi_ref__>(env->isolate);
  ref->global_value.Reset(env->isolate, value->Get());
  ref->ref_count = initial_refcount;
  if (initial_refcount == 0) {
    ref->SetWeak();
  }
  *result = ref.release();
  return napi_ok;
}

napi_status napi_delete_reference(napi_env env, napi_ref ref) {
  if (!env || !ref) return napi_invalid_arg;
  ref->global_value.Reset();
  delete ref;
  return napi_ok;
}

napi_status napi_reference_ref(napi_env env, napi_ref ref, uint32_t* result) {
  if (!env || !ref) return napi_invalid_arg;
  if (!ref->global_value.IsEmpty()) {
    if (ref->ref_count == 0) {
      ref->ClearWeak();
    }
    ++ref->ref_count;
  }
  if (result) *result = ref->ref_count;
  return napi_ok;
}

napi_status napi_reference_unref(napi_env env, napi_ref ref, uint32_t* result) {
  if (!env || !ref) return napi_invalid_arg;
  if (ref->ref_count > 0) {
    --ref->ref_count;
    if (ref->ref_count == 0) {
      ref->SetWeak();
    }
  }
  if (result) *result = ref->ref_count;
  return napi_ok;
}

napi_status napi_get_reference_value(napi_env env, napi_ref ref, napi_value* result) {
  if (!env || !ref || !result) return napi_invalid_arg;
  if (ref->global_value.IsEmpty()) {
    *result = nullptr;
  } else {
    *result = env->CreateValue(ref->global_value.Get(env->isolate));
  }
  return napi_ok;
}

// --- Error handling ---

namespace {

enum class ErrorType {
  kError,
  kTypeError,
  kRangeError,
};

napi_status CreateErrorValue(napi_env env,
                             napi_value code,
                             napi_value message,
                             napi_value* result,
                             ErrorType type) {
  if (!env || !message || !result) {
    return napi_invalid_arg;
  }
  if (!message->Get()->IsString()) {
    return napi_string_expected;
  }
  if (code && !code->Get()->IsString()) {
    return napi_string_expected;
  }
  v8::Local<v8::Value> error;
  switch (type) {
    case ErrorType::kError:
      error = v8::Exception::Error(message->Get().As<v8::String>());
      break;
    case ErrorType::kTypeError:
      error = v8::Exception::TypeError(message->Get().As<v8::String>());
      break;
    case ErrorType::kRangeError:
      error = v8::Exception::RangeError(message->Get().As<v8::String>());
      break;
  }
  if (code && !error.As<v8::Object>()
                   ->Set(env->GetContext(),
                         v8::String::NewFromUtf8Literal(env->isolate, "code"),
                         code->Get())
                   .FromMaybe(false)) {
    return napi_generic_failure;
  }
  *result = env->CreateValue(error);
  return napi_ok;
}

}  // namespace

napi_status napi_create_error(napi_env env,
                              napi_value code,
                              napi_value msg,
                              napi_value* result) {
  return CreateErrorValue(env, code, msg, result, ErrorType::kError);
}

napi_status napi_create_type_error(napi_env env,
                                   napi_value code,
                                   napi_value msg,
                                   napi_value* result) {
  return CreateErrorValue(env, code, msg, result, ErrorType::kTypeError);
}

napi_status napi_create_range_error(napi_env env,
                                    napi_value code,
                                    napi_value msg,
                                    napi_value* result) {
  return CreateErrorValue(env, code, msg, result, ErrorType::kRangeError);
}

napi_status napi_throw(napi_env env, napi_value error) {
  if (!env || !error) return napi_invalid_arg;
  env->last_exception.Reset(env->isolate, error->Get());
  env->isolate->ThrowException(error->Get());
  return napi_ok;
}

napi_status napi_throw_error(napi_env env, const char* code, const char* msg) {
  if (!env || !msg) return napi_invalid_arg;
  v8::Local<v8::String> msg_str = v8::String::NewFromUtf8(env->isolate, msg, v8::NewStringType::kNormal).ToLocalChecked();
  v8::Local<v8::Value> err = v8::Exception::Error(msg_str);
  env->last_exception.Reset(env->isolate, err);
  env->isolate->ThrowException(err);
  return napi_ok;
}

napi_status napi_throw_type_error(napi_env env, const char* code, const char* msg) {
  if (!env || !msg) return napi_invalid_arg;
  v8::Local<v8::String> msg_str = v8::String::NewFromUtf8(env->isolate, msg, v8::NewStringType::kNormal).ToLocalChecked();
  v8::Local<v8::Value> err = v8::Exception::TypeError(msg_str);
  env->last_exception.Reset(env->isolate, err);
  env->isolate->ThrowException(err);
  return napi_ok;
}

napi_status napi_throw_range_error(napi_env env, const char* code, const char* msg) {
  if (!env || !msg) return napi_invalid_arg;
  v8::Local<v8::String> msg_str = v8::String::NewFromUtf8(env->isolate, msg, v8::NewStringType::kNormal).ToLocalChecked();
  v8::Local<v8::Value> err = v8::Exception::RangeError(msg_str);
  env->last_exception.Reset(env->isolate, err);
  env->isolate->ThrowException(err);
  return napi_ok;
}

napi_status napi_get_and_clear_last_exception(napi_env env, napi_value* result) {
  if (!env || !result) return napi_invalid_arg;
  if (env->last_exception.IsEmpty()) {
    *result = env->CreateValue(v8::Undefined(env->isolate));
  } else {
    *result = env->CreateValue(env->last_exception.Get(env->isolate));
    env->last_exception.Reset();
  }
  return napi_ok;
}

napi_status napi_is_exception_pending(napi_env env, bool* result) {
  if (!env || !result) return napi_invalid_arg;
  *result = !env->last_exception.IsEmpty();
  return napi_ok;
}

napi_status napi_fatal_error(const char* location, size_t location_len, const char* message, size_t message_len) {
  LOG(FATAL) << "Fatal error in Node addon: " << std::string(message, message_len) << " at " << std::string(location, location_len);
}

// --- Class Creation ---

napi_status napi_define_class(napi_env env,
                              const char* utf8name,
                              size_t length,
                              napi_callback constructor,
                              void* data,
                              size_t property_count,
                              const napi_property_descriptor* properties,
                              napi_value* result) {
  if (!env || !constructor || !result) {
    return napi_invalid_arg;
  }
  if (property_count > 0 && !properties) {
    return napi_invalid_arg;
  }
  if (utf8name && length != NAPI_AUTO_LENGTH &&
      length > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return napi_invalid_arg;
  }
  *result = nullptr;

  v8::Isolate* isolate = env->isolate;
  v8::EscapableHandleScope scope(isolate);
  v8::Local<v8::Context> context = env->GetContext();

  auto cbd =
      std::make_unique<CallbackData>(CallbackData{constructor, data, env});
  v8::Local<v8::External> ext = v8::External::New(
      isolate, cbd.get(), v8::kExternalPointerTypeTagDefault);
  env->callbacks.push_back(std::move(cbd));

  // Only install the constructor on the template. Instance methods are attached
  // afterwards via napi_define_properties on Constructor.prototype; this
  // matches Node's reliability profile better than stuffing dozens of
  // FunctionTemplates onto PrototypeTemplate (which can fail GetFunction).
  v8::Local<v8::FunctionTemplate> tpl =
      v8::FunctionTemplate::New(isolate, CallbackDispatcher, ext);
  tpl->InstanceTemplate()->SetInternalFieldCount(1);

  if (utf8name) {
    // Addons often pass sizeof(literal) which includes the trailing '\0'.
    size_t name_len = length;
    if (length == NAPI_AUTO_LENGTH) {
      name_len = strlen(utf8name);
    } else if (length > 0 && utf8name[length - 1] == '\0') {
      name_len = length - 1;
    }
    v8::Local<v8::String> name_str =
        v8::String::NewFromUtf8(isolate, utf8name, v8::NewStringType::kNormal,
                                static_cast<int>(name_len))
            .ToLocalChecked();
    tpl->SetClassName(name_str);
  }

  v8::Local<v8::Function> fn;
  if (!tpl->GetFunction(context).ToLocal(&fn)) {
    LOG(ERROR) << "[napi_define_class] GetFunction failed for "
               << (utf8name ? utf8name : "<anonymous>");
    return napi_generic_failure;
  }

  // Partition instance vs static descriptors (static go on the constructor).
  std::vector<napi_property_descriptor> instance_props;
  std::vector<napi_property_descriptor> static_props;
  instance_props.reserve(property_count);
  static_props.reserve(property_count);
  // FunctionTemplate::Set lets a later duplicate descriptor replace the
  // earlier one. Preserve that behavior when defining properties afterwards.
  auto append_property =
      [](std::vector<napi_property_descriptor>* descriptors,
         const napi_property_descriptor& descriptor) {
        for (auto& existing : *descriptors) {
          const bool same_utf8_name =
              existing.utf8name && descriptor.utf8name &&
              strcmp(existing.utf8name, descriptor.utf8name) == 0;
          const bool same_value_name =
              !existing.utf8name && !descriptor.utf8name &&
              existing.name && descriptor.name &&
              existing.name->Get()->SameValue(descriptor.name->Get());
          if (same_utf8_name || same_value_name) {
            existing = descriptor;
            return;
          }
        }
        descriptors->push_back(descriptor);
      };
  for (size_t i = 0; i < property_count; ++i) {
    napi_property_descriptor desc = properties[i];
    if (desc.attributes & napi_static) {
      append_property(&static_props, desc);
    } else {
      append_property(&instance_props, desc);
    }
  }

  v8::Local<v8::String> proto_key =
      v8::String::NewFromUtf8Literal(isolate, "prototype");
  v8::Local<v8::Value> prototype_value;
  if (!fn->Get(context, proto_key).ToLocal(&prototype_value) ||
      !prototype_value->IsObject()) {
    LOG(ERROR) << "[napi_define_class] constructor.prototype missing for "
               << (utf8name ? utf8name : "<anonymous>");
    return napi_generic_failure;
  }
  napi_value prototype = env->CreateValue(prototype_value);
  napi_value ctor = env->CreateValue(fn);

  if (!instance_props.empty()) {
    napi_status status = napi_define_properties(
        env, prototype, instance_props.size(), instance_props.data());
    if (status != napi_ok) {
      LOG(ERROR) << "[napi_define_class] define instance props failed: "
                 << status;
      return status;
    }
  }
  if (!static_props.empty()) {
    napi_status status = napi_define_properties(
        env, ctor, static_props.size(), static_props.data());
    if (status != napi_ok) {
      LOG(ERROR) << "[napi_define_class] define static props failed: "
                 << status;
      return status;
    }
  }

  // Escape into the caller's HandleScope before this scope closes.
  *result = env->CreateValue(scope.Escape(fn));
  VLOG(1) << "[napi_define_class] defined "
            << (utf8name ? utf8name : "<anonymous>")
            << " with " << instance_props.size() << " instance and "
            << static_props.size() << " static properties";
  return napi_ok;
}

// --- Wrap / Unwrap ---

napi_status napi_wrap(napi_env env, napi_value js_object, void* native_object, napi_finalize finalize_cb, void* finalize_hint, napi_ref* result) {
  if (!env || !js_object) return napi_invalid_arg;
  v8::Local<v8::Context> context = env->GetContext();
  v8::Local<v8::Object> obj;
  if (!js_object->Get()->ToObject(context).ToLocal(&obj)) {
    return napi_object_expected;
  }

  if (GetWrapData(env, obj)) {
    return napi_invalid_arg;
  }

  auto data = std::make_unique<NapiFinalizerData>(
      NapiFinalizerData{env, native_object, finalize_cb, finalize_hint, {}});
  NapiFinalizerData* data_ptr = data.get();
  v8::Local<v8::External> external = v8::External::New(
      env->isolate, data_ptr, v8::kExternalPointerTypeTagDefault);
  if (!obj->SetPrivate(context, GetWrapPrivateKey(env), external)
           .FromMaybe(false)) {
    return napi_generic_failure;
  }

  data_ptr->persistent.Reset(env->isolate, obj);
  data_ptr->persistent.SetWeak(data_ptr, OnNapiFinalizerFirstPass,
                               v8::WeakCallbackType::kParameter);
  env->finalizers.push_back(std::move(data));

  if (result) {
    napi_create_reference(env, js_object, 0, result);
  }
  return napi_ok;
}

napi_status napi_unwrap(napi_env env, napi_value js_object, void** result) {
  if (!env || !js_object || !result) return napi_invalid_arg;
  v8::Local<v8::Context> context = env->GetContext();
  v8::Local<v8::Object> obj;
  if (!js_object->Get()->ToObject(context).ToLocal(&obj)) {
    return napi_object_expected;
  }

  NapiFinalizerData* data = GetWrapData(env, obj);
  if (!data) {
    *result = nullptr;
    return napi_invalid_arg;
  }

  *result = data->data;
  return napi_ok;
}

napi_status napi_remove_wrap(napi_env env, napi_value js_object, void** result) {
  if (!env || !js_object) return napi_invalid_arg;
  v8::Local<v8::Context> context = env->GetContext();
  v8::Local<v8::Object> obj;
  if (!js_object->Get()->ToObject(context).ToLocal(&obj)) {
    return napi_object_expected;
  }

  NapiFinalizerData* data = GetWrapData(env, obj);
  if (!data) {
    if (result) *result = nullptr;
    return napi_invalid_arg;
  }

  if (result) {
    *result = data->data;
  }
  if (!obj->DeletePrivate(context, GetWrapPrivateKey(env)).FromMaybe(false)) {
    return napi_generic_failure;
  }
  std::unique_ptr<NapiFinalizerData> removed = TakeFinalizer(data);
  if (removed) {
    removed->callback = nullptr;
    removed->persistent.Reset();
  }
  return napi_ok;
}

// --- External values ---

napi_status napi_create_external(napi_env env, void* data, napi_finalize finalize_cb, void* finalize_hint, napi_value* result) {
  if (!env || !result) return napi_invalid_arg;
  v8::Local<v8::External> ext = v8::External::New(
      env->isolate, data, v8::kExternalPointerTypeTagDefault);
  
  if (finalize_cb) {
    auto finalizer = std::make_unique<NapiFinalizerData>(
        NapiFinalizerData{env, data, finalize_cb, finalize_hint, {}});
    NapiFinalizerData* finalizer_ptr = finalizer.get();
    finalizer_ptr->persistent.Reset(env->isolate, ext);
    finalizer_ptr->persistent.SetWeak(finalizer_ptr, OnNapiFinalizerFirstPass,
                                      v8::WeakCallbackType::kParameter);
    env->finalizers.push_back(std::move(finalizer));
  }

  *result = env->CreateValue(ext);
  return napi_ok;
}

napi_status napi_get_value_external(napi_env env, napi_value value, void** result) {
  if (!env || !value || !result) return napi_invalid_arg;
  if (!value->Get()->IsExternal()) return napi_invalid_arg;
  *result = v8::Local<v8::External>::Cast(value->Get())
                ->Value(v8::kExternalPointerTypeTagDefault);
  return napi_ok;
}

// --- Execution ---

napi_status napi_run_script(napi_env env, napi_value script, napi_value* result) {
  if (!env || !script || !result) return napi_invalid_arg;
  v8::Local<v8::Context> context = env->GetContext();
  v8::Local<v8::String> src;
  if (!script->Get()->ToString(context).ToLocal(&src)) return napi_string_expected;

  v8::Local<v8::Script> compiled_script;
  if (!v8::Script::Compile(context, src).ToLocal(&compiled_script)) return napi_generic_failure;

  v8::Local<v8::Value> val;
  if (!compiled_script->Run(context).ToLocal(&val)) return napi_generic_failure;

  *result = env->CreateValue(val);
  return napi_ok;
}

napi_status napi_adjust_external_memory(napi_env env,
                                        int64_t change_in_bytes,
                                        int64_t* adjusted_value) {
  if (!env || !adjusted_value) {
    return napi_invalid_arg;
  }
  *adjusted_value =
      env->isolate->AdjustAmountOfExternalAllocatedMemory(change_in_bytes);
  return napi_ok;
}

// --- Node-specific API Stub implementations ---

#if !BUILDFLAG(ENABLE_XENON_NODE_UV_COMPAT)

struct uv_async_s;
struct uv_timer_s;
struct uv_handle_s;
struct uv_stream_s;
struct uv_write_s;
struct uv_connect_s;
struct uv_pipe_s;

using uv_async_t = uv_async_s;
using uv_timer_t = uv_timer_s;
using uv_handle_t = uv_handle_s;
using uv_stream_t = uv_stream_s;
using uv_write_t = uv_write_s;
using uv_connect_t = uv_connect_s;
using uv_pipe_t = uv_pipe_s;
using uv_async_cb = void (*)(uv_async_t* handle);
using uv_timer_cb = void (*)(uv_timer_t* handle);
using uv_close_cb = void (*)(uv_handle_t* handle);
using uv_connection_cb = void (*)(uv_stream_t* server, int status);
using uv_read_cb = void (*)(uv_stream_t* stream, intptr_t nread, const struct uv_buf_t* buf);
using uv_alloc_cb = void (*)(uv_handle_t* handle, size_t suggested_size, struct uv_buf_t* buf);
using uv_write_cb = void (*)(uv_write_t* req, int status);
using uv_connect_cb = void (*)(uv_connect_t* req, int status);

struct uv_buf_t {
  unsigned long len;
  NAPI_RAW_PTR_EXCLUSION char* base;
};

namespace {

constexpr int kUvRunDefault = 0;
constexpr int kUvRunNowait = 2;

struct FakeUvLoopState {
  FakeUvLoopState(scoped_refptr<base::SingleThreadTaskRunner> task_runner,
                  bool dispatch_on_run_thread)
      : task_runner(std::move(task_runner)),
        dispatch_on_run_thread(dispatch_on_run_thread) {}

  scoped_refptr<base::SingleThreadTaskRunner> task_runner;
  const bool dispatch_on_run_thread;
  std::mutex mutex;
  std::condition_variable wake_condition;
  std::deque<base::OnceClosure> pending_tasks;
  bool stop_requested = false;
  bool closed = false;
};

struct FakeUvLoop {
  FakeUvLoop(scoped_refptr<base::SingleThreadTaskRunner> task_runner,
             bool dispatch_on_run_thread)
      : state(std::make_shared<FakeUvLoopState>(
            std::move(task_runner),
            dispatch_on_run_thread)) {}

  std::shared_ptr<FakeUvLoopState> state;
};

struct FakeAsyncState {
  std::shared_ptr<FakeUvLoopState> loop;
  uv_async_cb callback = nullptr;
};

struct FakeTimerState {
  std::shared_ptr<FakeUvLoopState> loop;
  uv_timer_cb callback = nullptr;
  uint64_t repeat_ms = 0;
  uint64_t generation = 0;
  bool stopped = true;
};

std::mutex& FakeUvMutex() {
  static std::mutex* mutex = new std::mutex();
  return *mutex;
}

std::unordered_map<void*, std::shared_ptr<FakeAsyncState>>& FakeAsyncMap() {
  static auto* map =
      new std::unordered_map<void*, std::shared_ptr<FakeAsyncState>>();
  return *map;
}

std::unordered_map<void*, std::shared_ptr<FakeTimerState>>& FakeTimerMap() {
  static auto* map =
      new std::unordered_map<void*, std::shared_ptr<FakeTimerState>>();
  return *map;
}

scoped_refptr<base::SingleThreadTaskRunner> CreateFakeUvTaskRunner() {
  if (base::SingleThreadTaskRunner::HasCurrentDefault()) {
    return base::SingleThreadTaskRunner::GetCurrentDefault();
  }
  return base::ThreadPool::CreateSingleThreadTaskRunner({});
}

FakeUvLoop* GetDefaultFakeUvLoop() {
  static FakeUvLoop* loop =
      new FakeUvLoop(CreateFakeUvTaskRunner(), false);
  return loop;
}

void EnqueueFakeUvTask(std::shared_ptr<FakeUvLoopState> loop,
                       base::OnceClosure task) {
  {
    std::lock_guard<std::mutex> lock(loop->mutex);
    if (loop->closed) {
      return;
    }
    loop->pending_tasks.push_back(std::move(task));
  }
  loop->wake_condition.notify_one();
}

void PostFakeUvTask(std::shared_ptr<FakeUvLoopState> loop,
                    base::OnceClosure task,
                    base::TimeDelta delay = base::TimeDelta()) {
  if (!loop) {
    return;
  }

  if (!loop->dispatch_on_run_thread) {
    loop->task_runner->PostDelayedTask(FROM_HERE, std::move(task), delay);
    return;
  }

  base::OnceClosure enqueue =
      base::BindOnce(&EnqueueFakeUvTask, loop, std::move(task));
  if (delay.is_zero()) {
    std::move(enqueue).Run();
    return;
  }
  loop->task_runner->PostDelayedTask(FROM_HERE, std::move(enqueue), delay);
}

void ScheduleFakeTimer(void* handle,
                       std::shared_ptr<FakeTimerState> state,
                       uint64_t delay_ms,
                       uint64_t generation);

void RunFakeTimer(void* handle,
                  std::shared_ptr<FakeTimerState> state,
                  uint64_t generation) {
  {
    std::lock_guard<std::mutex> lock(FakeUvMutex());
    if (state->stopped || state->generation != generation) {
      return;
    }
  }

  if (state->callback) {
    state->callback(reinterpret_cast<uv_timer_t*>(handle));
  }

  uint64_t repeat_ms = 0;
  uint64_t next_generation = 0;
  {
    std::lock_guard<std::mutex> lock(FakeUvMutex());
    if (state->stopped || state->generation != generation ||
        state->repeat_ms == 0) {
      return;
    }
    repeat_ms = state->repeat_ms;
    ++state->generation;
    next_generation = state->generation;
  }
  ScheduleFakeTimer(handle, std::move(state), repeat_ms, next_generation);
}

void ScheduleFakeTimer(void* handle,
                       std::shared_ptr<FakeTimerState> state,
                       uint64_t delay_ms,
                       uint64_t generation) {
  std::shared_ptr<FakeUvLoopState> loop = state->loop;
  if (!loop || !loop->task_runner) {
    return;
  }
  PostFakeUvTask(
      std::move(loop),
      base::BindOnce(&RunFakeTimer, handle, std::move(state), generation),
      base::Milliseconds(delay_ms));
}

}  // namespace

NAPI_EXTERN size_t uv_loop_size(void) {
  return sizeof(FakeUvLoop);
}

NAPI_EXTERN int uv_loop_init(uv_loop_t* loop) {
  if (!loop) return -1;
  new (reinterpret_cast<FakeUvLoop*>(loop))
      FakeUvLoop(CreateFakeUvTaskRunner(), true);
  return 0;
}

NAPI_EXTERN int uv_loop_close(uv_loop_t* loop) {
  if (!loop) return -1;
  FakeUvLoop* fake_loop = reinterpret_cast<FakeUvLoop*>(loop);
  if (fake_loop == GetDefaultFakeUvLoop()) {
    return 0;
  }
  {
    std::lock_guard<std::mutex> lock(fake_loop->state->mutex);
    fake_loop->state->closed = true;
    fake_loop->state->stop_requested = true;
    fake_loop->state->pending_tasks.clear();
  }
  fake_loop->state->wake_condition.notify_all();
  fake_loop->~FakeUvLoop();
  return 0;
}

NAPI_EXTERN uv_loop_t* uv_default_loop(void) {
  return reinterpret_cast<uv_loop_t*>(GetDefaultFakeUvLoop());
}

NAPI_EXTERN int uv_run(uv_loop_t* loop, int mode) {
  if (!loop) return -1;
  std::shared_ptr<FakeUvLoopState> state =
      reinterpret_cast<FakeUvLoop*>(loop)->state;
  if (!state->dispatch_on_run_thread) {
    return 0;
  }

  std::unique_lock<std::mutex> lock(state->mutex);
  while (!state->stop_requested && !state->closed) {
    if (state->pending_tasks.empty()) {
      if (mode == kUvRunNowait) {
        break;
      }
      state->wake_condition.wait(lock);
      continue;
    }

    base::OnceClosure task = std::move(state->pending_tasks.front());
    state->pending_tasks.pop_front();
    lock.unlock();
    std::move(task).Run();
    lock.lock();
    if (mode != kUvRunDefault) {
      break;
    }
  }
  state->stop_requested = false;
  return state->pending_tasks.empty() ? 0 : 1;
}

NAPI_EXTERN void uv_stop(uv_loop_t* loop) {
  if (!loop) return;
  std::shared_ptr<FakeUvLoopState> state =
      reinterpret_cast<FakeUvLoop*>(loop)->state;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->stop_requested = true;
  }
  state->wake_condition.notify_all();
}

NAPI_EXTERN int uv_async_init(uv_loop_t* loop,
                              uv_async_t* handle,
                              uv_async_cb async_cb) {
  if (!loop || !handle || !async_cb) return -1;
  auto state = std::make_shared<FakeAsyncState>();
  state->loop = reinterpret_cast<FakeUvLoop*>(loop)->state;
  state->callback = async_cb;
  std::lock_guard<std::mutex> lock(FakeUvMutex());
  FakeAsyncMap()[handle] = std::move(state);
  return 0;
}

NAPI_EXTERN int uv_async_send(uv_async_t* handle) {
  std::shared_ptr<FakeAsyncState> state;
  {
    std::lock_guard<std::mutex> lock(FakeUvMutex());
    auto it = FakeAsyncMap().find(handle);
    if (it == FakeAsyncMap().end()) return -1;
    state = it->second;
  }
  if (!state || !state->loop || !state->callback) {
    return -1;
  }
  void* handle_key = handle;
  std::shared_ptr<FakeUvLoopState> loop = state->loop;
  PostFakeUvTask(
      std::move(loop),
      base::BindOnce(
          [](void* handle, std::shared_ptr<FakeAsyncState> state) {
            uv_async_cb callback = nullptr;
            {
              std::lock_guard<std::mutex> lock(FakeUvMutex());
              auto it = FakeAsyncMap().find(handle);
              if (it == FakeAsyncMap().end() || it->second != state) {
                return;
              }
              callback = state->callback;
            }
            if (callback) {
              callback(reinterpret_cast<uv_async_t*>(handle));
            }
          },
          handle_key, std::move(state)));
  return 0;
}

NAPI_EXTERN int uv_timer_init(uv_loop_t* loop, uv_timer_t* handle) {
  if (!loop || !handle) return -1;
  auto state = std::make_shared<FakeTimerState>();
  state->loop = reinterpret_cast<FakeUvLoop*>(loop)->state;
  std::lock_guard<std::mutex> lock(FakeUvMutex());
  FakeTimerMap()[handle] = std::move(state);
  return 0;
}

NAPI_EXTERN int uv_timer_start(uv_timer_t* handle,
                               uv_timer_cb timer_cb,
                               uint64_t timeout,
                               uint64_t repeat) {
  if (!handle || !timer_cb) return -1;
  std::shared_ptr<FakeTimerState> state;
  uint64_t generation = 0;
  {
    std::lock_guard<std::mutex> lock(FakeUvMutex());
    auto it = FakeTimerMap().find(handle);
    if (it == FakeTimerMap().end()) return -1;
    state = it->second;
    state->callback = timer_cb;
    state->repeat_ms = repeat;
    state->stopped = false;
    ++state->generation;
    generation = state->generation;
  }
  ScheduleFakeTimer(handle, state, timeout, generation);
  return 0;
}

NAPI_EXTERN int uv_timer_stop(uv_timer_t* handle) {
  if (!handle) return -1;
  std::lock_guard<std::mutex> lock(FakeUvMutex());
  auto it = FakeTimerMap().find(handle);
  if (it == FakeTimerMap().end()) return -1;
  it->second->stopped = true;
  ++it->second->generation;
  return 0;
}

NAPI_EXTERN void uv_close(uv_handle_t* handle, uv_close_cb close_cb) {
  if (!handle) return;
  {
    std::lock_guard<std::mutex> lock(FakeUvMutex());
    FakeAsyncMap().erase(handle);
    auto timer_it = FakeTimerMap().find(handle);
    if (timer_it != FakeTimerMap().end()) {
      timer_it->second->stopped = true;
      ++timer_it->second->generation;
      FakeTimerMap().erase(timer_it);
    }
  }
  if (close_cb) {
    close_cb(handle);
  }
}

NAPI_EXTERN int uv_accept(uv_stream_t* server, uv_stream_t* client) {
  return -1;
}

NAPI_EXTERN uv_buf_t uv_buf_init(char* base, unsigned int len) {
  return uv_buf_t{len, base};
}

NAPI_EXTERN int uv_read_start(uv_stream_t* stream,
                              uv_alloc_cb alloc_cb,
                              uv_read_cb read_cb) {
  return -1;
}

NAPI_EXTERN int uv_write(uv_write_t* req,
                         uv_stream_t* handle,
                         const uv_buf_t bufs[],
                         unsigned int nbufs,
                         uv_write_cb cb) {
  if (cb) {
    cb(req, -1);
  }
  return -1;
}

NAPI_EXTERN int uv_pipe_init(uv_loop_t* loop, uv_pipe_t* handle, int ipc) {
  return loop && handle ? 0 : -1;
}

NAPI_EXTERN void uv_pipe_connect(uv_connect_t* req,
                                 uv_pipe_t* handle,
                                 const char* name,
                                 uv_connect_cb cb) {
  if (cb) {
    cb(req, -1);
  }
}

NAPI_EXTERN int uv_listen(uv_stream_t* stream,
                          int backlog,
                          uv_connection_cb cb) {
  return -1;
}

NAPI_EXTERN int uv_pipe_bind(uv_pipe_t* handle, const char* name) {
  return -1;
}

NAPI_EXTERN void uv_sleep(unsigned int msec) {
  std::this_thread::sleep_for(std::chrono::milliseconds(msec));
}

#endif  // !BUILDFLAG(ENABLE_XENON_NODE_UV_COMPAT)

#if defined(_WIN32)
namespace node {
class __declspec(dllexport) CallbackScope {
 public:
  ~CallbackScope();
};

CallbackScope::~CallbackScope() = default;
}  // namespace node

extern "C" void* XenonV8ArrayBufferAllocatorReallocate(
    void* self,
    void* data,
    size_t old_length,
    size_t new_length) {
  if (new_length == 0) {
    if (data) {
      if (self) {
        static_cast<v8::ArrayBuffer::Allocator*>(self)->Free(data, old_length);
      } else {
        std::free(data);
      }
    }
    return nullptr;
  }

  void* new_data = nullptr;
  if (self) {
    auto* allocator = static_cast<v8::ArrayBuffer::Allocator*>(self);
    new_data = allocator->AllocateUninitialized(new_length);
    if (new_data && data) {
      const size_t copy_len = std::min(old_length, new_length);
      // SAFETY: |data| is |old_length| bytes from the allocator; |new_data| is
      // |new_length| bytes of uninitialized storage.
      UNSAFE_BUFFERS(base::span(static_cast<uint8_t*>(new_data), copy_len))
          .copy_from(UNSAFE_BUFFERS(
              base::span(static_cast<const uint8_t*>(data), copy_len)));
      allocator->Free(data, old_length);
    }
  } else {
    new_data = std::realloc(data, new_length);
  }
  return new_data;
}

#pragma comment(linker, "/export:?Reallocate@Allocator@ArrayBuffer@v8@@UEAAPEAXPEAX_K1@Z=XenonV8ArrayBufferAllocatorReallocate")
#endif

static std::vector<napi_module*>& GetRegisteredModules() {
  static base::NoDestructor<std::vector<napi_module*>> modules;
  return *modules;
}

static std::mutex& GetRegisteredModulesLock() {
  static std::mutex lock;
  return lock;
}

void napi_module_register(napi_module* mod) {
  if (!mod) {
    return;
  }
  std::lock_guard<std::mutex> guard(GetRegisteredModulesLock());
  GetRegisteredModules().push_back(mod);
}

NAPI_EXTERN void xenon_napi_clear_registered_modules(void) {
  std::lock_guard<std::mutex> guard(GetRegisteredModulesLock());
  GetRegisteredModules().clear();
}

NAPI_EXTERN size_t xenon_napi_get_registered_module_count(void) {
  std::lock_guard<std::mutex> guard(GetRegisteredModulesLock());
  return GetRegisteredModules().size();
}

NAPI_EXTERN napi_module* xenon_napi_get_registered_module(size_t index) {
  std::lock_guard<std::mutex> guard(GetRegisteredModulesLock());
  auto& modules = GetRegisteredModules();
  if (index >= modules.size()) {
    return nullptr;
  }
  return modules[index];
}

NAPI_EXTERN bool xenon_napi_remove_registered_module(napi_module* mod) {
  std::lock_guard<std::mutex> guard(GetRegisteredModulesLock());
  auto& modules = GetRegisteredModules();
  auto module = std::find(modules.begin(), modules.end(), mod);
  if (module == modules.end()) {
    return false;
  }
  modules.erase(module);
  return true;
}

NAPI_EXTERN napi_module* xenon_napi_get_last_registered_module(void) {
  std::lock_guard<std::mutex> guard(GetRegisteredModulesLock());
  auto& modules = GetRegisteredModules();
  if (modules.empty()) {
    return nullptr;
  }
  return modules.back();
}

struct napi_async_work__ {
  napi_async_work__(napi_env env,
                    napi_async_execute_callback execute,
                    napi_async_complete_callback complete,
                    void* data);
  ~napi_async_work__();

  napi_env env;
  napi_async_execute_callback execute;
  napi_async_complete_callback complete;
  NAPI_RAW_PTR_EXCLUSION void* data;
  scoped_refptr<base::SingleThreadTaskRunner> main_task_runner;
};

napi_async_work__::napi_async_work__(napi_env env,
                                     napi_async_execute_callback execute,
                                     napi_async_complete_callback complete,
                                     void* data)
    : env(env), execute(execute), complete(complete), data(data) {}
napi_async_work__::~napi_async_work__() = default;

napi_status napi_create_async_work(napi_env env,
                                   napi_value resource,
                                   napi_value resource_name,
                                   napi_async_execute_callback execute,
                                   napi_async_complete_callback complete,
                                   void* data,
                                   napi_async_work* result) {
  if (!env || !execute || !result) return napi_invalid_arg;
  auto* work = new napi_async_work__(env, execute, complete, data);
  *result = work;
  return napi_ok;
}

napi_status napi_delete_async_work(napi_env env, napi_async_work work) {
  if (!env || !work) return napi_invalid_arg;
  delete work;
  return napi_ok;
}

napi_status napi_queue_async_work(napi_env env, napi_async_work work) {
  if (!env || !work) return napi_invalid_arg;
  work->main_task_runner = base::SingleThreadTaskRunner::GetCurrentDefault();
  
  base::ThreadPool::PostTaskAndReply(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce([](napi_async_work work) {
        work->execute(work->env, work->data);
      }, work),
      base::BindOnce([](napi_async_work work) {
        if (work->complete) {
          v8::Isolate* isolate = work->env->isolate;
          v8::Locker locker(isolate);
          v8::Isolate::Scope isolate_scope(isolate);
          v8::HandleScope handle_scope(isolate);
          v8::Context::Scope context_scope(work->env->GetContext());
          NapiValueScope value_scope(work->env);

          work->complete(work->env, napi_ok, work->data);
        }
      }, work));
  return napi_ok;
}

napi_status napi_cancel_async_work(napi_env env, napi_async_work work) {
  return napi_generic_failure;
}

struct napi_threadsafe_function__ {
  napi_threadsafe_function__();
  ~napi_threadsafe_function__();

  napi_env env;
  v8::Global<v8::Function> js_callback;
  NAPI_RAW_PTR_EXCLUSION void* context;
  napi_threadsafe_function_call_js call_js_cb;

  scoped_refptr<base::SingleThreadTaskRunner> main_task_runner;
  std::mutex mutex;
  std::condition_variable queue_condition;
  size_t max_queue_size = 0;
  size_t queue_size = 0;
  size_t thread_count = 0;
  bool closing = false;
  bool aborting = false;
  bool finalize_posted = false;

  NAPI_RAW_PTR_EXCLUSION void* thread_finalize_data;
  napi_finalize thread_finalize_cb;
};

napi_threadsafe_function__::napi_threadsafe_function__() = default;
napi_threadsafe_function__::~napi_threadsafe_function__() = default;

namespace {

void FinalizeThreadsafeFunction(napi_threadsafe_function func) {
  v8::Isolate* isolate = func->env->isolate;
  v8::Locker locker(isolate);
  v8::Isolate::Scope isolate_scope(isolate);
  v8::HandleScope handle_scope(isolate);
  v8::Context::Scope context_scope(func->env->GetContext());
  NapiValueScope value_scope(func->env);

  if (func->thread_finalize_cb) {
    func->thread_finalize_cb(func->env, func->thread_finalize_data,
                             func->context);
  }
  func->js_callback.Reset();
  delete func;
}

void ScheduleThreadsafeFunctionFinalizer(napi_threadsafe_function func) {
  bool should_post = false;
  {
    std::lock_guard<std::mutex> lock(func->mutex);
    if (func->closing && func->thread_count == 0 && func->queue_size == 0 &&
        !func->finalize_posted) {
      func->finalize_posted = true;
      should_post = true;
    }
  }
  if (should_post) {
    func->main_task_runner->PostTask(
        FROM_HERE, base::BindOnce(&FinalizeThreadsafeFunction, func));
  }
}

void RunThreadsafeFunctionCall(napi_threadsafe_function func, void* data) {
  bool aborting = false;
  {
    std::lock_guard<std::mutex> lock(func->mutex);
    aborting = func->aborting;
  }

  if (aborting) {
    if (func->call_js_cb) {
      func->call_js_cb(nullptr, nullptr, func->context, data);
    }
  } else {
    v8::Isolate* isolate = func->env->isolate;
    v8::Locker locker(isolate);
    v8::Isolate::Scope isolate_scope(isolate);
    v8::HandleScope handle_scope(isolate);
    v8::Context::Scope context_scope(func->env->GetContext());
    NapiValueScope value_scope(func->env);

    v8::Local<v8::Function> callback;
    if (!func->js_callback.IsEmpty()) {
      callback = func->js_callback.Get(isolate);
    }

    if (func->call_js_cb) {
      napi_value js_callback =
          callback.IsEmpty() ? nullptr : func->env->CreateValue(callback);
      func->call_js_cb(func->env, js_callback, func->context, data);
    } else if (!callback.IsEmpty()) {
      callback
          ->Call(func->env->GetContext(), v8::Undefined(isolate), 0, nullptr)
          .IsEmpty();
    }
  }

  {
    std::lock_guard<std::mutex> lock(func->mutex);
    CHECK_GT(func->queue_size, 0u);
    --func->queue_size;
  }
  func->queue_condition.notify_all();
  ScheduleThreadsafeFunctionFinalizer(func);
}

v8::Local<v8::Private> GetBufferPrivateKey(napi_env env) {
  return v8::Private::ForApi(env->isolate, v8::String::NewFromUtf8Literal(
                                               env->isolate, "napi::buffer"));
}

bool IsMarkedBuffer(napi_env env, v8::Local<v8::Value> value) {
  if (!value->IsObject()) {
    return false;
  }
  v8::Local<v8::Value> marker;
  return value.As<v8::Object>()
             ->GetPrivate(env->GetContext(), GetBufferPrivateKey(env))
             .ToLocal(&marker) &&
         marker->IsTrue();
}

}  // namespace

napi_status napi_create_threadsafe_function(napi_env env,
                                            napi_value func,
                                            napi_value resource,
                                            napi_value resource_name,
                                            size_t max_queue_size,
                                            size_t initial_thread_count,
                                            void* thread_finalize_data,
                                            napi_finalize thread_finalize_cb,
                                            void* context,
                                            napi_threadsafe_function_call_js call_js_cb,
                                            napi_threadsafe_function* result) {
  if (!env || !result || initial_thread_count == 0 ||
      !base::SingleThreadTaskRunner::HasCurrentDefault()) {
    return napi_invalid_arg;
  }
  if (func && !func->Get()->IsFunction()) {
    return napi_function_expected;
  }

  auto* tsfn = new napi_threadsafe_function__();
  tsfn->env = env;
  if (func) {
    tsfn->js_callback.Reset(env->isolate, func->Get().As<v8::Function>());
  }
  tsfn->context = context;
  tsfn->call_js_cb = call_js_cb;
  tsfn->main_task_runner = base::SingleThreadTaskRunner::GetCurrentDefault();
  tsfn->max_queue_size = max_queue_size;
  tsfn->thread_count = initial_thread_count;
  tsfn->thread_finalize_data = thread_finalize_data;
  tsfn->thread_finalize_cb = thread_finalize_cb;

  *result = tsfn;
  return napi_ok;
}

napi_status napi_get_threadsafe_function_context(napi_threadsafe_function func, void** result) {
  if (!func || !result) return napi_invalid_arg;
  *result = func->context;
  return napi_ok;
}

napi_status napi_call_threadsafe_function(napi_threadsafe_function func, void* data, napi_threadsafe_function_call_mode is_blocking) {
  if (!func) return napi_invalid_arg;

  {
    std::unique_lock<std::mutex> lock(func->mutex);
    if (func->closing) {
      return napi_closing;
    }
    if (func->max_queue_size > 0) {
      if (is_blocking == napi_tsfn_nonblocking &&
          func->queue_size >= func->max_queue_size) {
        return napi_queue_full;
      }
      if (is_blocking == napi_tsfn_blocking &&
          func->queue_size >= func->max_queue_size) {
        if (func->main_task_runner->RunsTasksInCurrentSequence()) {
          return napi_would_deadlock;
        }
        func->queue_condition.wait(lock, [func] {
          return func->closing || func->queue_size < func->max_queue_size;
        });
        if (func->closing) {
          return napi_closing;
        }
      }
    }
    ++func->queue_size;
  }

  if (!func->main_task_runner->PostTask(
          FROM_HERE, base::BindOnce(&RunThreadsafeFunctionCall, func, data))) {
    {
      std::lock_guard<std::mutex> lock(func->mutex);
      --func->queue_size;
    }
    func->queue_condition.notify_all();
    return napi_generic_failure;
  }

  return napi_ok;
}

napi_status napi_acquire_threadsafe_function(napi_threadsafe_function func) {
  if (!func) return napi_invalid_arg;
  std::lock_guard<std::mutex> lock(func->mutex);
  if (func->closing) {
    return napi_closing;
  }
  ++func->thread_count;
  return napi_ok;
}

napi_status napi_release_threadsafe_function(napi_threadsafe_function func, napi_threadsafe_function_release_mode mode) {
  if (!func) return napi_invalid_arg;
  {
    std::lock_guard<std::mutex> lock(func->mutex);
    if (func->thread_count == 0) {
      return napi_invalid_arg;
    }
    if (mode == napi_tsfn_abort) {
      func->aborting = true;
      func->closing = true;
    }
    --func->thread_count;
    if (func->thread_count == 0) {
      func->closing = true;
    }
  }
  func->queue_condition.notify_all();
  ScheduleThreadsafeFunctionFinalizer(func);
  return napi_ok;
}

napi_status napi_ref_threadsafe_function(napi_env env, napi_threadsafe_function func) {
  if (!env || !func) {
    return napi_invalid_arg;
  }
  return napi_ok;
}

napi_status napi_unref_threadsafe_function(napi_env env, napi_threadsafe_function func) {
  if (!env || !func) {
    return napi_invalid_arg;
  }
  return napi_ok;
}

napi_status napi_create_buffer(napi_env env, size_t length, void** data, napi_value* result) {
  if (!env || !result) {
    return napi_invalid_arg;
  }
  std::unique_ptr<v8::BackingStore> backing_store =
      v8::ArrayBuffer::NewBackingStore(env->isolate, length);
  void* buffer_data = backing_store->Data();
  v8::Local<v8::ArrayBuffer> array_buffer =
      v8::ArrayBuffer::New(env->isolate, std::move(backing_store));
  v8::Local<v8::Uint8Array> buffer =
      v8::Uint8Array::New(array_buffer, 0, length);
  if (!buffer.As<v8::Object>()
           ->SetPrivate(env->GetContext(), GetBufferPrivateKey(env),
                        v8::True(env->isolate))
           .FromMaybe(false)) {
    return napi_generic_failure;
  }
  if (data) {
    *data = buffer_data;
  }
  *result = env->CreateValue(buffer);
  return napi_ok;
}

napi_status napi_create_buffer_copy(napi_env env,
                                    size_t length,
                                    const void* data,
                                    void** result_data,
                                    napi_value* result) {
  if (!env || !result || (length > 0 && !data)) return napi_invalid_arg;
  void* buffer_data = nullptr;
  napi_status status = napi_create_buffer(env, length, &buffer_data, result);
  if (status != napi_ok) {
    return status;
  }
  if (length > 0) {
    // SAFETY: napi_create_buffer allocated |length| bytes; |data| is the
    // caller-provided source of the same length.
    UNSAFE_BUFFERS(base::span(static_cast<uint8_t*>(buffer_data), length))
        .copy_from(UNSAFE_BUFFERS(
            base::span(static_cast<const uint8_t*>(data), length)));
  }
  if (result_data) {
    *result_data = buffer_data;
  }
  return napi_ok;
}

napi_status napi_get_buffer_info(napi_env env, napi_value value, void** data, size_t* length) {
  if (!env || !value) return napi_invalid_arg;
  if (value->Get()->IsArrayBuffer()) {
    return napi_get_arraybuffer_info(env, value, data, length);
  }
  if (value->Get()->IsArrayBufferView()) {
    v8::Local<v8::ArrayBufferView> view =
        v8::Local<v8::ArrayBufferView>::Cast(value->Get());
    std::shared_ptr<v8::BackingStore> backing_store =
        view->Buffer()->GetBackingStore();
    if (data) {
      *data = static_cast<uint8_t*>(backing_store->Data()) + view->ByteOffset();
    }
    if (length) {
      *length = view->ByteLength();
    }
    return napi_ok;
  }
  return napi_invalid_arg;
}

napi_status napi_is_buffer(napi_env env, napi_value value, bool* result) {
  if (!env || !value || !result) return napi_invalid_arg;
  *result = IsMarkedBuffer(env, value->Get());
  return napi_ok;
}

napi_status napi_get_node_version(napi_env env, const napi_node_version** version) {
  if (!env || !version) {
    return napi_invalid_arg;
  }
  static napi_node_version node_version = {0, 0, 0, "xenon-napi-compat"};
  *version = &node_version;
  return napi_ok;
}

napi_status napi_get_uv_event_loop(napi_env env, uv_loop_t** loop) {
  if (!env || !loop) return napi_invalid_arg;
#if BUILDFLAG(ENABLE_XENON_NODE_UV_COMPAT)
  env->uv_loop = uv_default_loop();
  env->uv_run_function = reinterpret_cast<XenonUvRunFunction>(&uv_run);
  env->uv_loop_alive_function =
      reinterpret_cast<XenonUvLoopAliveFunction>(&uv_loop_alive);
#else
  env->uv_loop = reinterpret_cast<uv_loop_t*>(GetDefaultFakeUvLoop());
  env->uv_run_function = reinterpret_cast<XenonUvRunFunction>(&uv_run);
#endif
  *loop = env->uv_loop;
  return napi_ok;
}

NAPI_EXTERN void node_module_register(napi_module* mod) {
  napi_module_register(mod);
}
