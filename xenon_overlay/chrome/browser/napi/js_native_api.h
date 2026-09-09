// Copyright 2026 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_NAPI_JS_NATIVE_API_H_
#define XENON_OVERLAY_CHROME_BROWSER_NAPI_JS_NATIVE_API_H_

#include "js_native_api_types.h"

// Scope
NAPI_EXTERN napi_status napi_open_handle_scope(napi_env env, napi_handle_scope* result);
NAPI_EXTERN napi_status napi_close_handle_scope(napi_env env, napi_handle_scope scope);
NAPI_EXTERN napi_status napi_open_escapable_handle_scope(napi_env env, napi_escapable_handle_scope* result);
NAPI_EXTERN napi_status napi_close_escapable_handle_scope(napi_env env, napi_escapable_handle_scope scope);
NAPI_EXTERN napi_status napi_escape_handle(napi_env env, napi_escapable_handle_scope scope, napi_value escapee, napi_value* result);

// Type Creation
NAPI_EXTERN napi_status napi_get_undefined(napi_env env, napi_value* result);
NAPI_EXTERN napi_status napi_get_null(napi_env env, napi_value* result);
NAPI_EXTERN napi_status napi_get_global(napi_env env, napi_value* result);
NAPI_EXTERN napi_status napi_get_boolean(napi_env env, bool value, napi_value* result);
NAPI_EXTERN napi_status napi_create_bigint_int64(napi_env env,
                                                 int64_t value,
                                                 napi_value* result);
NAPI_EXTERN napi_status napi_create_bigint_uint64(napi_env env, uint64_t value, napi_value* result);
NAPI_EXTERN napi_status napi_create_double(napi_env env, double value, napi_value* result);
NAPI_EXTERN napi_status napi_create_int32(napi_env env, int32_t value, napi_value* result);
NAPI_EXTERN napi_status napi_create_uint32(napi_env env, uint32_t value, napi_value* result);
NAPI_EXTERN napi_status napi_create_int64(napi_env env, int64_t value, napi_value* result);
NAPI_EXTERN napi_status napi_create_string_latin1(napi_env env,
                                                  const char* str,
                                                  size_t length,
                                                  napi_value* result);
NAPI_EXTERN napi_status napi_create_string_utf8(napi_env env, const char* str, size_t length, napi_value* result);
NAPI_EXTERN napi_status napi_create_string_utf16(napi_env env,
                                                 const char16_t* str,
                                                 size_t length,
                                                 napi_value* result);
NAPI_EXTERN napi_status napi_create_symbol(napi_env env, napi_value description, napi_value* result);
NAPI_EXTERN napi_status napi_create_object(napi_env env, napi_value* result);
NAPI_EXTERN napi_status napi_create_array(napi_env env, napi_value* result);
NAPI_EXTERN napi_status napi_create_array_with_length(napi_env env, size_t length, napi_value* result);
NAPI_EXTERN napi_status napi_create_arraybuffer(napi_env env, size_t byte_length, void** data, napi_value* result);
NAPI_EXTERN napi_status napi_create_typedarray(napi_env env,
                                               napi_typedarray_type type,
                                               size_t length,
                                               napi_value arraybuffer,
                                               size_t byte_offset,
                                               napi_value* result);
NAPI_EXTERN napi_status napi_create_dataview(napi_env env,
                                             size_t length,
                                             napi_value arraybuffer,
                                             size_t byte_offset,
                                             napi_value* result);
NAPI_EXTERN napi_status napi_create_date(napi_env env,
                                         double time,
                                         napi_value* result);
NAPI_EXTERN napi_status napi_create_promise(napi_env env, napi_deferred* deferred, napi_value* promise);
NAPI_EXTERN napi_status napi_resolve_deferred(napi_env env, napi_deferred deferred, napi_value resolution);
NAPI_EXTERN napi_status napi_reject_deferred(napi_env env, napi_deferred deferred, napi_value rejection);

// Type Checking
NAPI_EXTERN napi_status napi_typeof(napi_env env, napi_value value, napi_valuetype* result);
NAPI_EXTERN napi_status napi_coerce_to_bool(napi_env env, napi_value value, napi_value* result);
NAPI_EXTERN napi_status napi_coerce_to_number(napi_env env, napi_value value, napi_value* result);
NAPI_EXTERN napi_status napi_coerce_to_object(napi_env env, napi_value value, napi_value* result);
NAPI_EXTERN napi_status napi_coerce_to_string(napi_env env, napi_value value, napi_value* result);
NAPI_EXTERN napi_status napi_is_error(napi_env env,
                                      napi_value value,
                                      bool* result);
NAPI_EXTERN napi_status napi_is_promise(napi_env env,
                                        napi_value value,
                                        bool* result);
NAPI_EXTERN napi_status napi_is_date(napi_env env,
                                     napi_value value,
                                     bool* result);
NAPI_EXTERN napi_status napi_is_typedarray(napi_env env,
                                           napi_value value,
                                           bool* result);
NAPI_EXTERN napi_status napi_is_dataview(napi_env env,
                                         napi_value value,
                                         bool* result);

// Get Value From N-API representation
NAPI_EXTERN napi_status napi_get_value_double(napi_env env, napi_value value, double* result);
NAPI_EXTERN napi_status napi_get_value_int32(napi_env env, napi_value value, int32_t* result);
NAPI_EXTERN napi_status napi_get_value_uint32(napi_env env, napi_value value, uint32_t* result);
NAPI_EXTERN napi_status napi_get_value_int64(napi_env env, napi_value value, int64_t* result);
NAPI_EXTERN napi_status napi_get_value_bool(napi_env env, napi_value value, bool* result);
NAPI_EXTERN napi_status napi_get_value_bigint_int64(napi_env env, napi_value value, int64_t* result, bool* lossless);
NAPI_EXTERN napi_status napi_get_value_bigint_uint64(napi_env env, napi_value value, uint64_t* result, bool* lossless);
NAPI_EXTERN napi_status napi_get_value_string_latin1(napi_env env,
                                                     napi_value value,
                                                     char* buf,
                                                     size_t bufsize,
                                                     size_t* result);
NAPI_EXTERN napi_status napi_get_value_string_utf8(napi_env env, napi_value value, char* buf, size_t bufsize, size_t* result);
NAPI_EXTERN napi_status napi_get_value_string_utf16(napi_env env,
                                                    napi_value value,
                                                    char16_t* buf,
                                                    size_t bufsize,
                                                    size_t* result);
NAPI_EXTERN napi_status napi_get_date_value(napi_env env,
                                            napi_value value,
                                            double* result);

// Object Properties
NAPI_EXTERN napi_status napi_get_property_names(napi_env env, napi_value object, napi_value* result);
NAPI_EXTERN napi_status napi_set_property(napi_env env, napi_value object, napi_value key, napi_value value);
NAPI_EXTERN napi_status napi_has_property(napi_env env, napi_value object, napi_value key, bool* result);
NAPI_EXTERN napi_status napi_get_property(napi_env env, napi_value object, napi_value key, napi_value* result);
NAPI_EXTERN napi_status napi_delete_property(napi_env env, napi_value object, napi_value key, bool* result);
NAPI_EXTERN napi_status napi_has_own_property(napi_env env, napi_value object, napi_value key, bool* result);
NAPI_EXTERN napi_status napi_set_named_property(napi_env env, napi_value object, const char* utf8name, napi_value value);
NAPI_EXTERN napi_status napi_has_named_property(napi_env env, napi_value object, const char* utf8name, bool* result);
NAPI_EXTERN napi_status napi_get_named_property(napi_env env, napi_value object, const char* utf8name, napi_value* result);
NAPI_EXTERN napi_status napi_set_element(napi_env env, napi_value object, uint32_t index, napi_value value);
NAPI_EXTERN napi_status napi_has_element(napi_env env, napi_value object, uint32_t index, bool* result);
NAPI_EXTERN napi_status napi_get_element(napi_env env, napi_value object, uint32_t index, napi_value* result);
NAPI_EXTERN napi_status napi_delete_element(napi_env env, napi_value object, uint32_t index, bool* result);
NAPI_EXTERN napi_status napi_define_properties(napi_env env, napi_value object, size_t property_count, const napi_property_descriptor* properties);
NAPI_EXTERN napi_status napi_get_prototype(napi_env env,
                                           napi_value object,
                                           napi_value* result);

// Array Methods
NAPI_EXTERN napi_status napi_is_array(napi_env env, napi_value value, bool* result);
NAPI_EXTERN napi_status napi_get_array_length(napi_env env, napi_value value, uint32_t* result);
NAPI_EXTERN napi_status napi_is_arraybuffer(napi_env env, napi_value value, bool* result);
NAPI_EXTERN napi_status napi_get_arraybuffer_info(napi_env env, napi_value arraybuffer, void** data, size_t* byte_length);
NAPI_EXTERN napi_status napi_get_typedarray_info(napi_env env,
                                                 napi_value typedarray,
                                                 napi_typedarray_type* type,
                                                 size_t* length,
                                                 void** data,
                                                 napi_value* arraybuffer,
                                                 size_t* byte_offset);
NAPI_EXTERN napi_status napi_get_dataview_info(napi_env env,
                                               napi_value dataview,
                                               size_t* byte_length,
                                               void** data,
                                               napi_value* arraybuffer,
                                               size_t* byte_offset);
NAPI_EXTERN napi_status napi_get_version(napi_env env, uint32_t* result);

// Functions
NAPI_EXTERN napi_status napi_create_function(napi_env env, const char* utf8name, size_t length, napi_callback cb, void* data, napi_value* result);
NAPI_EXTERN napi_status napi_call_function(napi_env env, napi_value recv, napi_value func, size_t argc, const napi_value* argv, napi_value* result);
NAPI_EXTERN napi_status napi_strict_equals(napi_env env,
                                           napi_value lhs,
                                           napi_value rhs,
                                           bool* result);
NAPI_EXTERN napi_status napi_instanceof(napi_env env,
                                        napi_value object,
                                        napi_value constructor,
                                        bool* result);
NAPI_EXTERN napi_status napi_get_cb_info(napi_env env, napi_callback_info cbinfo, size_t* argc, napi_value* argv, napi_value* this_arg, void** data);
NAPI_EXTERN napi_status napi_get_new_target(napi_env env, napi_callback_info cbinfo, napi_value* result);
NAPI_EXTERN napi_status napi_new_instance(napi_env env, napi_value constructor, size_t argc, const napi_value* argv, napi_value* result);

// References
NAPI_EXTERN napi_status napi_create_reference(napi_env env, napi_value value, uint32_t initial_refcount, napi_ref* result);
NAPI_EXTERN napi_status napi_delete_reference(napi_env env, napi_ref ref);
NAPI_EXTERN napi_status napi_reference_ref(napi_env env, napi_ref ref, uint32_t* result);
NAPI_EXTERN napi_status napi_reference_unref(napi_env env, napi_ref ref, uint32_t* result);
NAPI_EXTERN napi_status napi_get_reference_value(napi_env env, napi_ref ref, napi_value* result);

// Error handling
NAPI_EXTERN napi_status napi_create_error(napi_env env,
                                          napi_value code,
                                          napi_value msg,
                                          napi_value* result);
NAPI_EXTERN napi_status napi_create_type_error(napi_env env,
                                               napi_value code,
                                               napi_value msg,
                                               napi_value* result);
NAPI_EXTERN napi_status napi_create_range_error(napi_env env,
                                                napi_value code,
                                                napi_value msg,
                                                napi_value* result);
NAPI_EXTERN napi_status napi_throw(napi_env env, napi_value error);
NAPI_EXTERN napi_status napi_throw_error(napi_env env, const char* code, const char* msg);
NAPI_EXTERN napi_status napi_throw_type_error(napi_env env, const char* code, const char* msg);
NAPI_EXTERN napi_status napi_throw_range_error(napi_env env, const char* code, const char* msg);
NAPI_EXTERN napi_status napi_get_and_clear_last_exception(napi_env env, napi_value* result);
NAPI_EXTERN napi_status napi_is_exception_pending(napi_env env, bool* result);
NAPI_EXTERN napi_status napi_fatal_error(const char* location, size_t location_len, const char* message, size_t message_len);

// Class Creation
NAPI_EXTERN napi_status napi_define_class(napi_env env, const char* utf8name, size_t length, napi_callback constructor, void* data, size_t property_count, const napi_property_descriptor* properties, napi_value* result);

// Wrap / Unwrap
NAPI_EXTERN napi_status napi_wrap(napi_env env, napi_value js_object, void* native_object, napi_finalize finalize_cb, void* finalize_hint, napi_ref* result);
NAPI_EXTERN napi_status napi_unwrap(napi_env env, napi_value js_object, void** result);
NAPI_EXTERN napi_status napi_remove_wrap(napi_env env, napi_value js_object, void** result);
NAPI_EXTERN napi_status napi_add_finalizer(napi_env env,
                                           napi_value js_object,
                                           void* native_object,
                                           napi_finalize finalize_cb,
                                           void* finalize_hint,
                                           napi_ref* result);

// Environment data and diagnostics
NAPI_EXTERN napi_status napi_set_instance_data(napi_env env,
                                               void* data,
                                               napi_finalize finalize_cb,
                                               void* finalize_hint);
NAPI_EXTERN napi_status napi_get_instance_data(napi_env env, void** data);
NAPI_EXTERN napi_status napi_get_last_error_info(
    napi_env env,
    const napi_extended_error_info** result);

// External values
NAPI_EXTERN napi_status napi_create_external(napi_env env, void* data, napi_finalize finalize_cb, void* finalize_hint, napi_value* result);
NAPI_EXTERN napi_status napi_get_value_external(napi_env env, napi_value value, void** result);

// Execution
NAPI_EXTERN napi_status napi_run_script(napi_env env, napi_value script, napi_value* result);
NAPI_EXTERN napi_status napi_adjust_external_memory(napi_env env,
                                                    int64_t change_in_bytes,
                                                    int64_t* adjusted_value);

#endif  // XENON_OVERLAY_CHROME_BROWSER_NAPI_JS_NATIVE_API_H_
