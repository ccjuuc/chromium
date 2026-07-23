// Copyright 2026 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_NAPI_NODE_API_H_
#define XENON_OVERLAY_CHROME_BROWSER_NAPI_NODE_API_H_

#include "js_native_api.h"

// Node.js specific types
typedef struct napi_async_work__* napi_async_work;
typedef struct napi_threadsafe_function__* napi_threadsafe_function;
typedef struct napi_async_cleanup_hook_handle__* napi_async_cleanup_hook_handle;
typedef struct uv_loop_s uv_loop_t;

typedef void (*napi_async_execute_callback)(napi_env env, void* data);
typedef void (*napi_async_complete_callback)(napi_env env, napi_status status, void* data);

typedef enum {
  napi_tsfn_release,
  napi_tsfn_abort
} napi_threadsafe_function_release_mode;

typedef enum {
  napi_tsfn_nonblocking,
  napi_tsfn_blocking
} napi_threadsafe_function_call_mode;

typedef void (*napi_threadsafe_function_call_js)(napi_env env, napi_value js_callback, void* context, void* data);

// Node.js specific functions (stubs/custom implementations)
NAPI_EXTERN napi_status napi_create_async_work(napi_env env,
                                               napi_value resource,
                                               napi_value resource_name,
                                               napi_async_execute_callback execute,
                                               napi_async_complete_callback complete,
                                               void* data,
                                               napi_async_work* result);
NAPI_EXTERN napi_status napi_delete_async_work(napi_env env, napi_async_work work);
NAPI_EXTERN napi_status napi_queue_async_work(napi_env env, napi_async_work work);
NAPI_EXTERN napi_status napi_cancel_async_work(napi_env env, napi_async_work work);

NAPI_EXTERN napi_status napi_create_threadsafe_function(napi_env env,
                                                        napi_value func,
                                                        napi_value resource,
                                                        napi_value resource_name,
                                                        size_t max_queue_size,
                                                        size_t initial_thread_count,
                                                        void* thread_finalize_data,
                                                        napi_finalize thread_finalize_cb,
                                                        void* context,
                                                        napi_threadsafe_function_call_js call_js_cb,
                                                        napi_threadsafe_function* result);
NAPI_EXTERN napi_status napi_get_threadsafe_function_context(napi_threadsafe_function func, void** result);
NAPI_EXTERN napi_status napi_call_threadsafe_function(napi_threadsafe_function func, void* data, napi_threadsafe_function_call_mode is_blocking);
NAPI_EXTERN napi_status napi_acquire_threadsafe_function(napi_threadsafe_function func);
NAPI_EXTERN napi_status napi_release_threadsafe_function(napi_threadsafe_function func, napi_threadsafe_function_release_mode mode);
NAPI_EXTERN napi_status napi_ref_threadsafe_function(napi_env env, napi_threadsafe_function func);
NAPI_EXTERN napi_status napi_unref_threadsafe_function(napi_env env, napi_threadsafe_function func);

NAPI_EXTERN napi_status napi_create_buffer(napi_env env, size_t length, void** data, napi_value* result);
NAPI_EXTERN napi_status napi_create_buffer_copy(napi_env env, size_t length, const void* data, void** result_data, napi_value* result);
NAPI_EXTERN napi_status napi_get_buffer_info(napi_env env, napi_value value, void** data, size_t* length);
NAPI_EXTERN napi_status napi_is_buffer(napi_env env, napi_value value, bool* result);

NAPI_EXTERN napi_status napi_get_node_version(napi_env env, const napi_node_version** version);
NAPI_EXTERN napi_status napi_get_uv_event_loop(napi_env env, uv_loop_t** loop);

// Node module structures
typedef struct napi_module {
  int nm_version;
  unsigned int nm_flags;
  const char* nm_filename;
  napi_addon_register_func nm_register_func;
  const char* nm_modname;
  NAPI_RAW_PTR_EXCLUSION void* nm_priv;
  NAPI_RAW_PTR_EXCLUSION void* reserved[4];
} napi_module;

NAPI_EXTERN void napi_module_register(napi_module* mod);
NAPI_EXTERN void xenon_napi_clear_registered_modules(void);
NAPI_EXTERN size_t xenon_napi_get_registered_module_count(void);
NAPI_EXTERN napi_module* xenon_napi_get_registered_module(size_t index);
NAPI_EXTERN bool xenon_napi_remove_registered_module(napi_module* mod);
NAPI_EXTERN napi_module* xenon_napi_get_last_registered_module(void);

#define NAPI_MODULE(modname, regfunc)                                 \
  EXTERN_C napi_value regfunc(napi_env env, napi_value exports);      \
  NAPI_EXTERN napi_value napi_register_module_v1(napi_env env, napi_value exports) { \
    return regfunc(env, exports);                                     \
  }

#endif  // XENON_OVERLAY_CHROME_BROWSER_NAPI_NODE_API_H_
