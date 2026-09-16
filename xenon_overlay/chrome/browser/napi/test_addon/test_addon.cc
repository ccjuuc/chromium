// Copyright 2026 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <chrono>
#include <cstdint>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <thread>

#include "base/containers/span.h"
#include "node_api.h"

#if defined(_WIN32)
#include <windows.h>
#endif

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

#if defined(_WIN32)
napi_value CanLoadLibrary(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  size_t utf8_length = 0;
  if (argc != 1 || napi_get_value_string_utf8(env, argv[0], nullptr, 0,
                                              &utf8_length) != napi_ok) {
    napi_throw_type_error(env, nullptr, "CanLoadLibrary expects a path");
    return nullptr;
  }
  std::string utf8_path(utf8_length + 1, '\0');
  size_t copied = 0;
  if (napi_get_value_string_utf8(env, argv[0], utf8_path.data(),
                                 utf8_path.size(), &copied) != napi_ok) {
    napi_throw_type_error(env, nullptr, "Cannot read library path");
    return nullptr;
  }
  utf8_path.resize(copied);

  const int wide_length =
      ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8_path.data(),
                            static_cast<int>(copied), nullptr, 0);
  std::wstring wide_path;
  if (wide_length > 0) {
    wide_path.resize(wide_length);
    ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8_path.data(),
                          static_cast<int>(copied), wide_path.data(),
                          wide_length);
  }

  HMODULE library =
      wide_path.empty() ? nullptr : ::LoadLibraryW(wide_path.c_str());
  const bool loaded = library != nullptr;
  if (library) {
    ::FreeLibrary(library);
  }
  napi_value result;
  napi_get_boolean(env, loaded, &result);
  return result;
}
#endif

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

napi_value InvokeNestedCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 1) {
    napi_throw_type_error(env, "ERR_XENON_NESTED_CALLBACK",
                          "InvokeNestedCallback expects an options object");
    return nullptr;
  }

  napi_value handlers;
  napi_value first;
  napi_value callback;
  bool is_array = false;
  if (napi_get_named_property(env, argv[0], "handlers", &handlers) != napi_ok ||
      napi_is_array(env, handlers, &is_array) != napi_ok || !is_array ||
      napi_get_element(env, handlers, 0, &first) != napi_ok ||
      napi_get_named_property(env, first, "onValue", &callback) != napi_ok) {
    napi_throw_type_error(env, "ERR_XENON_NESTED_CALLBACK",
                          "options.handlers[0].onValue is required");
    return nullptr;
  }

  napi_value undefined;
  napi_value payload;
  napi_get_undefined(env, &undefined);
  napi_create_string_utf8(env, "nested callback payload", NAPI_AUTO_LENGTH,
                          &payload);
  napi_call_function(env, undefined, callback, 1, &payload, nullptr);
  return undefined;
}

napi_value AddOne(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  int32_t value = 0;
  if (argc > 0) {
    napi_get_value_int32(env, argv[0], &value);
  }
  napi_value result;
  napi_create_int32(env, value + 1, &result);
  return result;
}

napi_value InvokeCallbackWithFunction(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 1) {
    napi_throw_type_error(env, "ERR_XENON_FUNCTION_CALLBACK",
                          "InvokeCallbackWithFunction expects a callback");
    return nullptr;
  }

  napi_value callable;
  napi_create_function(env, "addOne", NAPI_AUTO_LENGTH, AddOne, nullptr,
                       &callable);
  napi_value undefined;
  napi_get_undefined(env, &undefined);
  napi_call_function(env, undefined, argv[0], 1, &callable, nullptr);
  return undefined;
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

// Controllable real N-API Promises let the executor tests settle operations
// before or after subscribing, without timers or a second native invocation.
struct ControlledPromiseState {
  uint32_t calls = 0;
  std::map<uint32_t, napi_deferred> pending;
  std::map<uint32_t, napi_ref> retained_callbacks;
  uint32_t callback_payload_reads = 0;
};

ControlledPromiseState* GetControlledPromiseState(napi_env env) {
  void* state = nullptr;
  napi_get_instance_data(env, &state);
  return static_cast<ControlledPromiseState*>(state);
}

napi_value ReadOwnedHandle(napi_env env, napi_callback_info info) {
  napi_value receiver;
  napi_get_cb_info(env, info, nullptr, nullptr, &receiver, nullptr);
  napi_value result;
  napi_get_named_property(env, receiver, "value", &result);
  return result;
}

napi_value MakeOwnedObject(napi_env env, int value) {
  napi_value object;
  napi_create_object(env, &object);
  napi_value number;
  napi_create_int32(env, value, &number);
  napi_set_named_property(env, object, "value", number);
  napi_property_descriptor read = {"read",  nullptr, ReadOwnedHandle, nullptr,
                                   nullptr, nullptr, napi_default,    nullptr};
  napi_define_properties(env, object, 1, &read);
  return object;
}

napi_value OwnedHandleConstructor(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_value receiver;
  napi_get_cb_info(env, info, &argc, argv, &receiver, nullptr);
  napi_value value;
  napi_create_int32(env, 42, &value);
  napi_set_named_property(env, receiver, "value", value);
  napi_set_named_property(env, receiver, "child", MakeOwnedObject(env, 7));
  if (argc) {
    napi_valuetype type;
    if (napi_typeof(env, argv[0], &type) == napi_ok && type == napi_function &&
        napi_call_function(env, receiver, argv[0], 1, &receiver, nullptr) !=
            napi_ok) {
      return nullptr;
    }
  }
  return receiver;
}

napi_value InvokeCallbackWithReceiver(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc != 2) {
    napi_throw_type_error(env, nullptr, "A callback and receiver are required");
    return nullptr;
  }
  if (napi_call_function(env, argv[1], argv[0], 1, &argv[1], nullptr) !=
      napi_ok) {
    return nullptr;
  }
  return argv[1];
}

napi_value EmitPrototypeCallback(napi_env env,
                                 napi_value receiver,
                                 const char* event) {
  napi_value emit;
  napi_valuetype type;
  if (napi_get_named_property(env, receiver, "emit", &emit) != napi_ok ||
      napi_typeof(env, emit, &type) != napi_ok) {
    return nullptr;
  }
  const bool present = type == napi_function;
  if (present) {
    napi_value name;
    napi_create_string_utf8(env, event, NAPI_AUTO_LENGTH, &name);
    if (napi_call_function(env, receiver, emit, 1, &name, nullptr) != napi_ok) {
      return nullptr;
    }
  }
  napi_value result;
  napi_get_boolean(env, present, &result);
  return result;
}

napi_value PrototypeCallbackConstructor(napi_env env, napi_callback_info info) {
  napi_value receiver;
  napi_get_cb_info(env, info, nullptr, nullptr, &receiver, nullptr);
  auto value = std::make_unique<int>(42);
  if (napi_wrap(
          env, receiver, value.get(),
          [](napi_env, void* data, void*) { delete static_cast<int*>(data); },
          nullptr, nullptr) != napi_ok) {
    return nullptr;
  }
  value.release();
  if (!EmitPrototypeCallback(env, receiver, "constructed")) {
    return nullptr;
  }
  return receiver;
}

napi_value ReadPrototypeCallback(napi_env env, napi_callback_info info) {
  napi_value receiver;
  napi_get_cb_info(env, info, nullptr, nullptr, &receiver, nullptr);
  void* data = nullptr;
  if (napi_unwrap(env, receiver, &data) != napi_ok || !data) {
    return nullptr;
  }
  napi_value result;
  napi_create_int32(env, *static_cast<int*>(data), &result);
  return result;
}

napi_value FirePrototypeCallback(napi_env env, napi_callback_info info) {
  napi_value receiver;
  napi_get_cb_info(env, info, nullptr, nullptr, &receiver, nullptr);
  return EmitPrototypeCallback(env, receiver, "later");
}

struct AsyncPrototypeState {
  napi_ref wrapper = nullptr;
  napi_ref callback = nullptr;
  napi_async_work work = nullptr;
  int value = 0;
  napi_status completion_status = napi_ok;
};

void FinalizeAsyncPrototype(napi_env env, void* data, void*) {
  auto* state = static_cast<AsyncPrototypeState*>(data);
  if (state->wrapper) {
    napi_delete_reference(env, state->wrapper);
  }
  delete state;
}

void ExecuteAsyncPrototype(napi_env, void* data) {
  static_cast<AsyncPrototypeState*>(data)->value = 42;
}

void CompleteAsyncPrototype(napi_env env, napi_status status, void* data) {
  auto* state = static_cast<AsyncPrototypeState*>(data);
  napi_value receiver = nullptr;
  napi_value callback = nullptr;
  state->completion_status = status;
  if (status == napi_ok) {
    state->completion_status =
        napi_get_reference_value(env, state->wrapper, &receiver);
  }
  if (state->completion_status == napi_ok && receiver) {
    state->completion_status =
        napi_get_reference_value(env, state->callback, &callback);
  }
  if (state->completion_status == napi_ok && receiver && callback) {
    napi_value error;
    napi_get_null(env, &error);
    state->completion_status =
        napi_call_function(env, receiver, callback, 1, &error, nullptr);
    // Match sqlite3's Work_AfterOpen: the constructor callback runs before
    // native code reads and calls the instance's inherited emit method.
    if (state->completion_status == napi_ok &&
        !EmitPrototypeCallback(env, receiver, "open")) {
      state->completion_status = napi_generic_failure;
    }
  } else if (state->completion_status == napi_ok) {
    state->completion_status = napi_invalid_arg;
  }
  napi_delete_reference(env, state->callback);
  state->callback = nullptr;
  napi_delete_async_work(env, state->work);
  state->work = nullptr;
  napi_reference_unref(env, state->wrapper, nullptr);
}

napi_value AsyncPrototypeCallbackConstructor(napi_env env,
                                             napi_callback_info info) {
  size_t argc = 1;
  napi_value callback;
  napi_value receiver;
  napi_get_cb_info(env, info, &argc, &callback, &receiver, nullptr);
  napi_valuetype type;
  if (argc != 1 || napi_typeof(env, callback, &type) != napi_ok ||
      type != napi_function) {
    napi_throw_type_error(env, nullptr, "A constructor callback is required");
    return nullptr;
  }
  auto state = std::make_unique<AsyncPrototypeState>();
  if (napi_wrap(env, receiver, state.get(), FinalizeAsyncPrototype, nullptr,
                &state->wrapper) != napi_ok) {
    return nullptr;
  }
  napi_status status = napi_reference_ref(env, state->wrapper, nullptr);
  if (status == napi_ok) {
    status = napi_create_reference(env, callback, 1, &state->callback);
  }
  if (status == napi_ok) {
    status = napi_create_async_work(
        env, nullptr, nullptr, ExecuteAsyncPrototype, CompleteAsyncPrototype,
        state.get(), &state->work);
  }
  if (status == napi_ok) {
    status = napi_queue_async_work(env, state->work);
  }
  if (status != napi_ok) {
    if (state->work) {
      napi_delete_async_work(env, state->work);
    }
    if (state->callback) {
      napi_delete_reference(env, state->callback);
    }
    napi_delete_reference(env, state->wrapper);
    napi_remove_wrap(env, receiver, nullptr);
    napi_throw_error(env, nullptr, "Could not queue prototype callback work");
    return nullptr;
  }
  state.release();
  return receiver;
}

napi_value ReadAsyncPrototypeCallback(napi_env env, napi_callback_info info) {
  napi_value receiver;
  napi_get_cb_info(env, info, nullptr, nullptr, &receiver, nullptr);
  void* data = nullptr;
  if (napi_unwrap(env, receiver, &data) != napi_ok || !data) {
    return nullptr;
  }
  const auto* state = static_cast<AsyncPrototypeState*>(data);
  napi_value result;
  napi_create_int32(
      env, state->completion_status == napi_ok ? state->value : -1, &result);
  return result;
}

napi_value MakeOwnedReturns(napi_env env, napi_callback_info info) {
  napi_value object = MakeOwnedObject(env, 42);
  napi_set_named_property(env, object, "child", MakeOwnedObject(env, 7));
  napi_value callable;
  napi_create_function(env, "addOne", NAPI_AUTO_LENGTH, AddOne, nullptr,
                       &callable);
  napi_value array;
  napi_create_array_with_length(env, 2, &array);
  napi_set_element(env, array, 0, object);
  napi_set_element(env, array, 1, callable);
  return array;
}

napi_value EchoOwnedHandle(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  return argc == 1 ? argv[0] : nullptr;
}

napi_value ResolvedOwnedPromise(napi_env env, napi_callback_info info) {
  napi_deferred deferred;
  napi_value promise;
  napi_create_promise(env, &deferred, &promise);
  napi_resolve_deferred(env, deferred, MakeOwnedReturns(env, info));
  return promise;
}

napi_value BeginControlledPromise(napi_env env, napi_callback_info info) {
  auto* state = GetControlledPromiseState(env);
  napi_deferred deferred;
  napi_value promise;
  napi_create_promise(env, &deferred, &promise);
  state->pending.emplace(++state->calls, deferred);
  return promise;
}

napi_value PendingPromiseWithMicrotaskCallback(napi_env env,
                                               napi_callback_info info) {
  size_t argc = 1;
  napi_value callback;
  napi_get_cb_info(env, info, &argc, &callback, nullptr, nullptr);
  napi_deferred deferred;
  napi_value ready;
  napi_value undefined;
  napi_create_promise(env, &deferred, &ready);
  napi_get_undefined(env, &undefined);
  napi_resolve_deferred(env, deferred, undefined);
  napi_value then;
  napi_get_named_property(env, ready, "then", &then);
  napi_call_function(env, ready, then, 1, &callback, nullptr);
  return BeginControlledPromise(env, info);
}

napi_value ControlledPromiseCallCount(napi_env env, napi_callback_info info) {
  napi_value result;
  napi_create_uint32(env, GetControlledPromiseState(env)->calls, &result);
  return result;
}

napi_value SettleControlledPromise(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  uint32_t id = 0;
  bool reject = false;
  napi_get_value_uint32(env, argv[0], &id);
  napi_get_value_bool(env, argv[1], &reject);
  auto* state = GetControlledPromiseState(env);
  auto it = state->pending.find(id);
  if (it != state->pending.end()) {
    napi_value result;
    napi_create_uint32(env, id, &result);
    if (reject) {
      napi_reject_deferred(env, it->second, result);
    } else {
      napi_resolve_deferred(env, it->second, result);
    }
    state->pending.erase(it);
  }
  return ControlledPromiseCallCount(env, info);
}

napi_value SettleControlledPromiseWithHandles(napi_env env,
                                              napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  uint32_t id = 0;
  if (argc != 1 || napi_get_value_uint32(env, argv[0], &id) != napi_ok) {
    napi_throw_type_error(env, nullptr, "A Promise operation id is required");
    return nullptr;
  }
  auto* state = GetControlledPromiseState(env);
  auto pending = state->pending.find(id);
  if (pending != state->pending.end()) {
    napi_resolve_deferred(env, pending->second, MakeOwnedReturns(env, info));
    state->pending.erase(pending);
  }
  return ControlledPromiseCallCount(env, info);
}

napi_value RetainCallback(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  uint32_t slot = 0;
  napi_valuetype type;
  if (argc != 2 || napi_get_value_uint32(env, argv[0], &slot) != napi_ok ||
      napi_typeof(env, argv[1], &type) != napi_ok || type != napi_function) {
    napi_throw_type_error(env, nullptr, "A slot and callback are required");
    return nullptr;
  }
  auto* state = GetControlledPromiseState(env);
  napi_ref& retained = state->retained_callbacks[slot];
  if (retained) {
    napi_delete_reference(env, retained);
  }
  napi_create_reference(env, argv[1], 1, &retained);
  napi_value undefined;
  napi_get_undefined(env, &undefined);
  return undefined;
}

napi_value ReadCallbackPayload(napi_env env, napi_callback_info info) {
  napi_value result;
  napi_create_uint32(
      env, ++GetControlledPromiseState(env)->callback_payload_reads, &result);
  return result;
}

napi_value CallRetainedCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  uint32_t slot = 0;
  if (argc != 1 || napi_get_value_uint32(env, argv[0], &slot) != napi_ok) {
    napi_throw_type_error(env, nullptr, "A retained callback slot is required");
    return nullptr;
  }
  auto* state = GetControlledPromiseState(env);
  const auto retained = state->retained_callbacks.find(slot);
  if (retained == state->retained_callbacks.end()) {
    napi_throw_error(env, nullptr, "Unknown retained callback slot");
    return nullptr;
  }
  napi_value callback;
  napi_get_reference_value(env, retained->second, &callback);
  napi_value payload;
  napi_create_object(env, &payload);
  napi_property_descriptor property = {
      "value", nullptr, nullptr,         ReadCallbackPayload,
      nullptr, nullptr, napi_enumerable, nullptr};
  napi_define_properties(env, payload, 1, &property);
  napi_value undefined;
  napi_get_undefined(env, &undefined);
  if (napi_call_function(env, undefined, callback, 1, &payload, nullptr) !=
      napi_ok) {
    return nullptr;
  }
  napi_value result;
  napi_create_uint32(env, state->callback_payload_reads, &result);
  return result;
}

void FinalizeControlledPromises(napi_env env, void* data, void* hint) {
  auto* state = static_cast<ControlledPromiseState*>(data);
  napi_value value;
  napi_get_undefined(env, &value);
  for (const auto& [id, deferred] : state->pending) {
    napi_resolve_deferred(env, deferred, value);
  }
  for (const auto& [slot, callback] : state->retained_callbacks) {
    napi_delete_reference(env, callback);
  }
  delete state;
}

// Init Addon
extern "C" napi_value Init(napi_env env, napi_value exports) {
  napi_set_instance_data(env, new ControlledPromiseState(),
                         FinalizeControlledPromises, nullptr);
  napi_property_descriptor desc[] = {
      {"Add", nullptr, Add, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"AsyncAdd", nullptr, AsyncAdd, nullptr, nullptr, nullptr, napi_default,
       nullptr},
#if defined(_WIN32)
      {"CanLoadLibrary", nullptr, CanLoadLibrary, nullptr, nullptr, nullptr,
       napi_default, nullptr},
#endif
      {"StartThread", nullptr, StartThread, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"InspectTypes", nullptr, InspectTypes, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"BinaryEcho", nullptr, BinaryEcho, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"PromiseValue", nullptr, PromiseValue, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"BeginControlledPromise", nullptr, BeginControlledPromise, nullptr,
       nullptr, nullptr, napi_default, nullptr},
      {"PendingPromiseWithMicrotaskCallback", nullptr,
       PendingPromiseWithMicrotaskCallback, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"ControlledPromiseCallCount", nullptr, ControlledPromiseCallCount,
       nullptr, nullptr, nullptr, napi_default, nullptr},
      {"SettleControlledPromise", nullptr, SettleControlledPromise, nullptr,
       nullptr, nullptr, napi_default, nullptr},
      {"SettleControlledPromiseWithHandles", nullptr,
       SettleControlledPromiseWithHandles, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"MakeOwnedReturns", nullptr, MakeOwnedReturns, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"EchoOwnedHandle", nullptr, EchoOwnedHandle, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"ResolvedOwnedPromise", nullptr, ResolvedOwnedPromise, nullptr, nullptr,
       nullptr, napi_default, nullptr},
      {"RetainCallback", nullptr, RetainCallback, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"CallRetainedCallback", nullptr, CallRetainedCallback, nullptr, nullptr,
       nullptr, napi_default, nullptr},
      {"MultiCallback", nullptr, MultiCallback, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"InvokeNestedCallback", nullptr, InvokeNestedCallback, nullptr, nullptr,
       nullptr, napi_default, nullptr},
      {"InvokeCallbackWithFunction", nullptr, InvokeCallbackWithFunction,
       nullptr, nullptr, nullptr, napi_default, nullptr},
      {"InvokeCallbackWithReceiver", nullptr, InvokeCallbackWithReceiver,
       nullptr, nullptr, nullptr, napi_default, nullptr},
      {"ThrowComplexError", nullptr, ThrowComplexError, nullptr, nullptr,
       nullptr, napi_default, nullptr},
#if defined(XENON_TEST_UV_COMPAT)
      {"UvTimer", nullptr, UvTimer, nullptr, nullptr, nullptr, napi_default,
       nullptr},
#endif
  };
  napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
  napi_property_descriptor read = {"read",  nullptr, ReadOwnedHandle, nullptr,
                                   nullptr, nullptr, napi_default,    nullptr};
  napi_value constructor;
  napi_define_class(env, "OwnedHandle", NAPI_AUTO_LENGTH,
                    OwnedHandleConstructor, nullptr, 1, &read, &constructor);
  napi_set_named_property(env, exports, "OwnedHandle", constructor);
  napi_property_descriptor named_methods[] = {
      {"bind", nullptr, ReadOwnedHandle, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"call", nullptr, ReadOwnedHandle, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"apply", nullptr, ReadOwnedHandle, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"toString", nullptr, ReadOwnedHandle, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"valueOf", nullptr, ReadOwnedHandle, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"name", nullptr, ReadOwnedHandle, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"length", nullptr, ReadOwnedHandle, nullptr, nullptr, nullptr,
       napi_default, nullptr},
  };
  napi_define_class(env, "NamedPrototypeMethods", NAPI_AUTO_LENGTH,
                    OwnedHandleConstructor, nullptr, std::size(named_methods),
                    named_methods, &constructor);
  napi_set_named_property(env, exports, "NamedPrototypeMethods", constructor);
  napi_property_descriptor prototype_methods[] = {
      {"read", nullptr, ReadPrototypeCallback, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"fire", nullptr, FirePrototypeCallback, nullptr, nullptr, nullptr,
       napi_default, nullptr},
  };
  napi_define_class(
      env, "PrototypeCallback", NAPI_AUTO_LENGTH, PrototypeCallbackConstructor,
      nullptr, std::size(prototype_methods), prototype_methods, &constructor);
  napi_set_named_property(env, exports, "PrototypeCallback", constructor);
  napi_property_descriptor async_read = {
      "read",       nullptr, ReadAsyncPrototypeCallback,
      nullptr,      nullptr, nullptr,
      napi_default, nullptr};
  napi_define_class(env, "AsyncPrototypeCallback", NAPI_AUTO_LENGTH,
                    AsyncPrototypeCallbackConstructor, nullptr, 1, &async_read,
                    &constructor);
  napi_set_named_property(env, exports, "AsyncPrototypeCallback", constructor);
  return exports;
}

NAPI_MODULE(test_addon, Init)
