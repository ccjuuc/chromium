// Copyright 2026 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_NAPI_JS_NATIVE_API_TYPES_H_
#define XENON_OVERLAY_CHROME_BROWSER_NAPI_JS_NATIVE_API_TYPES_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#define EXTERN_C extern "C"
#else
#define EXTERN_C
#endif

#ifdef _WIN32
#define NAPI_EXTERN EXTERN_C __declspec(dllexport)
#else
#define NAPI_EXTERN EXTERN_C __attribute__((visibility("default")))
#endif

#if defined(__clang__)
#define NAPI_RAW_PTR_EXCLUSION __attribute__((annotate("raw_ptr_exclusion")))
#define NAPI_RAW_REF_EXCLUSION __attribute__((annotate("raw_ref_exclusion")))
#else
#define NAPI_RAW_PTR_EXCLUSION
#define NAPI_RAW_REF_EXCLUSION
#endif

typedef struct napi_env__* napi_env;
typedef struct napi_value__* napi_value;
typedef struct napi_ref__* napi_ref;
typedef struct napi_handle_scope__* napi_handle_scope;
typedef struct napi_escapable_handle_scope__* napi_escapable_handle_scope;
typedef struct napi_callback_info__* napi_callback_info;
typedef struct napi_deferred__* napi_deferred;

typedef enum {
  napi_ok,
  napi_invalid_arg,
  napi_object_expected,
  napi_string_expected,
  napi_name_expected,
  napi_function_expected,
  napi_number_expected,
  napi_boolean_expected,
  napi_array_expected,
  napi_generic_failure,
  napi_pending_exception,
  napi_cancelled,
  napi_escape_called_twice,
  napi_handle_scope_mismatch,
  napi_callback_scope_mismatch,
  napi_queue_full,
  napi_closing,
  napi_bigint_expected,
  napi_date_expected,
  napi_arraybuffer_expected,
  napi_detachable_arraybuffer_expected,
  napi_would_deadlock,
  napi_no_external_buffers_allowed
} napi_status;

typedef enum {
  napi_undefined,
  napi_null,
  napi_boolean,
  napi_number,
  napi_string,
  napi_symbol,
  napi_object,
  napi_function,
  napi_external,
  napi_bigint,
} napi_valuetype;

typedef napi_valuetype napi_value_type;

typedef enum {
  napi_int8_array,
  napi_uint8_array,
  napi_uint8_clamped_array,
  napi_int16_array,
  napi_uint16_array,
  napi_int32_array,
  napi_uint32_array,
  napi_float32_array,
  napi_float64_array,
  napi_bigint64_array,
  napi_biguint64_array,
} napi_typedarray_type;

typedef enum {
  napi_default = 0,
  napi_writable = 1 << 0,
  napi_enumerable = 1 << 1,
  napi_configurable = 1 << 2,
  napi_static = 1 << 10,
  napi_default_method = napi_writable | napi_configurable,
  napi_default_jsproperty = napi_writable | napi_enumerable | napi_configurable,
} napi_property_attributes;

typedef napi_value (*napi_callback)(napi_env env, napi_callback_info info);
typedef napi_value (*napi_addon_register_func)(napi_env env, napi_value exports);
typedef void (*napi_finalize)(napi_env env, void* finalize_data, void* finalize_hint);

typedef struct {
  const char* utf8name;
  napi_value name;
  napi_callback method;
  napi_callback getter;
  napi_callback setter;
  napi_value value;
  napi_property_attributes attributes;
  NAPI_RAW_PTR_EXCLUSION void* data;
} napi_property_descriptor;

typedef struct {
  uint32_t major;
  uint32_t minor;
  uint32_t patch;
  const char* release;
} napi_node_version;

typedef struct {
  const char* error_message;
  NAPI_RAW_PTR_EXCLUSION void* engine_reserved;
  uint32_t engine_error_code;
  napi_status error_code;
} napi_extended_error_info;

#define NAPI_AUTO_LENGTH ((size_t)-1)
#define NAPI_VERSION 1

#endif  // XENON_OVERLAY_CHROME_BROWSER_NAPI_JS_NATIVE_API_TYPES_H_
