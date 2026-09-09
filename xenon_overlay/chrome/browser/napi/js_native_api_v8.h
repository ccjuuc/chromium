// Copyright 2026 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_NAPI_JS_NATIVE_API_V8_H_
#define XENON_OVERLAY_CHROME_BROWSER_NAPI_JS_NATIVE_API_V8_H_

#include <memory>
#include <unordered_set>
#include <vector>

#include "node_api.h"
#include "v8/include/v8.h"

struct napi_value__ {
  napi_value__();
  ~napi_value__();

  napi_value__(const napi_value__&) = delete;
  napi_value__& operator=(const napi_value__&) = delete;

  void Reset(v8::Isolate* isolate, v8::Local<v8::Value> local);
  v8::Local<v8::Value> Get() const;

  v8::Global<v8::Value> persistent;
  NAPI_RAW_PTR_EXCLUSION v8::Isolate* isolate_ = nullptr;
};

struct napi_handle_scope__ {
  napi_handle_scope__(v8::Isolate* isolate, size_t index);
  ~napi_handle_scope__();
  v8::HandleScope v8_scope;
  size_t start_value_index;
};

struct napi_escapable_handle_scope__ {
  napi_escapable_handle_scope__(v8::Isolate* isolate, size_t index);
  ~napi_escapable_handle_scope__();
  v8::EscapableHandleScope v8_scope;
  size_t start_value_index;
  std::unique_ptr<napi_value__> escaped_value;
};

struct napi_ref__ {
  explicit napi_ref__(v8::Isolate* isolate);
  ~napi_ref__();

  void SetWeak();
  void ClearWeak();

  NAPI_RAW_PTR_EXCLUSION v8::Isolate* isolate;
  v8::Global<v8::Value> global_value;
  uint32_t ref_count = 0;
  bool is_weak = false;
};

struct napi_deferred__ {
  explicit napi_deferred__(napi_env env);
  ~napi_deferred__();

  napi_env env;
  v8::Global<v8::Promise::Resolver> resolver;
};

struct napi_callback_info__ {
  NAPI_RAW_PTR_EXCLUSION const v8::FunctionCallbackInfo<v8::Value>& v8_info;
  NAPI_RAW_PTR_EXCLUSION void* data;
};

struct CallbackData;
struct NapiFinalizerData;

using XenonUvRunFunction = int (*)(uv_loop_t* loop, int mode);
using XenonUvLoopAliveFunction = int (*)(const uv_loop_t* loop);

struct napi_env__ {
  napi_env__(v8::Isolate* iso, v8::Local<v8::Context> ctx);
  ~napi_env__();

  NAPI_RAW_PTR_EXCLUSION v8::Isolate* isolate;
  v8::Global<v8::Context> context;
  v8::Global<v8::Value> last_exception;
  NAPI_RAW_PTR_EXCLUSION uv_loop_t* uv_loop = nullptr;
  NAPI_RAW_PTR_EXCLUSION XenonUvRunFunction uv_run_function = nullptr;
  NAPI_RAW_PTR_EXCLUSION XenonUvLoopAliveFunction uv_loop_alive_function =
      nullptr;

  std::vector<std::unique_ptr<napi_value__>> allocated_values;
  std::unordered_set<napi_value> live_values;
  // Function/method CallbackData must outlive exported Functions; kept on the
  // env (env itself is intentionally not torn down by the weak context hook).
  std::vector<std::unique_ptr<CallbackData>> callbacks;
  std::vector<std::unique_ptr<NapiFinalizerData>> finalizers;
  NAPI_RAW_PTR_EXCLUSION void* instance_data = nullptr;
  napi_finalize instance_data_finalize = nullptr;
  NAPI_RAW_PTR_EXCLUSION void* instance_data_finalize_hint = nullptr;
  napi_extended_error_info last_error_info = {nullptr, nullptr, 0, napi_ok};

  napi_value CreateValue(v8::Local<v8::Value> local_val);
  void TrimAllocatedValues(size_t new_size);
  bool IsLiveValue(napi_value v) const;

  v8::Local<v8::Context> GetContext() const {
    return context.Get(isolate);
  }
};

namespace v8impl {
inline v8::Local<v8::Value> V8LocalValueFromJsValue(napi_env env,
                                                    napi_value v) {
  if (!env || !env->IsLiveValue(v)) {
    return v8::Local<v8::Value>();
  }
  return v->Get();
}

inline napi_value JsValueFromV8LocalValue(napi_env env, v8::Local<v8::Value> v) {
  if (v.IsEmpty()) return nullptr;
  return env->CreateValue(v);
}
}  // namespace v8impl

#endif  // XENON_OVERLAY_CHROME_BROWSER_NAPI_JS_NATIVE_API_V8_H_
