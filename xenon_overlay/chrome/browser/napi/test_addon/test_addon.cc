// Copyright 2026 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <chrono>
#include <cstdint>
#include <thread>

#include "base/containers/span.h"
#include "node_api.h"

#if defined(XENON_TEST_UV_COMPAT)
#include "uv.h"
#endif

// 1. Synchronous Add
napi_value Add(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  double val1 = 0;
  double val2 = 0;
  napi_get_value_double(env, argv[0], &val1);
  napi_get_value_double(env, argv[1], &val2);

  napi_value sum;
  napi_create_double(env, val1 + val2, &sum);
  return sum;
}

// 2. Asynchronous Add (using napi_async_work)
struct AsyncData {
  double val1;
  double val2;
  double sum;
  napi_ref cb_ref;
  napi_async_work work;
};

void ExecuteWork(napi_env env, void* data) {
  auto* async_data = static_cast<AsyncData*>(data);
  async_data->sum = async_data->val1 + async_data->val2;
}

void CompleteWork(napi_env env, napi_status status, void* data) {
  auto* async_data = static_cast<AsyncData*>(data);

  napi_handle_scope scope;
  napi_open_handle_scope(env, &scope);

  napi_value js_sum;
  napi_create_double(env, async_data->sum, &js_sum);

  napi_value cb;
  napi_get_reference_value(env, async_data->cb_ref, &cb);

  napi_value global;
  napi_get_undefined(env, &global);

  napi_value argv[1] = {js_sum};
  napi_call_function(env, global, cb, 1, argv, nullptr);

  napi_delete_reference(env, async_data->cb_ref);
  napi_delete_async_work(env, async_data->work);
  napi_close_handle_scope(env, scope);

  delete async_data;
}

napi_value AsyncAdd(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  auto* async_data = new AsyncData();
  napi_get_value_double(env, argv[0], &async_data->val1);
  napi_get_value_double(env, argv[1], &async_data->val2);
  napi_create_reference(env, argv[2], 1, &async_data->cb_ref);

  napi_value resource = nullptr;
  napi_value resource_name = nullptr;
  napi_create_async_work(env, resource, resource_name, ExecuteWork,
                         CompleteWork, async_data, &async_data->work);
  napi_queue_async_work(env, async_data->work);

  return nullptr;
}

// 3. Thread-safe Callbacks (using napi_threadsafe_function)
napi_value StartThread(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  napi_threadsafe_function tsfn;
  napi_value resource = nullptr;
  napi_value resource_name = nullptr;

  napi_create_threadsafe_function(
      env,
      argv[0],  // Callback function
      resource, resource_name,
      0,        // max_queue_size
      1,        // initial_thread_count
      nullptr,  // thread_finalize_data
      nullptr,  // thread_finalize_cb
      nullptr,  // context
      [](napi_env env, napi_value js_cb, void* context, void* data) {
        // This runs on the JS main thread
        napi_value js_msg;
        napi_create_string_utf8(env, "Hello from Thread!", NAPI_AUTO_LENGTH,
                                &js_msg);
        napi_value global;
        napi_get_undefined(env, &global);
        napi_value argv[1] = {js_msg};
        napi_call_function(env, global, js_cb, 1, argv, nullptr);
      },
      &tsfn);

  // Spawn background thread
  std::thread bg_thread([tsfn]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    napi_call_threadsafe_function(tsfn, nullptr, napi_tsfn_blocking);
    napi_release_threadsafe_function(tsfn, napi_tsfn_release);
  });
  bg_thread.detach();

  return nullptr;
}

napi_value InspectTypes(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  napi_value result;
  napi_create_object(env, &result);

  napi_value input = nullptr;
  if (argc > 0) {
    input = argv[0];
  } else {
    napi_get_null(env, &input);
  }

  napi_value flag;
  bool flag_value = false;
  if (napi_get_named_property(env, input, "flag", &flag) == napi_ok) {
    napi_get_value_bool(env, flag, &flag_value);
  }
  napi_value js_flag;
  napi_get_boolean(env, flag_value, &js_flag);
  napi_set_named_property(env, result, "flag", js_flag);

  napi_value items;
  uint32_t item_count = 0;
  bool is_array = false;
  if (napi_get_named_property(env, input, "items", &items) == napi_ok) {
    napi_is_array(env, items, &is_array);
    if (is_array) {
      napi_get_array_length(env, items, &item_count);
    }
  }
  napi_value js_item_count;
  napi_create_uint32(env, item_count, &js_item_count);
  napi_set_named_property(env, result, "itemCount", js_item_count);

  napi_value maybe_null;
  bool null_ok = false;
  if (napi_get_named_property(env, input, "maybeNull", &maybe_null) ==
      napi_ok) {
    napi_value_type value_type = napi_undefined;
    napi_typeof(env, maybe_null, &value_type);
    null_ok = value_type == napi_null;
  }
  napi_value js_null_ok;
  napi_get_boolean(env, null_ok, &js_null_ok);
  napi_set_named_property(env, result, "nullOk", js_null_ok);

  napi_value summary;
  napi_create_string_utf8(env, "object/array/bool/null ok", NAPI_AUTO_LENGTH,
                          &summary);
  napi_set_named_property(env, result, "summary", summary);
  napi_set_named_property(env, result, "echo", input);

  return result;
}

napi_value BinaryEcho(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  void* data = nullptr;
  size_t byte_length = 0;
  if (argc == 0 ||
      napi_get_buffer_info(env, argv[0], &data, &byte_length) != napi_ok) {
    napi_throw_type_error(env, "ERR_XENON_BINARY",
                          "BinaryEcho expects an ArrayBuffer or Buffer");
    return nullptr;
  }

  uint32_t checksum = 0;
  for (uint8_t byte : UNSAFE_BUFFERS(base::span(
           static_cast<const uint8_t*>(data), byte_length))) {
    checksum += byte;
  }

  napi_value result;
  napi_create_object(env, &result);

  napi_value js_byte_length;
  napi_create_uint32(env, static_cast<uint32_t>(byte_length), &js_byte_length);
  napi_set_named_property(env, result, "byteLength", js_byte_length);

  napi_value js_checksum;
  napi_create_uint32(env, checksum, &js_checksum);
  napi_set_named_property(env, result, "checksum", js_checksum);

  napi_value copy;
  napi_create_buffer_copy(env, byte_length, data, nullptr, &copy);
  napi_set_named_property(env, result, "copy", copy);

  return result;
}

napi_value PromiseValue(napi_env env, napi_callback_info info) {
  napi_deferred deferred;
  napi_value promise;
  napi_create_promise(env, &deferred, &promise);

  napi_value payload;
  napi_create_object(env, &payload);

  napi_value ok;
  napi_get_boolean(env, true, &ok);
  napi_set_named_property(env, payload, "ok", ok);

  napi_value label;
  napi_create_string_utf8(env, "resolved from standard N-API promise",
                          NAPI_AUTO_LENGTH, &label);
  napi_set_named_property(env, payload, "label", label);

  napi_value values;
  napi_create_array_with_length(env, 3, &values);
  for (uint32_t i = 0; i < 3; ++i) {
    napi_value value;
    napi_create_uint32(env, i + 1, &value);
    napi_set_element(env, values, i, value);
  }
  napi_set_named_property(env, payload, "values", values);

  napi_resolve_deferred(env, deferred, payload);
  return promise;
}

napi_value MultiCallback(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  if (argc < 2) {
    napi_throw_type_error(env, "ERR_XENON_CALLBACK",
                          "MultiCallback expects two callback functions");
    return nullptr;
  }

  napi_value undefined;
  napi_get_undefined(env, &undefined);

  napi_value first_message;
  napi_create_string_utf8(env, "first callback payload", NAPI_AUTO_LENGTH,
                          &first_message);
  napi_call_function(env, undefined, argv[0], 1, &first_message, nullptr);

  napi_value second_payload;
  napi_create_object(env, &second_payload);
  napi_value second_ok;
  napi_get_boolean(env, true, &second_ok);
  napi_set_named_property(env, second_payload, "second", second_ok);
  napi_value second_count;
  napi_create_uint32(env, 2, &second_count);
  napi_set_named_property(env, second_payload, "count", second_count);
  napi_call_function(env, undefined, argv[1], 1, &second_payload, nullptr);

  napi_value result;
  napi_create_object(env, &result);
  napi_value status;
  napi_create_string_utf8(env, "callbacks-called", NAPI_AUTO_LENGTH, &status);
  napi_set_named_property(env, result, "status", status);
  napi_value count;
  napi_create_uint32(env, 2, &count);
  napi_set_named_property(env, result, "count", count);
  return result;
}

napi_value ThrowComplexError(napi_env env, napi_callback_info info) {
  napi_throw_type_error(env, "ERR_XENON_COMPLEX",
                        "Complex failure from addon: "
                        "{\"code\":\"ERR_XENON_COMPLEX\",\"detail\":\"object "
                        "payload rejected\"}");
  return nullptr;
}

#if defined(XENON_TEST_UV_COMPAT)
struct UvTimerData {
  napi_env env = nullptr;
  napi_ref callback = nullptr;
  uv_timer_t timer = {};
};

void OnUvTimerClosed(uv_handle_t* handle) {
  delete static_cast<UvTimerData*>(handle->data);
}

void OnUvTimer(uv_timer_t* timer) {
  auto* data = static_cast<UvTimerData*>(timer->data);
  napi_handle_scope scope;
  napi_open_handle_scope(data->env, &scope);

  napi_value callback;
  napi_value undefined;
  napi_value message;
  napi_get_reference_value(data->env, data->callback, &callback);
  napi_get_undefined(data->env, &undefined);
  napi_create_string_utf8(data->env, "Hello from libuv timer!",
                          NAPI_AUTO_LENGTH, &message);
  napi_call_function(data->env, undefined, callback, 1, &message, nullptr);

  napi_delete_reference(data->env, data->callback);
  napi_close_handle_scope(data->env, scope);
  uv_close(reinterpret_cast<uv_handle_t*>(timer), OnUvTimerClosed);
}

napi_value UvTimer(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 1) {
    napi_throw_type_error(env, "ERR_XENON_UV_TIMER",
                          "UvTimer expects a callback");
    return nullptr;
  }

  uv_loop_t* loop = nullptr;
  if (napi_get_uv_event_loop(env, &loop) != napi_ok || !loop) {
    napi_throw_error(env, "ERR_XENON_UV_LOOP",
                     "Failed to obtain the libuv event loop");
    return nullptr;
  }

  auto* data = new UvTimerData();
  data->env = env;
  napi_create_reference(env, argv[0], 1, &data->callback);
  if (uv_timer_init(loop, &data->timer) != 0) {
    napi_delete_reference(env, data->callback);
    delete data;
    napi_throw_error(env, "ERR_XENON_UV_TIMER",
                     "Failed to initialize the libuv timer");
    return nullptr;
  }

  data->timer.data = data;
  if (uv_timer_start(&data->timer, OnUvTimer, 50, 0) != 0) {
    napi_delete_reference(env, data->callback);
    data->callback = nullptr;
    uv_close(reinterpret_cast<uv_handle_t*>(&data->timer), OnUvTimerClosed);
    napi_throw_error(env, "ERR_XENON_UV_TIMER",
                     "Failed to start the libuv timer");
    return nullptr;
  }

  napi_value undefined;
  napi_get_undefined(env, &undefined);
  return undefined;
}
#endif

// Init Addon
extern "C" napi_value Init(napi_env env, napi_value exports) {
  napi_property_descriptor desc[] = {
      {"Add", nullptr, Add, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"AsyncAdd", nullptr, AsyncAdd, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"StartThread", nullptr, StartThread, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"InspectTypes", nullptr, InspectTypes, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"BinaryEcho", nullptr, BinaryEcho, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"PromiseValue", nullptr, PromiseValue, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"MultiCallback", nullptr, MultiCallback, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"ThrowComplexError", nullptr, ThrowComplexError, nullptr, nullptr,
       nullptr, napi_default, nullptr},
#if defined(XENON_TEST_UV_COMPAT)
      {"UvTimer", nullptr, UvTimer, nullptr, nullptr, nullptr, napi_default,
       nullptr},
#endif
  };
  napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
  return exports;
}

NAPI_MODULE(test_addon, Init)
