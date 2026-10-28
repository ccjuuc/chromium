// Copyright 2026 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "napi_loader.h"

#include <optional>
#include <vector>

#include "base/base_paths.h"
#include "base/compiler_specific.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/logging.h"
#include "base/memory/raw_ptr.h"
#include "base/path_service.h"
#include "base/test/scoped_command_line.h"
#include "base/test/task_environment.h"
#include "build/build_config.h"
#include "js_native_api_v8.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "v8/include/libplatform/libplatform.h"
#include "v8/include/v8.h"
#include "xenon_overlay/buildflags/buildflags.h"
#include "xenon_overlay/chrome/browser/napi/napi_switches.h"

#if BUILDFLAG(IS_MAC) && BUILDFLAG(ENABLE_XENON_NODE_UV_COMPAT)
#include <dlfcn.h>

#include "xenon_overlay/third_party/libuv/include/uv.h"
#endif

namespace xenon {

namespace {

napi_value CreateTemporaryValues(napi_env env, napi_callback_info info) {
  napi_value unused;
  napi_value result;
  EXPECT_EQ(napi_create_int32(env, 1, &unused), napi_ok);
  EXPECT_EQ(napi_create_int32(env, 2, &result), napi_ok);
  return result;
}

napi_value ThrowNativeArgument(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argument = nullptr;
  EXPECT_EQ(napi_get_cb_info(env, info, &argc, &argument, nullptr, nullptr),
            napi_ok);
  EXPECT_EQ(napi_throw(env, argument), napi_ok);
  return nullptr;
}

struct ConstructorExceptionCapture {
  bool saw_pending_exception = false;
  bool removed_wrap = false;
  napi_ref reference = nullptr;
};

napi_value ConstructAfterNativeException(napi_env env,
                                         napi_callback_info info) {
  napi_value receiver = nullptr;
  void* data = nullptr;
  EXPECT_EQ(napi_get_cb_info(env, info, nullptr, nullptr, &receiver, &data),
            napi_ok);
  auto* capture = static_cast<ConstructorExceptionCapture*>(data);
  EXPECT_EQ(
      napi_wrap(env, receiver, capture, nullptr, nullptr, &capture->reference),
      napi_ok);
  EXPECT_EQ(napi_reference_ref(env, capture->reference, nullptr), napi_ok);
  EXPECT_EQ(napi_is_exception_pending(env, &capture->saw_pending_exception),
            napi_ok);
  // node-addon-api's ObjectWrap constructor wrapper performs this check after
  // constructing T. A stale exception destroys a valid new native instance.
  if (capture->saw_pending_exception) {
    napi_value exception = nullptr;
    EXPECT_EQ(napi_get_and_clear_last_exception(env, &exception), napi_ok);
    EXPECT_EQ(napi_remove_wrap(env, receiver, nullptr), napi_ok);
    EXPECT_EQ(napi_delete_reference(env, capture->reference), napi_ok);
    capture->reference = nullptr;
    capture->removed_wrap = true;
    EXPECT_EQ(napi_throw(env, exception), napi_ok);
  }
  return receiver;
}

napi_value ReadThrowingNativeProperty(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value object = nullptr;
  void* data = nullptr;
  EXPECT_EQ(napi_get_cb_info(env, info, &argc, &object, nullptr, &data),
            napi_ok);
  napi_value result = nullptr;
  EXPECT_EQ(napi_get_named_property(env, object, "value", &result),
            napi_pending_exception);
  if (*static_cast<bool*>(data)) {
    EXPECT_EQ(napi_get_and_clear_last_exception(env, &result), napi_ok);
  }
  return result;
}

struct MissingArgumentCapture {
  napi_status get_info_status = napi_generic_failure;
  size_t actual_argc = 0;
  bool value_is_live = false;
  napi_status type_status = napi_generic_failure;
  napi_valuetype value_type = napi_object;
  napi_status reference_status = napi_generic_failure;
};

napi_value CaptureMissingArgument(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  void* data = nullptr;
  napi_status status =
      napi_get_cb_info(env, info, &argc, argv, nullptr, &data);
  auto* capture = static_cast<MissingArgumentCapture*>(data);
  if (!capture) {
    return nullptr;
  }

  capture->get_info_status = status;
  capture->actual_argc = argc;
  capture->value_is_live = env->IsLiveValue(argv[0]);
  if (!capture->value_is_live) {
    return nullptr;
  }

  capture->type_status = napi_typeof(env, argv[0], &capture->value_type);
  napi_ref reference = nullptr;
  capture->reference_status =
      napi_create_reference(env, argv[0], 1, &reference);
  if (reference) {
    EXPECT_EQ(napi_delete_reference(env, reference), napi_ok);
  }
  return nullptr;
}

void MarkFinalized(napi_env env, void* data, void* hint) {
  *static_cast<bool*>(data) = true;
}

struct CleanupCall {
  NAPI_RAW_PTR_EXCLUSION std::vector<int>* calls;
  int value;
};

void RecordCleanupCall(void* data) {
  auto* call = static_cast<CleanupCall*>(data);
  call->calls->push_back(call->value);
}

void RecordCleanupFinalizer(napi_env, void* data, void*) {
  RecordCleanupCall(data);
}

struct CleanupMutation {
  NAPI_RAW_PTR_EXCLUSION napi_env env;
  NAPI_RAW_PTR_EXCLUSION CleanupCall* current;
  NAPI_RAW_PTR_EXCLUSION CleanupCall* removed;
  NAPI_RAW_PTR_EXCLUSION CleanupCall* added_first;
  NAPI_RAW_PTR_EXCLUSION CleanupCall* added_last;
};

void MutateCleanupHooks(void* data) {
  auto* mutation = static_cast<CleanupMutation*>(data);
  RecordCleanupCall(mutation->current);
  EXPECT_EQ(napi_remove_env_cleanup_hook(mutation->env, RecordCleanupCall,
                                        mutation->removed),
            napi_ok);
  EXPECT_EQ(napi_add_env_cleanup_hook(mutation->env, RecordCleanupCall,
                                     mutation->added_first),
            napi_ok);
  EXPECT_EQ(napi_add_env_cleanup_hook(mutation->env, RecordCleanupCall,
                                     mutation->added_last),
            napi_ok);
}

void CountThreadsafeCall(napi_env env,
                         napi_value callback,
                         void* context,
                         void* data) {
  ++*static_cast<int*>(context);
}

napi_value EvaluateNapiFixture(napi_env env, const char* source) {
  return env->CreateValue(
      v8::Script::Compile(
          env->GetContext(),
          v8::String::NewFromUtf8(env->isolate, source).ToLocalChecked())
          .ToLocalChecked()
          ->Run(env->GetContext())
          .ToLocalChecked());
}

// The real native lifetime monitor uses this standard callback boundary: the
// async receiver is an object, while the user freer retains a null `this`.
napi_value DispatchAsyncFreer(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value args[2];
  if (napi_get_cb_info(env, info, &argc, args, nullptr, nullptr) != napi_ok ||
      argc != 2) {
    return nullptr;
  }
  napi_value receiver;
  napi_get_null(env, &receiver);
  napi_value result = nullptr;
  return napi_call_function(env, receiver, args[0], 1, &args[1], &result) ==
                 napi_ok
             ? result
             : nullptr;
}

void ReportFatalAsyncMessage(v8::Local<v8::Message>, v8::Local<v8::Value>) {
  LOG(ERROR) << "NAPI_V8_UNCAUGHT_REPORT";
}

}  // namespace

TEST(NapiFatalErrorDeathTest, PreservesAutoLengthDiagnostic) {
  EXPECT_DEATH_IF_SUPPORTED(
      napi_fatal_error("native-location", NAPI_AUTO_LENGTH, "native-failure",
                       NAPI_AUTO_LENGTH),
      "Fatal error in Node addon: native-failure at native-location");
}

TEST(NapiFatalErrorDeathTest, SupportsExplicitLengthsAndNullLocation) {
  const char message[] = {'b', 'a', 'd', 'x'};
  EXPECT_DEATH_IF_SUPPORTED(napi_fatal_error(nullptr, 0, message, 3),
                            "Fatal error in Node addon: bad at ");
  EXPECT_DEATH_IF_SUPPORTED(napi_fatal_error(nullptr, 0, nullptr, 0),
                            "Fatal error in Node addon:  at ");
}

class NapiLoaderTest : public testing::Test {
 protected:
  static void SetUpTestSuite() {
    base::FilePath exe_path;
    base::PathService::Get(base::FILE_EXE, &exe_path);
    std::string exe_path_str = exe_path.AsUTF8Unsafe();
    v8::V8::InitializeICUDefaultLocation(exe_path_str.c_str());
    v8::V8::InitializeExternalStartupData(exe_path_str.c_str());
    platform_ = v8::platform::NewDefaultPlatform().release();
    v8::V8::InitializePlatform(platform_);
    v8::V8::Initialize();
  }

  static void TearDownTestSuite() {
    v8::V8::Dispose();
    v8::V8::DisposePlatform();
    delete platform_;
    platform_ = nullptr;
  }

  void SetUp() override {
    v8::Isolate::CreateParams create_params;
    create_params.array_buffer_allocator =
        v8::ArrayBuffer::Allocator::NewDefaultAllocator();
    allocator_ = create_params.array_buffer_allocator;
    isolate_ = v8::Isolate::New(create_params);
    v8::Locker locker(isolate_);
    v8::Isolate::Scope isolate_scope(isolate_);
    v8::HandleScope handle_scope(isolate_);
    context_.Reset(isolate_, v8::Context::New(isolate_));
  }

  void TearDown() override {
    {
      v8::Locker locker(isolate_);
      v8::Isolate::Scope isolate_scope(isolate_);
      context_.Reset();
    }
    isolate_->Dispose();
    delete allocator_;
  }

  v8::Isolate* isolate() { return isolate_; }
  v8::Local<v8::Context> context() { return context_.Get(isolate_); }

 protected:
  base::test::TaskEnvironment task_environment_{
      base::test::TaskEnvironment::TimeSource::MOCK_TIME};

 private:
  static v8::Platform* platform_;
  NAPI_RAW_PTR_EXCLUSION v8::Isolate* isolate_ = nullptr;
  NAPI_RAW_PTR_EXCLUSION v8::ArrayBuffer::Allocator* allocator_ = nullptr;
  v8::Global<v8::Context> context_;
};

v8::Platform* NapiLoaderTest::platform_ = nullptr;

TEST_F(NapiLoaderTest, NamedPropertyPreservesGetterExceptionAndErrorStatus) {
  v8::Locker locker(isolate());
  v8::Isolate::Scope isolate_scope(isolate());
  v8::HandleScope handle_scope(isolate());
  v8::Context::Scope context_scope(context());
  auto env = std::make_unique<napi_env__>(isolate(), context());
  v8::Local<v8::Script> script;
  ASSERT_TRUE(v8::Script::Compile(
                  context(), v8::String::NewFromUtf8Literal(isolate(), R"JS(
                    globalThis.originalError = new Error('native getter failure');
                    globalThis.getterCalls = 0;
                    ({ get dangerous() { ++getterCalls; throw originalError; },
                       safe: 42 })
                  )JS"))
                  .ToLocal(&script));
  v8::Local<v8::Value> object;
  ASSERT_TRUE(script->Run(context()).ToLocal(&object));
  v8::Local<v8::Value> original_error;
  ASSERT_TRUE(context()
                  ->Global()
                  ->Get(context(), v8::String::NewFromUtf8Literal(
                                       isolate(), "originalError"))
                  .ToLocal(&original_error));
  napi_value napi_object = env->CreateValue(object);
  napi_value result = nullptr;
  v8::TryCatch outer_catch(isolate());
  EXPECT_EQ(
      napi_get_named_property(env.get(), napi_object, "dangerous", &result),
      napi_pending_exception);
  EXPECT_EQ(result, nullptr);
  EXPECT_FALSE(outer_catch.HasCaught());

  const napi_extended_error_info* error_info = nullptr;
  ASSERT_EQ(napi_get_last_error_info(env.get(), &error_info), napi_ok);
  ASSERT_NE(error_info, nullptr);
  EXPECT_EQ(error_info->error_code, napi_pending_exception);
  EXPECT_STREQ(error_info->error_message, "An exception is pending");
  EXPECT_EQ(error_info->engine_reserved, nullptr);
  EXPECT_EQ(error_info->engine_error_code, 0u);
  bool pending = false;
  ASSERT_EQ(napi_is_exception_pending(env.get(), &pending), napi_ok);
  EXPECT_TRUE(pending);
  napi_value exception = nullptr;
  ASSERT_EQ(napi_get_and_clear_last_exception(env.get(), &exception), napi_ok);
  ASSERT_NE(exception, nullptr);
  EXPECT_TRUE(exception->Get()->StrictEquals(original_error));
  ASSERT_EQ(napi_is_exception_pending(env.get(), &pending), napi_ok);
  EXPECT_FALSE(pending);
  ASSERT_EQ(napi_get_and_clear_last_exception(env.get(), &exception), napi_ok);
  EXPECT_TRUE(exception->Get()->IsUndefined());

  ASSERT_EQ(napi_get_named_property(env.get(), napi_object, "safe", &result),
            napi_ok);
  EXPECT_EQ(result->Get().As<v8::Int32>()->Value(), 42);
  ASSERT_EQ(napi_get_last_error_info(env.get(), &error_info), napi_ok);
  EXPECT_EQ(error_info->error_code, napi_ok);
  EXPECT_EQ(error_info->error_message, nullptr);
  v8::Local<v8::Value> getter_calls;
  ASSERT_TRUE(context()
                  ->Global()
                  ->Get(context(), v8::String::NewFromUtf8Literal(
                                       isolate(), "getterCalls"))
                  .ToLocal(&getter_calls));
  EXPECT_EQ(getter_calls.As<v8::Int32>()->Value(), 1);
}

TEST_F(NapiLoaderTest, NamedPropertyRecordsInvalidArgumentsAndClearsOnSuccess) {
  v8::Locker locker(isolate());
  v8::Isolate::Scope isolate_scope(isolate());
  v8::HandleScope handle_scope(isolate());
  v8::Context::Scope context_scope(context());
  auto env = std::make_unique<napi_env__>(isolate(), context());
  napi_value result = nullptr;
  EXPECT_EQ(napi_get_named_property(env.get(), nullptr, "missing", &result),
            napi_invalid_arg);
  const napi_extended_error_info* error_info = nullptr;
  ASSERT_EQ(napi_get_last_error_info(env.get(), &error_info), napi_ok);
  EXPECT_EQ(error_info->error_code, napi_invalid_arg);
  EXPECT_STREQ(error_info->error_message, "Invalid argument");
  bool pending = true;
  EXPECT_EQ(napi_is_exception_pending(env.get(), &pending), napi_ok);
  EXPECT_FALSE(pending);

  napi_value object = env->CreateValue(v8::Object::New(isolate()));
  EXPECT_EQ(napi_get_named_property(env.get(), object, nullptr, &result),
            napi_invalid_arg);
  EXPECT_EQ(napi_get_named_property(env.get(), object, "missing", nullptr),
            napi_invalid_arg);
  EXPECT_EQ(napi_get_named_property(env.get(), object, "missing", &result),
            napi_ok);
  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(result->Get()->IsUndefined());
  ASSERT_EQ(napi_get_last_error_info(env.get(), &error_info), napi_ok);
  EXPECT_EQ(error_info->error_code, napi_ok);
  EXPECT_EQ(error_info->error_message, nullptr);
}

TEST_F(NapiLoaderTest,
       CaughtNativeThrowDoesNotInvalidateNextWrappedConstructor) {
  v8::Locker locker(isolate());
  v8::Isolate::Scope isolate_scope(isolate());
  v8::HandleScope handle_scope(isolate());
  v8::Context::Scope context_scope(context());
  auto env = std::make_unique<napi_env__>(isolate(), context());
  napi_value thrower = nullptr;
  ASSERT_EQ(napi_create_function(env.get(), "throwNative", NAPI_AUTO_LENGTH,
                                 ThrowNativeArgument, nullptr, &thrower),
            napi_ok);
  ConstructorExceptionCapture capture;
  napi_value constructor = nullptr;
  ASSERT_EQ(napi_define_class(env.get(), "Wrapped", NAPI_AUTO_LENGTH,
                              ConstructAfterNativeException, &capture, 0,
                              nullptr, &constructor),
            napi_ok);
  ASSERT_TRUE(
      context()
          ->Global()
          ->Set(context(),
                v8::String::NewFromUtf8Literal(isolate(), "throwNative"),
                thrower->Get())
          .FromJust());
  ASSERT_TRUE(context()
                  ->Global()
                  ->Set(context(),
                        v8::String::NewFromUtf8Literal(isolate(), "Wrapped"),
                        constructor->Get())
                  .FromJust());
  v8::Local<v8::Script> script;
  ASSERT_TRUE(v8::Script::Compile(
                  context(), v8::String::NewFromUtf8Literal(isolate(), R"JS(
                    const error = new Error('handled native failure');
                    let caught;
                    try { throwNative(error); } catch (e) { caught = e; }
                    if (caught !== error) throw new Error('original error lost');
                    new Wrapped();
                  )JS"))
                  .ToLocal(&script));
  v8::TryCatch try_catch(isolate());
  v8::Local<v8::Value> instance;
  EXPECT_TRUE(script->Run(context()).ToLocal(&instance));
  EXPECT_FALSE(try_catch.HasCaught());
  EXPECT_FALSE(capture.saw_pending_exception);
  EXPECT_FALSE(capture.removed_wrap);
  ASSERT_NE(capture.reference, nullptr);
  napi_value referenced = nullptr;
  ASSERT_EQ(napi_get_reference_value(env.get(), capture.reference, &referenced),
            napi_ok);
  ASSERT_NE(referenced, nullptr);
  EXPECT_TRUE(referenced->Get()->StrictEquals(instance));
  EXPECT_EQ(napi_remove_wrap(env.get(), referenced, nullptr), napi_ok);
  EXPECT_EQ(napi_delete_reference(env.get(), capture.reference), napi_ok);
}

TEST_F(NapiLoaderTest, CallbackBoundaryPropagatesUnhandledGetterException) {
  v8::Locker locker(isolate());
  v8::Isolate::Scope isolate_scope(isolate());
  v8::HandleScope handle_scope(isolate());
  v8::Context::Scope context_scope(context());
  auto env = std::make_unique<napi_env__>(isolate(), context());
  bool clear_exception = false;
  napi_value reader = nullptr;
  ASSERT_EQ(napi_create_function(env.get(), "readNative", NAPI_AUTO_LENGTH,
                                 ReadThrowingNativeProperty, &clear_exception,
                                 &reader),
            napi_ok);
  v8::Local<v8::Value> original_error = v8::Exception::Error(
      v8::String::NewFromUtf8Literal(isolate(), "original getter error"));
  v8::Local<v8::Object> object = v8::Object::New(isolate());
  v8::Local<v8::Function> getter;
  ASSERT_TRUE(v8::Function::New(
                  context(),
                  [](const v8::FunctionCallbackInfo<v8::Value>& info) {
                    info.GetIsolate()->ThrowException(info.Data());
                  },
                  original_error)
                  .ToLocal(&getter));
  object->SetAccessorProperty(
      v8::String::NewFromUtf8Literal(isolate(), "value"), getter);
  v8::Local<v8::Value> arguments[] = {object};
  v8::TryCatch try_catch(isolate());
  EXPECT_TRUE(reader->Get()
                  .As<v8::Function>()
                  ->Call(context(), context()->Global(), 1, arguments)
                  .IsEmpty());
  ASSERT_TRUE(try_catch.HasCaught());
  EXPECT_TRUE(try_catch.Exception()->StrictEquals(original_error));
  EXPECT_TRUE(env->last_exception.IsEmpty());
}

TEST_F(NapiLoaderTest, CallbackBoundaryHonorsExplicitExceptionHandling) {
  v8::Locker locker(isolate());
  v8::Isolate::Scope isolate_scope(isolate());
  v8::HandleScope handle_scope(isolate());
  v8::Context::Scope context_scope(context());
  auto env = std::make_unique<napi_env__>(isolate(), context());
  bool clear_exception = true;
  napi_value reader = nullptr;
  ASSERT_EQ(napi_create_function(env.get(), "readNative", NAPI_AUTO_LENGTH,
                                 ReadThrowingNativeProperty, &clear_exception,
                                 &reader),
            napi_ok);
  v8::Local<v8::Value> original_error = v8::Exception::Error(
      v8::String::NewFromUtf8Literal(isolate(), "handled getter error"));
  v8::Local<v8::Object> object = v8::Object::New(isolate());
  v8::Local<v8::Function> getter;
  ASSERT_TRUE(v8::Function::New(
                  context(),
                  [](const v8::FunctionCallbackInfo<v8::Value>& info) {
                    info.GetIsolate()->ThrowException(info.Data());
                  },
                  original_error)
                  .ToLocal(&getter));
  object->SetAccessorProperty(
      v8::String::NewFromUtf8Literal(isolate(), "value"), getter);
  v8::Local<v8::Value> arguments[] = {object};
  v8::TryCatch try_catch(isolate());
  v8::Local<v8::Value> result;
  ASSERT_TRUE(reader->Get()
                  .As<v8::Function>()
                  ->Call(context(), context()->Global(), 1, arguments)
                  .ToLocal(&result));
  EXPECT_TRUE(result->StrictEquals(original_error));
  EXPECT_FALSE(try_catch.HasCaught());
  EXPECT_TRUE(env->last_exception.IsEmpty());
}

TEST_F(NapiLoaderTest, InstanceOfAcceptsPrimitiveValues) {
  v8::Locker locker(isolate());
  v8::Isolate::Scope isolate_scope(isolate());
  v8::HandleScope handle_scope(isolate());
  v8::Context::Scope context_scope(context());
  auto env = std::make_unique<napi_env__>(isolate(), context());
  v8::Local<v8::Value> constructor;
  ASSERT_TRUE(
      context()
          ->Global()
          ->Get(context(), v8::String::NewFromUtf8Literal(isolate(), "RegExp"))
          .ToLocal(&constructor));
  napi_value napi_constructor = env->CreateValue(constructor);
  v8::Local<v8::Value> primitives[] = {
      v8::Undefined(isolate()),
      v8::Null(isolate()),
      v8::Boolean::New(isolate(), true),
      v8::Number::New(isolate(), 42),
      v8::BigInt::New(isolate(), 42),
      v8::String::NewFromUtf8Literal(isolate(), "text"),
      v8::Symbol::New(isolate())};
  for (v8::Local<v8::Value> value : primitives) {
    bool result = true;
    EXPECT_EQ(napi_instanceof(env.get(), env->CreateValue(value),
                              napi_constructor, &result),
              napi_ok);
    EXPECT_FALSE(result);
  }
  v8::Local<v8::RegExp> regexp;
  ASSERT_TRUE(v8::RegExp::New(context(),
                              v8::String::NewFromUtf8Literal(isolate(), "test"),
                              v8::RegExp::kNone)
                  .ToLocal(&regexp));
  bool result = false;
  ASSERT_EQ(napi_instanceof(env.get(), env->CreateValue(regexp),
                            napi_constructor, &result),
            napi_ok);
  EXPECT_TRUE(result);
  bool pending = true;
  ASSERT_EQ(napi_is_exception_pending(env.get(), &pending), napi_ok);
  EXPECT_FALSE(pending);
}

TEST_F(NapiLoaderTest,
       InstanceOfHonorsCustomHasInstanceAndPreservesExceptions) {
  v8::Locker locker(isolate());
  v8::Isolate::Scope isolate_scope(isolate());
  v8::HandleScope handle_scope(isolate());
  v8::Context::Scope context_scope(context());
  auto env = std::make_unique<napi_env__>(isolate(), context());
  v8::Local<v8::Script> script;
  ASSERT_TRUE(v8::Script::Compile(
                  context(), v8::String::NewFromUtf8Literal(isolate(), R"JS(
        (() => {
          const error = new Error('hasInstance failure');
          function Custom() {}
          Object.defineProperty(Custom, Symbol.hasInstance, {
            value(value) {
              if (value === null) throw error;
              return value === 42;
            }
          });
          return [Custom, error];
        })()
      )JS"))
                  .ToLocal(&script));
  v8::Local<v8::Value> setup;
  ASSERT_TRUE(script->Run(context()).ToLocal(&setup));
  v8::Local<v8::Value> constructor;
  v8::Local<v8::Value> original_error;
  ASSERT_TRUE(setup.As<v8::Array>()->Get(context(), 0).ToLocal(&constructor));
  ASSERT_TRUE(
      setup.As<v8::Array>()->Get(context(), 1).ToLocal(&original_error));
  napi_value napi_constructor = env->CreateValue(constructor);
  bool result = false;
  EXPECT_EQ(napi_instanceof(env.get(),
                            env->CreateValue(v8::Number::New(isolate(), 42)),
                            napi_constructor, &result),
            napi_ok);
  EXPECT_TRUE(result);

  v8::TryCatch outer_catch(isolate());
  EXPECT_EQ(napi_instanceof(env.get(), env->CreateValue(v8::Null(isolate())),
                            napi_constructor, &result),
            napi_pending_exception);
  EXPECT_FALSE(result);
  EXPECT_FALSE(outer_catch.HasCaught());
  const napi_extended_error_info* error_info = nullptr;
  ASSERT_EQ(napi_get_last_error_info(env.get(), &error_info), napi_ok);
  EXPECT_EQ(error_info->error_code, napi_pending_exception);
  bool pending = false;
  ASSERT_EQ(napi_is_exception_pending(env.get(), &pending), napi_ok);
  EXPECT_TRUE(pending);
  napi_value exception = nullptr;
  ASSERT_EQ(napi_get_and_clear_last_exception(env.get(), &exception), napi_ok);
  ASSERT_NE(exception, nullptr);
  EXPECT_TRUE(exception->Get()->StrictEquals(original_error));
  ASSERT_EQ(napi_is_exception_pending(env.get(), &pending), napi_ok);
  EXPECT_FALSE(pending);

  EXPECT_EQ(
      napi_instanceof(env.get(), env->CreateValue(v8::Object::New(isolate())),
                      napi_constructor, &result),
      napi_ok);
  EXPECT_FALSE(result);
  ASSERT_EQ(napi_get_last_error_info(env.get(), &error_info), napi_ok);
  EXPECT_EQ(error_info->error_code, napi_ok);
}

TEST_F(NapiLoaderTest, BasicNapiValueCreation) {
  v8::Locker locker(isolate());
  v8::Isolate::Scope isolate_scope(isolate());
  v8::HandleScope handle_scope(isolate());
  v8::Context::Scope context_scope(context());

  napi_env env = new napi_env__(isolate(), context());

  napi_handle_scope n_scope;
  EXPECT_EQ(napi_open_handle_scope(env, &n_scope), napi_ok);

  napi_value int_val;
  EXPECT_EQ(napi_create_int32(env, 42, &int_val), napi_ok);

  int32_t val_out = 0;
  EXPECT_EQ(napi_get_value_int32(env, int_val, &val_out), napi_ok);
  EXPECT_EQ(val_out, 42);

  EXPECT_EQ(napi_close_handle_scope(env, n_scope), napi_ok);

  delete env;
}

TEST_F(NapiLoaderTest, EscapableHandleSurvivesScopeClose) {
  v8::Locker locker(isolate());
  v8::Isolate::Scope isolate_scope(isolate());
  v8::HandleScope handle_scope(isolate());
  v8::Context::Scope context_scope(context());

  auto env = std::make_unique<napi_env__>(isolate(), context());
  napi_escapable_handle_scope scope;
  ASSERT_EQ(napi_open_escapable_handle_scope(env.get(), &scope), napi_ok);

  napi_value inner_value;
  ASSERT_EQ(napi_create_int32(env.get(), 17, &inner_value), napi_ok);
  napi_value escaped_value;
  ASSERT_EQ(napi_escape_handle(env.get(), scope, inner_value, &escaped_value),
            napi_ok);
  EXPECT_EQ(napi_escape_handle(env.get(), scope, inner_value, &inner_value),
            napi_escape_called_twice);
  ASSERT_EQ(napi_close_escapable_handle_scope(env.get(), scope), napi_ok);

  ASSERT_EQ(env->allocated_values.size(), 1u);
  EXPECT_EQ(env->allocated_values.front().get(), escaped_value);
  int32_t result = 0;
  EXPECT_EQ(napi_get_value_int32(env.get(), escaped_value, &result), napi_ok);
  EXPECT_EQ(result, 17);
}

TEST_F(NapiLoaderTest, CallbackTemporaryValuesAreReleased) {
  v8::Locker locker(isolate());
  v8::Isolate::Scope isolate_scope(isolate());
  v8::HandleScope handle_scope(isolate());
  v8::Context::Scope context_scope(context());

  auto env = std::make_unique<napi_env__>(isolate(), context());
  napi_value function;
  ASSERT_EQ(napi_create_function(env.get(), "temporary", NAPI_AUTO_LENGTH,
                                 &CreateTemporaryValues, nullptr, &function),
            napi_ok);
  const size_t value_count = env->allocated_values.size();

  v8::Local<v8::Value> result;
  ASSERT_TRUE(function->Get()
                  .As<v8::Function>()
                  ->Call(context(), v8::Undefined(isolate()), 0, nullptr)
                  .ToLocal(&result));
  EXPECT_TRUE(result->IsInt32());
  EXPECT_EQ(result.As<v8::Int32>()->Value(), 2);
  EXPECT_EQ(env->allocated_values.size(), value_count);
}

TEST_F(NapiLoaderTest, CallbackMissingArgumentsAreUndefined) {
  v8::Locker locker(isolate());
  v8::Isolate::Scope isolate_scope(isolate());
  v8::HandleScope handle_scope(isolate());
  v8::Context::Scope context_scope(context());

  auto env = std::make_unique<napi_env__>(isolate(), context());
  MissingArgumentCapture capture;
  napi_value function;
  ASSERT_EQ(napi_create_function(env.get(), "missing", NAPI_AUTO_LENGTH,
                                 &CaptureMissingArgument, &capture, &function),
            napi_ok);
  const size_t value_count = env->allocated_values.size();

  v8::Local<v8::Value> result;
  ASSERT_TRUE(function->Get()
                  .As<v8::Function>()
                  ->Call(context(), v8::Undefined(isolate()), 0, nullptr)
                  .ToLocal(&result));
  EXPECT_TRUE(result->IsUndefined());
  EXPECT_EQ(capture.get_info_status, napi_ok);
  EXPECT_EQ(capture.actual_argc, 0u);
  EXPECT_TRUE(capture.value_is_live);
  EXPECT_EQ(capture.type_status, napi_ok);
  EXPECT_EQ(capture.value_type, napi_undefined);
  EXPECT_EQ(capture.reference_status, napi_invalid_arg);
  EXPECT_EQ(env->allocated_values.size(), value_count);
}

TEST_F(NapiLoaderTest, AsyncContextRetainsResourceUntilDestroyed) {
  v8::Locker locker(isolate());
  v8::Isolate::Scope isolate_scope(isolate());
  v8::HandleScope handle_scope(isolate());
  v8::Context::Scope context_scope(context());
  auto env = std::make_unique<napi_env__>(isolate(), context());
  napi_async_context async_context = nullptr;
  napi_ref weak = nullptr;
  napi_handle_scope scope;
  ASSERT_EQ(napi_open_handle_scope(env.get(), &scope), napi_ok);
  napi_value resource;
  napi_value name;
  ASSERT_EQ(napi_create_object(env.get(), &resource), napi_ok);
  ASSERT_EQ(napi_create_string_utf8(env.get(), "lifetime-monitor",
                                    NAPI_AUTO_LENGTH, &name),
            napi_ok);
  ASSERT_EQ(napi_create_reference(env.get(), resource, 0, &weak), napi_ok);
  ASSERT_EQ(napi_async_init(env.get(), resource, name, &async_context),
            napi_ok);
  ASSERT_EQ(napi_close_handle_scope(env.get(), scope), napi_ok);
  isolate()->LowMemoryNotification();
  ASSERT_EQ(napi_open_handle_scope(env.get(), &scope), napi_ok);
  napi_value retained = nullptr;
  ASSERT_EQ(napi_get_reference_value(env.get(), weak, &retained), napi_ok);
  EXPECT_NE(nullptr, retained);
  ASSERT_EQ(napi_close_handle_scope(env.get(), scope), napi_ok);

  EXPECT_EQ(napi_async_destroy(env.get(), async_context), napi_ok);
  EXPECT_TRUE(env->async_contexts.empty());
  isolate()->LowMemoryNotification();
  EXPECT_EQ(napi_get_reference_value(env.get(), weak, &retained), napi_ok);
  EXPECT_EQ(nullptr, retained);
  EXPECT_EQ(napi_delete_reference(env.get(), weak), napi_ok);
  EXPECT_EQ(napi_async_destroy(env.get(), async_context), napi_invalid_arg);
}

TEST_F(NapiLoaderTest, EnvironmentTeardownReleasesRemainingAsyncResources) {
  v8::Locker locker(isolate());
  v8::Isolate::Scope isolate_scope(isolate());
  v8::HandleScope handle_scope(isolate());
  v8::Context::Scope context_scope(context());
  v8::Global<v8::Object> resource_monitor;
  struct Monitor {
    raw_ptr<v8::Global<v8::Object>> handle;
    bool collected = false;
  } monitor{&resource_monitor};
  napi_env expired_env = nullptr;
  napi_async_context expired_context = nullptr;
  {
    v8::HandleScope nested_scope(isolate());
    auto env = std::make_unique<napi_env__>(isolate(), context());
    expired_env = env.get();
    napi_value resource;
    napi_value name;
    ASSERT_EQ(napi_create_object(env.get(), &resource), napi_ok);
    ASSERT_EQ(napi_create_int32(env.get(), 42, &name), napi_ok);
    // Node-API coerces the resource name to string rather than requiring a
    // string napi_value, and permits a null optional resource pointer.
    ASSERT_EQ(napi_async_init(env.get(), resource, name, &expired_context),
              napi_ok);
    napi_async_context anonymous_context;
    ASSERT_EQ(napi_async_init(env.get(), nullptr, name, &anonymous_context),
              napi_ok);
    resource_monitor.Reset(isolate(), resource->Get().As<v8::Object>());
    resource_monitor.SetWeak(
        &monitor,
        [](const v8::WeakCallbackInfo<Monitor>& info) {
          info.GetParameter()->handle->Reset();
          info.GetParameter()->collected = true;
        },
        v8::WeakCallbackType::kParameter);
    env.reset();
  }
  isolate()->LowMemoryNotification();
  EXPECT_TRUE(monitor.collected);
  EXPECT_EQ(napi_async_destroy(expired_env, expired_context), napi_invalid_arg);
}

TEST_F(NapiLoaderTest, AsyncCallbackRejectsForeignValuesAndContexts) {
  v8::Locker locker(isolate());
  v8::Isolate::Scope isolate_scope(isolate());
  v8::HandleScope handle_scope(isolate());
  v8::Context::Scope context_scope(context());
  auto env = std::make_unique<napi_env__>(isolate(), context());
  auto foreign = std::make_unique<napi_env__>(isolate(), context());
  napi_value receiver = EvaluateNapiFixture(env.get(), "({})");
  napi_value foreign_receiver = EvaluateNapiFixture(foreign.get(), "({})");
  napi_value function = EvaluateNapiFixture(env.get(), "(() => 42)");
  napi_value foreign_function =
      EvaluateNapiFixture(foreign.get(), "(() => 42)");
  napi_value name = EvaluateNapiFixture(env.get(), "'resource'");
  napi_async_context async_context = nullptr;
  EXPECT_EQ(napi_async_init(env.get(), foreign_receiver, name, &async_context),
            napi_invalid_arg);
  EXPECT_EQ(nullptr, async_context);
  ASSERT_EQ(napi_async_init(env.get(), receiver, name, &async_context),
            napi_ok);
  EXPECT_EQ(napi_async_destroy(foreign.get(), async_context), napi_invalid_arg);
  EXPECT_EQ(napi_make_callback(foreign.get(), async_context, foreign_receiver,
                               foreign_function, 0, nullptr, nullptr),
            napi_invalid_arg);
  EXPECT_EQ(napi_make_callback(env.get(), async_context, receiver,
                               foreign_function, 0, nullptr, nullptr),
            napi_invalid_arg);
  EXPECT_EQ(napi_make_callback(env.get(), async_context, receiver, function, 1,
                               &foreign_receiver, nullptr),
            napi_invalid_arg);
  EXPECT_EQ(napi_make_callback(env.get(), async_context, nullptr, function, 0,
                               nullptr, nullptr),
            napi_invalid_arg);
  EXPECT_EQ(napi_fatal_exception(env.get(), foreign_receiver),
            napi_invalid_arg);
  ASSERT_EQ(napi_async_destroy(env.get(), async_context), napi_ok);
  EXPECT_EQ(napi_make_callback(env.get(), async_context, receiver, function, 0,
                               nullptr, nullptr),
            napi_invalid_arg);
  napi_async_context replacement = nullptr;
  ASSERT_EQ(napi_async_init(env.get(), receiver, name, &replacement), napi_ok);
  EXPECT_NE(async_context, replacement);
}

TEST_F(NapiLoaderTest, AsyncFreerExceptionIsHandledAtItsOwnCallbackBoundary) {
  v8::Locker locker(isolate());
  v8::Isolate::Scope isolate_scope(isolate());
  v8::HandleScope handle_scope(isolate());
  v8::Context::Scope context_scope(context());
  auto env = std::make_unique<napi_env__>(isolate(), context());
  napi_value resource = EvaluateNapiFixture(env.get(), "({})");
  napi_value name = EvaluateNapiFixture(env.get(), "'freer'");
  napi_async_context async_context;
  ASSERT_EQ(napi_async_init(env.get(), resource, name, &async_context),
            napi_ok);
  napi_value dispatcher;
  ASSERT_EQ(napi_create_function(env.get(), "dispatchFreer", NAPI_AUTO_LENGTH,
                                 DispatchAsyncFreer, nullptr, &dispatcher),
            napi_ok);
  napi_value throwing =
      EvaluateNapiFixture(env.get(),
                          "(function() { 'use strict'; if (this !== null) "
                          "throw new Error('wrong receiver'); "
                          "throw new Error('freer failure'); })");
  napi_value argument = EvaluateNapiFixture(env.get(), "42");
  napi_value args[] = {throwing, argument};
  napi_value result = argument;
  EXPECT_EQ(napi_make_callback(env.get(), async_context, resource, dispatcher,
                               2, args, &result),
            napi_pending_exception);
  EXPECT_EQ(argument, result);
  bool pending = false;
  EXPECT_EQ(napi_is_exception_pending(env.get(), &pending), napi_ok);
  EXPECT_TRUE(pending);
  // An uncleared exception must not be overwritten by another callback.
  EXPECT_EQ(napi_make_callback(env.get(), async_context, resource, dispatcher,
                               2, args, &result),
            napi_pending_exception);
  napi_value exception;
  ASSERT_EQ(napi_get_and_clear_last_exception(env.get(), &exception), napi_ok);
  int reported = 0;
  env->uncaught_exception_handler = base::BindRepeating(
      [](napi_env expected_env, int* reported, v8::Local<v8::Context> context,
         v8::Local<v8::Value> error) {
        EXPECT_EQ(expected_env->GetContext(), context);
        v8::String::Utf8Value text(expected_env->isolate, error);
        EXPECT_STREQ("Error: freer failure", *text);
        ++*reported;
        return true;
      },
      env.get(), &reported);
  EXPECT_EQ(napi_fatal_exception(env.get(), exception), napi_ok);
  EXPECT_EQ(1, reported);
  EXPECT_EQ(napi_is_exception_pending(env.get(), &pending), napi_ok);
  EXPECT_FALSE(pending);
  args[0] = EvaluateNapiFixture(
      env.get(),
      "(function(value) { 'use strict'; if (this !== null) "
      "throw new Error('wrong receiver'); return value; })");
  ASSERT_EQ(napi_make_callback(env.get(), async_context, resource, dispatcher,
                               2, args, &result),
            napi_ok);
  int32_t value;
  EXPECT_EQ(napi_get_value_int32(env.get(), result, &value), napi_ok);
  EXPECT_EQ(42, value);
  EXPECT_EQ(0u, env->async_callback_depth);
  EXPECT_EQ(nullptr, env->active_async_context);
}

TEST_F(NapiLoaderTest, AsyncCallbackDrainsMicrotasksOnlyAtOutermostBoundary) {
  v8::Locker locker(isolate());
  v8::Isolate::Scope isolate_scope(isolate());
  v8::HandleScope handle_scope(isolate());
  v8::Context::Scope context_scope(context());
  isolate()->SetMicrotasksPolicy(v8::MicrotasksPolicy::kExplicit);
  auto env = std::make_unique<napi_env__>(isolate(), context());
  napi_value resource = EvaluateNapiFixture(env.get(), "({})");
  napi_value name = EvaluateNapiFixture(env.get(), "'resource'");
  napi_async_context async_context;
  ASSERT_EQ(napi_async_init(env.get(), resource, name, &async_context),
            napi_ok);
  napi_value inner = EvaluateNapiFixture(
      env.get(),
      "(() => { trace.push('inner'); Promise.resolve().then("
      "() => trace.push('microtask')); })");
  struct Nested {
    napi_async_context context;
    napi_value receiver;
    napi_value function;
  } nested{async_context, resource, inner};
  napi_value nested_callback;
  ASSERT_EQ(napi_create_function(
                env.get(), "nested", NAPI_AUTO_LENGTH,
                [](napi_env env, napi_callback_info info) -> napi_value {
                  void* data;
                  napi_get_cb_info(env, info, nullptr, nullptr, nullptr, &data);
                  auto* nested = static_cast<Nested*>(data);
                  EXPECT_EQ(1u, env->async_callback_depth);
                  EXPECT_EQ(nested->context, env->active_async_context);
                  napi_value result = nullptr;
                  EXPECT_EQ(
                      napi_make_callback(env, nested->context, nested->receiver,
                                         nested->function, 0, nullptr, &result),
                      napi_ok);
                  EXPECT_EQ(1u, env->async_callback_depth);
                  return result;
                },
                &nested, &nested_callback),
            napi_ok);
  napi_value outer =
      EvaluateNapiFixture(env.get(),
                          "(function(nested) { globalThis.trace = ['outer']; "
                          "nested(); trace.push('after'); })");
  EXPECT_EQ(napi_make_callback(env.get(), async_context, resource, outer, 1,
                               &nested_callback, nullptr),
            napi_ok);
  auto trace = EvaluateNapiFixture(env.get(), "trace.join(',')");
  v8::String::Utf8Value text(isolate(), trace->Get());
  EXPECT_STREQ("outer,inner,after,microtask", *text);
  EXPECT_EQ(0u, env->async_callback_depth);
  EXPECT_EQ(nullptr, env->active_async_context);
}

TEST_F(NapiLoaderTest, AsyncCallbackNullReceiverPreservesConversionException) {
  v8::Locker locker(isolate());
  v8::Isolate::Scope isolate_scope(isolate());
  v8::HandleScope handle_scope(isolate());
  v8::Context::Scope context_scope(context());
  auto env = std::make_unique<napi_env__>(isolate(), context());
  napi_value receiver = EvaluateNapiFixture(env.get(), "null");
  napi_value callback = EvaluateNapiFixture(env.get(), "(() => 42)");
  EXPECT_EQ(napi_make_callback(env.get(), nullptr, receiver, callback, 0,
                               nullptr, nullptr),
            napi_object_expected);
  bool pending = false;
  EXPECT_EQ(napi_is_exception_pending(env.get(), &pending), napi_ok);
  EXPECT_TRUE(pending);
  napi_value exception;
  ASSERT_EQ(napi_get_and_clear_last_exception(env.get(), &exception), napi_ok);
  EXPECT_TRUE(exception->Get()->IsNativeError());
  receiver = EvaluateNapiFixture(env.get(), "({})");
  EXPECT_EQ(napi_make_callback(env.get(), nullptr, receiver, callback, 0,
                               nullptr, nullptr),
            napi_ok);
}

TEST_F(NapiLoaderTest, FatalAsyncExceptionReportsToV8AndTerminatesByDefault) {
  v8::Locker locker(isolate());
  v8::Isolate::Scope isolate_scope(isolate());
  v8::HandleScope handle_scope(isolate());
  v8::Context::Scope context_scope(context());
  auto env = std::make_unique<napi_env__>(isolate(), context());
  napi_value error =
      EvaluateNapiFixture(env.get(), "new Error('native failure')");
  ASSERT_TRUE(isolate()->AddMessageListener(ReportFatalAsyncMessage));
  EXPECT_DEATH_IF_SUPPORTED(napi_fatal_exception(env.get(), error),
                            "NAPI_V8_UNCAUGHT_REPORT");
  isolate()->RemoveMessageListeners(ReportFatalAsyncMessage);
}

TEST_F(NapiLoaderTest, ReferenceTransitionsBetweenWeakAndStrong) {
  v8::Locker locker(isolate());
  v8::Isolate::Scope isolate_scope(isolate());
  v8::HandleScope handle_scope(isolate());
  v8::Context::Scope context_scope(context());

  auto env = std::make_unique<napi_env__>(isolate(), context());
  napi_handle_scope scope;
  ASSERT_EQ(napi_open_handle_scope(env.get(), &scope), napi_ok);
  napi_value object;
  ASSERT_EQ(napi_create_object(env.get(), &object), napi_ok);
  napi_ref reference;
  ASSERT_EQ(napi_create_reference(env.get(), object, 0, &reference), napi_ok);

  uint32_t ref_count = 0;
  EXPECT_EQ(napi_reference_ref(env.get(), reference, &ref_count), napi_ok);
  EXPECT_EQ(ref_count, 1u);
  ASSERT_EQ(napi_close_handle_scope(env.get(), scope), napi_ok);
  isolate()->LowMemoryNotification();
  ASSERT_EQ(napi_open_handle_scope(env.get(), &scope), napi_ok);
  napi_value retained = nullptr;
  EXPECT_EQ(napi_get_reference_value(env.get(), reference, &retained), napi_ok);
  EXPECT_NE(retained, nullptr);
  ASSERT_EQ(napi_close_handle_scope(env.get(), scope), napi_ok);
  EXPECT_EQ(napi_reference_unref(env.get(), reference, &ref_count), napi_ok);
  EXPECT_EQ(ref_count, 0u);
  isolate()->LowMemoryNotification();
  EXPECT_EQ(napi_get_reference_value(env.get(), reference, &retained), napi_ok);
  EXPECT_EQ(retained, nullptr);
  EXPECT_EQ(napi_delete_reference(env.get(), reference), napi_ok);
  EXPECT_TRUE(env->references.empty());
}

TEST_F(NapiLoaderTest,
       LateReferenceOperationsAfterEnvironmentAndIsolateDispose) {
  auto allocator = std::unique_ptr<v8::ArrayBuffer::Allocator>(
      v8::ArrayBuffer::Allocator::NewDefaultAllocator());
  v8::Isolate::CreateParams create_params;
  create_params.array_buffer_allocator = allocator.get();
  v8::Isolate* native_isolate = v8::Isolate::New(create_params);
  napi_env expired_env = nullptr;
  napi_ref strong = nullptr;
  napi_ref weak = nullptr;
  {
    v8::Locker locker(native_isolate);
    v8::Isolate::Scope isolate_scope(native_isolate);
    v8::HandleScope handle_scope(native_isolate);
    auto local_context = v8::Context::New(native_isolate);
    v8::Context::Scope context_scope(local_context);
    auto env = std::make_unique<napi_env__>(native_isolate, local_context);
    expired_env = env.get();
    napi_value object = nullptr;
    EXPECT_EQ(napi_create_object(env.get(), &object), napi_ok);
    EXPECT_EQ(napi_create_reference(env.get(), object, 1, &strong), napi_ok);
    EXPECT_EQ(napi_create_reference(env.get(), object, 0, &weak), napi_ok);
    EXPECT_EQ(env->references.size(), 2u);
    env.reset();
  }
  native_isolate->Dispose();
  allocator.reset();

  // Models a dlopen-retained addon's static destructor at process exit. Both
  // the env and its isolate are gone; neither pointer may be dereferenced.
  for (napi_ref reference : {strong, weak}) {
    EXPECT_EQ(napi_delete_reference(expired_env, reference), napi_invalid_arg);
    uint32_t count = 99;
    EXPECT_EQ(napi_reference_ref(expired_env, reference, &count),
              napi_invalid_arg);
    EXPECT_EQ(napi_reference_unref(expired_env, reference, &count),
              napi_invalid_arg);
    EXPECT_EQ(count, 99u);
    napi_value value = nullptr;
    EXPECT_EQ(napi_get_reference_value(expired_env, reference, &value),
              napi_invalid_arg);
    EXPECT_EQ(value, nullptr);
  }
}

TEST_F(NapiLoaderTest, ReferenceTokensCannotDeleteAnotherOrReusedEnvironment) {
  v8::Locker locker(isolate());
  v8::Isolate::Scope isolate_scope(isolate());
  v8::HandleScope handle_scope(isolate());
  v8::Context::Scope context_scope(context());
  std::optional<napi_env__> reused;
  reused.emplace(isolate(), context());
  napi_env old_address = &*reused;
  napi_value object = nullptr;
  ASSERT_EQ(napi_create_object(&*reused, &object), napi_ok);
  napi_ref expired = nullptr;
  ASSERT_EQ(napi_create_reference(&*reused, object, 1, &expired), napi_ok);
  reused.reset();
  reused.emplace(isolate(), context());
  ASSERT_EQ(&*reused, old_address);
  ASSERT_EQ(napi_create_object(&*reused, &object), napi_ok);
  napi_ref current = nullptr;
  ASSERT_EQ(napi_create_reference(&*reused, object, 1, &current), napi_ok);
  EXPECT_NE(current, expired);
  EXPECT_EQ(napi_delete_reference(old_address, expired), napi_invalid_arg);
  auto foreign = std::make_unique<napi_env__>(isolate(), context());
  EXPECT_EQ(napi_delete_reference(foreign.get(), current), napi_invalid_arg);
  uint32_t count = 99;
  napi_value value = nullptr;
  EXPECT_EQ(napi_reference_ref(foreign.get(), current, &count),
            napi_invalid_arg);
  EXPECT_EQ(napi_reference_unref(foreign.get(), current, &count),
            napi_invalid_arg);
  EXPECT_EQ(napi_get_reference_value(foreign.get(), current, &value),
            napi_invalid_arg);
  EXPECT_EQ(count, 99u);
  ASSERT_EQ(napi_get_reference_value(&*reused, current, &value), napi_ok);
  EXPECT_TRUE(value->Get()->StrictEquals(object->Get()));
  EXPECT_EQ(napi_delete_reference(&*reused, current), napi_ok);
  EXPECT_EQ(napi_delete_reference(&*reused, current), napi_invalid_arg);
  EXPECT_TRUE(reused->references.empty());
}

TEST_F(NapiLoaderTest, ReferencesRemainUsableThroughCleanupAndFinalizers) {
  v8::Locker locker(isolate());
  v8::Isolate::Scope isolate_scope(isolate());
  v8::HandleScope handle_scope(isolate());
  v8::Context::Scope context_scope(context());
  auto env = std::make_unique<napi_env__>(isolate(), context());
  struct Capture {
    napi_env env;
    napi_ref ref;
    std::vector<int> calls;
  } capture{env.get(), nullptr, {}};
  napi_value object = nullptr;
  ASSERT_EQ(napi_create_object(env.get(), &object), napi_ok);
  ASSERT_EQ(napi_create_reference(env.get(), object, 1, &capture.ref), napi_ok);
  ASSERT_EQ(napi_add_env_cleanup_hook(
                env.get(),
                [](void* data) {
                  auto* capture = static_cast<Capture*>(data);
                  napi_value value = nullptr;
                  EXPECT_EQ(napi_get_reference_value(capture->env, capture->ref,
                                                     &value),
                            napi_ok);
                  EXPECT_NE(value, nullptr);
                  capture->calls.push_back(1);
                },
                &capture),
            napi_ok);
  napi_value external = nullptr;
  ASSERT_EQ(napi_create_external(
                env.get(), &capture,
                [](napi_env env, void* data, void*) {
                  auto* capture = static_cast<Capture*>(data);
                  EXPECT_EQ(napi_delete_reference(env, capture->ref), napi_ok);
                  napi_value value = nullptr;
                  EXPECT_EQ(napi_create_object(env, &value), napi_ok);
                  EXPECT_EQ(napi_create_reference(env, value, 1, &capture->ref),
                            napi_ok);
                  capture->calls.push_back(2);
                },
                nullptr, &external),
            napi_ok);
  ASSERT_EQ(napi_set_instance_data(
                env.get(), &capture,
                [](napi_env env, void* data, void*) {
                  auto* capture = static_cast<Capture*>(data);
                  napi_value value = nullptr;
                  EXPECT_EQ(napi_get_reference_value(env, capture->ref, &value),
                            napi_ok);
                  EXPECT_NE(value, nullptr);
                  capture->calls.push_back(3);
                },
                nullptr),
            napi_ok);
  env.reset();
  EXPECT_EQ(capture.calls, std::vector<int>({1, 2, 3}));
  EXPECT_EQ(napi_delete_reference(capture.env, capture.ref), napi_invalid_arg);
}

TEST_F(NapiLoaderTest, CreateReferenceRejectsForeignNapiValue) {
  v8::Locker locker(isolate());
  v8::Isolate::Scope isolate_scope(isolate());
  v8::HandleScope handle_scope(isolate());
  v8::Context::Scope context_scope(context());

  auto env = std::make_unique<napi_env__>(isolate(), context());
  auto* fake = reinterpret_cast<napi_value>(static_cast<uintptr_t>(-1));
  napi_ref reference = nullptr;
  EXPECT_EQ(napi_create_reference(env.get(), fake, 1, &reference),
            napi_invalid_arg);
  EXPECT_EQ(reference, nullptr);
}

TEST_F(NapiLoaderTest, CreateReferenceEnforcesStableNodeApiValueTypes) {
  v8::Locker locker(isolate());
  v8::Isolate::Scope isolate_scope(isolate());
  v8::HandleScope handle_scope(isolate());
  v8::Context::Scope context_scope(context());
  auto env = std::make_unique<napi_env__>(isolate(), context());
  v8::Local<v8::Function> function;
  ASSERT_TRUE(v8::Function::New(
                  context(), [](const v8::FunctionCallbackInfo<v8::Value>&) {})
                  .ToLocal(&function));
  v8::Local<v8::Value> valid_values[] = {v8::Object::New(isolate()), function,
                                         v8::Symbol::New(isolate())};
  for (v8::Local<v8::Value> value : valid_values) {
    napi_ref reference = nullptr;
    ASSERT_EQ(napi_create_reference(env.get(), env->CreateValue(value), 1,
                                    &reference),
              napi_ok);
    napi_value result = nullptr;
    ASSERT_EQ(napi_get_reference_value(env.get(), reference, &result), napi_ok);
    EXPECT_TRUE(result->Get()->StrictEquals(value));
    EXPECT_EQ(napi_delete_reference(env.get(), reference), napi_ok);
  }

  napi_ref sentinel = nullptr;
  ASSERT_EQ(napi_create_reference(env.get(),
                                  env->CreateValue(v8::Object::New(isolate())),
                                  1, &sentinel),
            napi_ok);
  v8::Local<v8::Value> invalid_values[] = {
      v8::Undefined(isolate()),
      v8::Null(isolate()),
      v8::True(isolate()),
      v8::Number::New(isolate(), 42),
      v8::BigInt::New(isolate(), 7),
      v8::String::NewFromUtf8Literal(isolate(), "primitive")};
  for (v8::Local<v8::Value> value : invalid_values) {
    for (uint32_t initial_refcount : {0u, 1u}) {
      napi_ref result = sentinel;
      EXPECT_EQ(napi_create_reference(env.get(), env->CreateValue(value),
                                      initial_refcount, &result),
                napi_invalid_arg);
      EXPECT_EQ(result, sentinel);
      const napi_extended_error_info* error_info = nullptr;
      ASSERT_EQ(napi_get_last_error_info(env.get(), &error_info), napi_ok);
      EXPECT_EQ(error_info->error_code, napi_invalid_arg);
      bool pending = true;
      EXPECT_EQ(napi_is_exception_pending(env.get(), &pending), napi_ok);
      EXPECT_FALSE(pending);
    }
  }
  EXPECT_EQ(napi_delete_reference(env.get(), sentinel), napi_ok);
}

TEST_F(NapiLoaderTest, MissingOptionalCallbackDoesNotBecomeCallableReference) {
  v8::Locker locker(isolate());
  v8::Isolate::Scope isolate_scope(isolate());
  v8::HandleScope handle_scope(isolate());
  v8::Context::Scope context_scope(context());
  auto env = std::make_unique<napi_env__>(isolate(), context());
  napi_ref optional_callback = nullptr;
  EXPECT_EQ(napi_create_reference(env.get(),
                                  env->CreateValue(v8::Undefined(isolate())), 1,
                                  &optional_callback),
            napi_invalid_arg);
  EXPECT_EQ(optional_callback, nullptr);
  napi_value callback_value = nullptr;
  EXPECT_EQ(
      napi_get_reference_value(env.get(), optional_callback, &callback_value),
      napi_invalid_arg);
  EXPECT_EQ(callback_value, nullptr);

  int calls = 0;
  v8::Local<v8::Function> callback;
  ASSERT_TRUE(v8::Function::New(
                  context(),
                  [](const v8::FunctionCallbackInfo<v8::Value>& info) {
                    auto* call_count =
                        static_cast<int*>(info.Data().As<v8::External>()->Value(
                            v8::kExternalPointerTypeTagDefault));
                    ++*call_count;
                  },
                  v8::External::New(isolate(), &calls,
                                    v8::kExternalPointerTypeTagDefault))
                  .ToLocal(&callback));
  ASSERT_EQ(napi_create_reference(env.get(), env->CreateValue(callback), 1,
                                  &optional_callback),
            napi_ok);
  ASSERT_EQ(
      napi_get_reference_value(env.get(), optional_callback, &callback_value),
      napi_ok);
  ASSERT_NE(callback_value, nullptr);
  EXPECT_EQ(napi_call_function(env.get(), nullptr, callback_value, 0, nullptr,
                               nullptr),
            napi_ok);
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(napi_delete_reference(env.get(), optional_callback), napi_ok);
}

TEST_F(NapiLoaderTest, SetElementRejectsForeignNapiValue) {
  v8::Locker locker(isolate());
  v8::Isolate::Scope isolate_scope(isolate());
  v8::HandleScope handle_scope(isolate());
  v8::Context::Scope context_scope(context());

  auto env = std::make_unique<napi_env__>(isolate(), context());
  napi_value array;
  ASSERT_EQ(napi_create_array_with_length(env.get(), 1, &array), napi_ok);
  auto* fake = reinterpret_cast<napi_value>(static_cast<uintptr_t>(-1));
  EXPECT_EQ(napi_set_element(env.get(), array, 0, fake), napi_invalid_arg);
  EXPECT_EQ(napi_set_element(env.get(), fake, 0, array), napi_invalid_arg);
}

TEST_F(NapiLoaderTest, RemoveWrapCancelsFinalizer) {
  v8::Locker locker(isolate());
  v8::Isolate::Scope isolate_scope(isolate());
  v8::HandleScope handle_scope(isolate());
  v8::Context::Scope context_scope(context());

  auto env = std::make_unique<napi_env__>(isolate(), context());
  napi_value object;
  ASSERT_EQ(napi_create_object(env.get(), &object), napi_ok);
  bool finalized = false;
  ASSERT_EQ(napi_wrap(env.get(), object, &finalized, &MarkFinalized, nullptr,
                      nullptr),
            napi_ok);

  void* native_object = nullptr;
  EXPECT_EQ(napi_remove_wrap(env.get(), object, &native_object), napi_ok);
  EXPECT_EQ(native_object, &finalized);
  EXPECT_FALSE(finalized);
  EXPECT_EQ(napi_unwrap(env.get(), object, &native_object), napi_invalid_arg);
}

TEST_F(NapiLoaderTest, EnvironmentDestructionRunsFinalizers) {
  v8::Locker locker(isolate());
  v8::Isolate::Scope isolate_scope(isolate());
  v8::HandleScope handle_scope(isolate());
  v8::Context::Scope context_scope(context());

  bool wrap_finalized = false;
  bool external_finalized = false;
  {
    auto env = std::make_unique<napi_env__>(isolate(), context());
    napi_value object;
    ASSERT_EQ(napi_create_object(env.get(), &object), napi_ok);
    ASSERT_EQ(napi_wrap(env.get(), object, &wrap_finalized, &MarkFinalized,
                        nullptr, nullptr),
              napi_ok);

    napi_value external;
    ASSERT_EQ(napi_create_external(env.get(), &external_finalized,
                                   &MarkFinalized, nullptr, &external),
              napi_ok);
  }

  EXPECT_TRUE(wrap_finalized);
  EXPECT_TRUE(external_finalized);
}

TEST_F(NapiLoaderTest, CleanupHooksRunInReverseOrderBeforeFinalizers) {
  v8::Locker locker(isolate());
  v8::Isolate::Scope isolate_scope(isolate());
  v8::HandleScope handle_scope(isolate());
  v8::Context::Scope context_scope(context());

  std::vector<int> calls;
  CleanupCall first{&calls, 1};
  CleanupCall second{&calls, 2};
  CleanupCall finalizer{&calls, 3};
  CleanupCall instance_finalizer{&calls, 4};
  auto env = std::make_unique<napi_env__>(isolate(), context());
  ASSERT_EQ(napi_add_env_cleanup_hook(env.get(), RecordCleanupCall, &first),
            napi_ok);
  ASSERT_EQ(napi_add_env_cleanup_hook(env.get(), RecordCleanupCall, &second),
            napi_ok);
  napi_value external;
  ASSERT_EQ(napi_create_external(env.get(), &finalizer, RecordCleanupFinalizer,
                                 nullptr, &external),
            napi_ok);
  ASSERT_EQ(napi_set_instance_data(env.get(), &instance_finalizer,
                                   RecordCleanupFinalizer, nullptr),
            napi_ok);
  EXPECT_TRUE(calls.empty());
  env.reset();
  EXPECT_EQ(calls, (std::vector<int>{2, 1, 3, 4}));
}

TEST_F(NapiLoaderTest, CleanupHooksHonorRemovalAndDrainNewHooksInNextBatch) {
  v8::Locker locker(isolate());
  v8::Isolate::Scope isolate_scope(isolate());
  v8::HandleScope handle_scope(isolate());
  v8::Context::Scope context_scope(context());

  std::vector<int> calls;
  CleanupCall first{&calls, 1};
  CleanupCall removed{&calls, 2};
  CleanupCall third{&calls, 3};
  CleanupCall current{&calls, 4};
  CleanupCall added_first{&calls, 5};
  CleanupCall added_last{&calls, 6};
  auto env = std::make_unique<napi_env__>(isolate(), context());
  CleanupMutation mutation{env.get(), &current, &removed, &added_first,
                           &added_last};
  ASSERT_EQ(napi_add_env_cleanup_hook(env.get(), RecordCleanupCall, &first),
            napi_ok);
  ASSERT_EQ(napi_add_env_cleanup_hook(env.get(), RecordCleanupCall, &removed),
            napi_ok);
  ASSERT_EQ(napi_add_env_cleanup_hook(env.get(), RecordCleanupCall, &third),
            napi_ok);
  ASSERT_EQ(napi_add_env_cleanup_hook(env.get(), MutateCleanupHooks, &mutation),
            napi_ok);
  env.reset();
  EXPECT_EQ(calls, (std::vector<int>{4, 3, 1, 6, 5}));
}

TEST_F(NapiLoaderTest, CleanupHookArgumentsAndRepeatedRemoval) {
  v8::Locker locker(isolate());
  v8::Isolate::Scope isolate_scope(isolate());
  v8::HandleScope handle_scope(isolate());
  v8::Context::Scope context_scope(context());

  auto env = std::make_unique<napi_env__>(isolate(), context());
  EXPECT_EQ(napi_add_env_cleanup_hook(nullptr, RecordCleanupCall, nullptr),
            napi_invalid_arg);
  EXPECT_EQ(napi_remove_env_cleanup_hook(nullptr, RecordCleanupCall, nullptr),
            napi_invalid_arg);
  EXPECT_EQ(napi_add_env_cleanup_hook(env.get(), nullptr, nullptr),
            napi_invalid_arg);
  EXPECT_EQ(napi_remove_env_cleanup_hook(env.get(), nullptr, nullptr),
            napi_invalid_arg);
  const napi_extended_error_info* error_info = nullptr;
  ASSERT_EQ(napi_get_last_error_info(env.get(), &error_info), napi_ok);
  EXPECT_EQ(error_info->error_code, napi_invalid_arg);
  EXPECT_EQ(napi_add_env_cleanup_hook(env.get(), RecordCleanupCall, nullptr),
            napi_ok);
  ASSERT_EQ(napi_get_last_error_info(env.get(), &error_info), napi_ok);
  EXPECT_EQ(error_info->error_code, napi_ok);
  EXPECT_EQ(napi_remove_env_cleanup_hook(env.get(), RecordCleanupCall, nullptr),
            napi_ok);
  EXPECT_EQ(napi_remove_env_cleanup_hook(env.get(), RecordCleanupCall, nullptr),
            napi_ok);
  env.reset();
}

TEST_F(NapiLoaderTest, BufferUsesUint8ArraySemantics) {
  v8::Locker locker(isolate());
  v8::Isolate::Scope isolate_scope(isolate());
  v8::HandleScope handle_scope(isolate());
  v8::Context::Scope context_scope(context());

  auto env = std::make_unique<napi_env__>(isolate(), context());
  void* data = nullptr;
  napi_value buffer;
  ASSERT_EQ(napi_create_buffer(env.get(), 8, &data, &buffer), napi_ok);
  ASSERT_NE(data, nullptr);
  EXPECT_TRUE(buffer->Get()->IsUint8Array());

  bool is_buffer = false;
  EXPECT_EQ(napi_is_buffer(env.get(), buffer, &is_buffer), napi_ok);
  EXPECT_TRUE(is_buffer);
  size_t length = 0;
  void* buffer_data = nullptr;
  EXPECT_EQ(napi_get_buffer_info(env.get(), buffer, &buffer_data, &length),
            napi_ok);
  EXPECT_EQ(buffer_data, data);
  EXPECT_EQ(length, 8u);

  napi_value array_buffer;
  ASSERT_EQ(napi_create_arraybuffer(env.get(), 8, nullptr, &array_buffer),
            napi_ok);
  napi_value typed_array;
  ASSERT_EQ(napi_create_typedarray(env.get(), napi_uint8_array, 8, array_buffer,
                                   0, &typed_array),
            napi_ok);
  EXPECT_EQ(napi_is_buffer(env.get(), typed_array, &is_buffer), napi_ok);
  EXPECT_TRUE(is_buffer);
  EXPECT_FALSE(v8impl::IsMarkedBuffer(context(), typed_array->Get()));
  EXPECT_TRUE(v8impl::IsMarkedBuffer(context(), buffer->Get()));
  EXPECT_EQ(napi_is_buffer(env.get(), array_buffer, &is_buffer), napi_ok);
  EXPECT_FALSE(is_buffer);
  EXPECT_EQ(
      napi_get_buffer_info(env.get(), array_buffer, &buffer_data, &length),
      napi_invalid_arg);
}

TEST_F(NapiLoaderTest, BufferInfoAcceptsViewsAndUsesTheirActiveByteRange) {
  v8::Locker locker(isolate());
  v8::Isolate::Scope isolate_scope(isolate());
  v8::HandleScope handle_scope(isolate());
  v8::Context::Scope context_scope(context());
  auto env = std::make_unique<napi_env__>(isolate(), context());
  napi_value array_buffer;
  void* storage = nullptr;
  ASSERT_EQ(napi_create_arraybuffer(env.get(), 16, &storage, &array_buffer),
            napi_ok);
  auto backing = array_buffer->Get().As<v8::ArrayBuffer>();
  v8::Local<v8::Value> views[] = {
      v8::Uint8Array::New(backing, 3, 4),
      v8::Uint8ClampedArray::New(backing, 3, 4),
      v8::Int8Array::New(backing, 3, 4),
      v8::Uint16Array::New(backing, 2, 3),
      v8::Int32Array::New(backing, 4, 2),
      v8::Float64Array::New(backing, 8, 1),
      v8::BigUint64Array::New(backing, 8, 1),
      v8::DataView::New(backing, 3, 5),
      v8::Uint8Array::New(backing, 5, 0),
  };
  for (auto view : views) {
    napi_value value = env->CreateValue(view);
    bool is_buffer = false;
    ASSERT_EQ(napi_is_buffer(env.get(), value, &is_buffer), napi_ok);
    EXPECT_TRUE(is_buffer);
    void* data = nullptr;
    size_t length = 0;
    ASSERT_EQ(napi_get_buffer_info(env.get(), value, &data, &length), napi_ok);
    auto array_view = view.As<v8::ArrayBufferView>();
    EXPECT_EQ(data, UNSAFE_BUFFERS(static_cast<uint8_t*>(storage) +
                                   array_view->ByteOffset()));
    EXPECT_EQ(length, array_view->ByteLength());
    EXPECT_FALSE(v8impl::IsMarkedBuffer(context(), view));
  }

  napi_value empty;
  ASSERT_EQ(napi_create_buffer(env.get(), 0, nullptr, &empty), napi_ok);
  size_t length = 1;
  void* data = storage;
  ASSERT_EQ(napi_get_buffer_info(env.get(), empty, &data, &length), napi_ok);
  EXPECT_EQ(length, 0u);
}

TEST_F(NapiLoaderTest, ThreadsafeFunctionHonorsQueueAndFinalizes) {
  v8::Locker locker(isolate());
  v8::Isolate::Scope isolate_scope(isolate());
  v8::HandleScope handle_scope(isolate());
  v8::Context::Scope context_scope(context());

  auto env = std::make_unique<napi_env__>(isolate(), context());
  int call_count = 0;
  bool finalized = false;
  napi_threadsafe_function function;
  ASSERT_EQ(napi_create_threadsafe_function(
                env.get(), nullptr, nullptr, nullptr, 1, 1, &finalized,
                &MarkFinalized, &call_count, &CountThreadsafeCall, &function),
            napi_ok);

  EXPECT_EQ(
      napi_call_threadsafe_function(function, nullptr, napi_tsfn_nonblocking),
      napi_ok);
  EXPECT_EQ(
      napi_call_threadsafe_function(function, nullptr, napi_tsfn_nonblocking),
      napi_queue_full);
  EXPECT_EQ(
      napi_call_threadsafe_function(function, nullptr, napi_tsfn_blocking),
      napi_would_deadlock);
  EXPECT_EQ(napi_release_threadsafe_function(function, napi_tsfn_release),
            napi_ok);

  task_environment_.RunUntilIdle();
  EXPECT_EQ(call_count, 1);
  EXPECT_TRUE(finalized);
}

TEST_F(NapiLoaderTest, AsyncWorkExecution) {
  v8::Locker locker(isolate());
  v8::Isolate::Scope isolate_scope(isolate());
  v8::HandleScope handle_scope(isolate());
  v8::Context::Scope context_scope(context());

  napi_env env = new napi_env__(isolate(), context());

  struct WorkData {
    bool executed = false;
    bool completed = false;
  };
  WorkData data;

  napi_async_work work;
  napi_value resource = nullptr;
  napi_value resource_name = nullptr;
  
  EXPECT_EQ(napi_create_async_work(
                env, resource, resource_name,
                [](napi_env env, void* data) {
                  auto* d = static_cast<WorkData*>(data);
                  d->executed = true;
                },
                [](napi_env env, napi_status status, void* data) {
                  auto* d = static_cast<WorkData*>(data);
                  d->completed = true;
                },
                &data, &work),
            napi_ok);

  EXPECT_EQ(napi_queue_async_work(env, work), napi_ok);

  task_environment_.RunUntilIdle();

  EXPECT_TRUE(data.executed);
  EXPECT_TRUE(data.completed);

  EXPECT_EQ(napi_delete_async_work(env, work), napi_ok);
  delete env;
}

#if BUILDFLAG(IS_MAC) && BUILDFLAG(ENABLE_XENON_NODE_UV_COMPAT)
TEST_F(NapiLoaderTest, StaticRegistrationUsesRecordFromLoadedImage) {
  v8::Locker locker(isolate());
  v8::Isolate::Scope isolate_scope(isolate());
  v8::HandleScope handle_scope(isolate());
  v8::Context::Scope context_scope(context());
  LoadedNodeAddon loaded_addon;

  // 模拟已卸载的地址和另一个 image 的记录，二者都不能成为该 addon 入口。
  napi_module_register(reinterpret_cast<napi_module*>(1));
  static napi_module unrelated = {};
  unrelated.nm_register_func = [](napi_env, napi_value) -> napi_value {
    ADD_FAILURE() << "Selected a registration from another image";
    return nullptr;
  };
  napi_module_register(&unrelated);
  base::ScopedClosureRunner remove_test_registrations(base::BindOnce([] {
    xenon_napi_remove_registered_module(reinterpret_cast<napi_module*>(1));
    xenon_napi_remove_registered_module(&unrelated);
  }));

  base::FilePath exe_dir;
  ASSERT_TRUE(base::PathService::Get(base::DIR_EXE, &exe_dir));
  auto exports = LoadAndInitializeNodeAddon(
      isolate(), context(), exe_dir.AppendASCII("test_static_addon.node"),
      &loaded_addon);
  ASSERT_FALSE(exports.IsEmpty());
  ASSERT_TRUE(exports->IsObject());
  v8::Local<v8::Value> answer;
  ASSERT_TRUE(
      exports.As<v8::Object>()
          ->Get(context(), v8::String::NewFromUtf8Literal(isolate(), "answer"))
          .ToLocal(&answer));
  ASSERT_TRUE(answer->IsInt32());
  EXPECT_EQ(answer.As<v8::Int32>()->Value(), 42);
}

TEST_F(NapiLoaderTest, StaticRegistrationCanBeDisabled) {
  base::test::ScopedCommandLine command_line;
  command_line.GetProcessCommandLine()->AppendSwitch(
      napi_switches::kDisableNodeStaticRegistration);
  v8::Locker locker(isolate());
  v8::Isolate::Scope isolate_scope(isolate());
  v8::HandleScope handle_scope(isolate());
  v8::Context::Scope context_scope(context());
  LoadedNodeAddon loaded_addon;

  base::FilePath exe_dir;
  ASSERT_TRUE(base::PathService::Get(base::DIR_EXE, &exe_dir));
  base::NativeLibraryLoadError error;
  auto library = LoadNodeAddonLibrary(
      exe_dir.AppendASCII("test_static_addon.node"), &error);
  ASSERT_TRUE(library.is_valid());
  ASSERT_GT(xenon_napi_get_registered_module_count(), 0u);
  auto exports = InitializeLoadedNodeAddon(isolate(), context(),
                                           std::move(library), &loaded_addon);
  EXPECT_TRUE(exports.IsEmpty());
  EXPECT_FALSE(loaded_addon.library.is_valid());
}

TEST_F(NapiLoaderTest,
       NativeCloseCallbackOutlivesEnvironmentAndImageReference) {
  v8::Locker locker(isolate());
  v8::Isolate::Scope isolate_scope(isolate());
  v8::HandleScope handle_scope(isolate());
  v8::Context::Scope context_scope(context());
  base::FilePath exe_dir;
  ASSERT_TRUE(base::PathService::Get(base::DIR_EXE, &exe_dir));
  const auto path = exe_dir.AppendASCII("test_static_addon.node");
  auto loaded_addon = std::make_unique<LoadedNodeAddon>();
  ASSERT_FALSE(
      LoadAndInitializeNodeAddon(isolate(), context(), path, loaded_addon.get())
          .IsEmpty());

  using StartTimer = int (*)(napi_env, uv_loop_t*, int*, int*);
  using GetCloseCallback = uv_close_cb (*)();
  auto start_timer = reinterpret_cast<StartTimer>(
      loaded_addon->library.GetFunctionPointer("XenonTestStartCloseTimer"));
  auto get_close_callback = reinterpret_cast<GetCloseCallback>(
      loaded_addon->library.GetFunctionPointer("XenonTestGetCloseCallback"));
  ASSERT_TRUE(start_timer);
  ASSERT_TRUE(get_close_callback);
  uv_close_cb close_callback = get_close_callback();
  uv_loop_t loop;
  ASSERT_EQ(uv_loop_init(&loop), 0);
  int closed = 0;
  int ticks = 0;
  ASSERT_EQ(start_timer(loaded_addon->env.get(), &loop, &closed, &ticks), 0);

  loaded_addon.reset();
  EXPECT_EQ(closed, 0);
  Dl_info info = {};
  ASSERT_NE(dladdr(reinterpret_cast<void*>(close_callback), &info), 0);
  // 仅测试驱动该独立 loop；宿主析构不运行共享 loop 或未知 JS 回调。
  uv_run(&loop, UV_RUN_NOWAIT);
  EXPECT_EQ(closed, 1);
  EXPECT_EQ(ticks, 0);
  EXPECT_EQ(uv_loop_close(&loop), 0);

  // image 不再重跑 constructor，第二个 env 必须复用仍有效的静态注册记录。
  LoadedNodeAddon second_environment;
  EXPECT_FALSE(LoadAndInitializeNodeAddon(isolate(), context(), path,
                                          &second_environment)
                   .IsEmpty());
}

TEST_F(NapiLoaderTest, NativeCloseCallbackOutlivesFailedInitialization) {
  v8::Locker locker(isolate());
  v8::Isolate::Scope isolate_scope(isolate());
  v8::HandleScope handle_scope(isolate());
  v8::Context::Scope context_scope(context());
  base::FilePath exe_dir;
  ASSERT_TRUE(base::PathService::Get(base::DIR_EXE, &exe_dir));
  base::NativeLibraryLoadError error;
  auto library = LoadNodeAddonLibrary(
      exe_dir.AppendASCII("test_static_addon.node"), &error);
  ASSERT_TRUE(library.is_valid());
  using PrepareFailure = void (*)(uv_loop_t*, int*, int*);
  using GetCloseCallback = uv_close_cb (*)();
  auto prepare_failure = reinterpret_cast<PrepareFailure>(
      library.GetFunctionPointer("XenonTestPrepareInitializationFailure"));
  auto get_close_callback = reinterpret_cast<GetCloseCallback>(
      library.GetFunctionPointer("XenonTestGetCloseCallback"));
  ASSERT_TRUE(prepare_failure);
  ASSERT_TRUE(get_close_callback);
  uv_close_cb close_callback = get_close_callback();
  uv_loop_t loop;
  ASSERT_EQ(uv_loop_init(&loop), 0);
  int closed = 0;
  int ticks = 0;
  prepare_failure(&loop, &closed, &ticks);
  LoadedNodeAddon loaded_addon;
  EXPECT_TRUE(InitializeLoadedNodeAddon(isolate(), context(),
                                        std::move(library), &loaded_addon)
                  .IsEmpty());
  EXPECT_EQ(loaded_addon.env, nullptr);
  EXPECT_EQ(closed, 0);
  Dl_info info = {};
  ASSERT_NE(dladdr(reinterpret_cast<void*>(close_callback), &info), 0);
  uv_run(&loop, UV_RUN_NOWAIT);
  EXPECT_EQ(closed, 1);
  EXPECT_EQ(ticks, 0);
  EXPECT_EQ(uv_loop_close(&loop), 0);
}
#endif

TEST_F(NapiLoaderTest, LoadAndRunTestAddon) {
  v8::Locker locker(isolate());
  v8::Isolate::Scope isolate_scope(isolate());
  v8::HandleScope handle_scope(isolate());
  v8::Context::Scope context_scope(context());

  // 1. Locate the test addon in the executable directory
  base::FilePath exe_dir;
  ASSERT_TRUE(base::PathService::Get(base::DIR_EXE, &exe_dir));
  base::FilePath addon_path =
      exe_dir.Append(FILE_PATH_LITERAL("test_addon.node"));

  // 2. Load the addon
  LoadedNodeAddon loaded_addon;
  v8::Local<v8::Value> exports_val = LoadAndInitializeNodeAddon(
      isolate(), context(), addon_path, &loaded_addon);
  ASSERT_FALSE(exports_val.IsEmpty());
  ASSERT_TRUE(exports_val->IsObject());
  ASSERT_NE(loaded_addon.env, nullptr);
  EXPECT_TRUE(loaded_addon.env->allocated_values.empty());

  v8::Local<v8::Object> exports = exports_val.As<v8::Object>();

  // --- Test Case 1: Synchronous Add ---
  {
    v8::Local<v8::Value> add_val;
    ASSERT_TRUE(
        exports
            ->Get(context(), v8::String::NewFromUtf8(isolate(), "Add",
                                                     v8::NewStringType::kNormal)
                                 .ToLocalChecked())
            .ToLocal(&add_val));
    ASSERT_TRUE(add_val->IsFunction());

    v8::Local<v8::Function> add_fn = add_val.As<v8::Function>();
    v8::Local<v8::Value> argv[2] = {v8::Number::New(isolate(), 15.0),
                                    v8::Number::New(isolate(), 27.0)};

    v8::Local<v8::Value> result;
    ASSERT_TRUE(add_fn->Call(context(), exports, 2, argv).ToLocal(&result));
    ASSERT_TRUE(result->IsNumber());
    EXPECT_DOUBLE_EQ(result.As<v8::Number>()->Value(), 42.0);
  }

  // --- Test Case 2: Asynchronous Add ---
  {
    v8::Local<v8::Value> async_add_val;
    ASSERT_TRUE(exports->Get(context(), v8::String::NewFromUtf8(isolate(), "AsyncAdd", v8::NewStringType::kNormal).ToLocalChecked()).ToLocal(&async_add_val));
    ASSERT_TRUE(async_add_val->IsFunction());

    v8::Local<v8::Function> async_add_fn = async_add_val.As<v8::Function>();

    double sum_received = 0;
    auto cb_data = std::make_unique<double*>(&sum_received);
    
    v8::Local<v8::Function> callback_fn = v8::Function::New(
        context(),
        [](const v8::FunctionCallbackInfo<v8::Value>& info) {
          double** out = static_cast<double**>(
              v8::Local<v8::External>::Cast(info.Data())
                  ->Value(v8::kExternalPointerTypeTagDefault));
          if (info.Length() > 0 && info[0]->IsNumber()) {
            **out = info[0].As<v8::Number>()->Value();
          }
        },
        v8::External::New(isolate(), cb_data.get(),
                          v8::kExternalPointerTypeTagDefault))
        .ToLocalChecked();

    v8::Local<v8::Value> argv[3] = {
        v8::Number::New(isolate(), 10.0),
        v8::Number::New(isolate(), 5.0),
        callback_fn
    };

    v8::Local<v8::Value> dummy_result;
    ASSERT_TRUE(async_add_fn->Call(context(), exports, 3, argv).ToLocal(&dummy_result));

    task_environment_.RunUntilIdle();

    EXPECT_DOUBLE_EQ(sum_received, 15.0);
  }

  // --- Test Case 3: Thread-safe Notification ---
  {
    v8::Local<v8::Value> start_thread_val;
    ASSERT_TRUE(exports->Get(context(), v8::String::NewFromUtf8(isolate(), "StartThread", v8::NewStringType::kNormal).ToLocalChecked()).ToLocal(&start_thread_val));
    ASSERT_TRUE(start_thread_val->IsFunction());

    v8::Local<v8::Function> start_thread_fn = start_thread_val.As<v8::Function>();

    std::string message_received;
    auto cb_data = std::make_unique<std::string*>(&message_received);

    v8::Local<v8::Function> callback_fn = v8::Function::New(
        context(),
        [](const v8::FunctionCallbackInfo<v8::Value>& info) {
          std::string** out = static_cast<std::string**>(
              v8::Local<v8::External>::Cast(info.Data())
                  ->Value(v8::kExternalPointerTypeTagDefault));
          if (info.Length() > 0 && info[0]->IsString()) {
            v8::String::Utf8Value utf8(info.GetIsolate(), info[0]);
            **out = *utf8;
          }
        },
        v8::External::New(isolate(), cb_data.get(),
                          v8::kExternalPointerTypeTagDefault))
        .ToLocalChecked();

    v8::Local<v8::Value> argv[1] = { callback_fn };
    v8::Local<v8::Value> dummy_result;
    ASSERT_TRUE(start_thread_fn->Call(context(), exports, 1, argv).ToLocal(&dummy_result));

    // Wait until the message is received, up to 500ms
    int attempts = 0;
    while (message_received.empty() && attempts < 100) {
      task_environment_.FastForwardBy(base::Milliseconds(5));
      base::PlatformThread::Sleep(base::Milliseconds(5));
      attempts++;
    }

    EXPECT_EQ(message_received, "Hello from Thread!");
  }
}

}  // namespace xenon
