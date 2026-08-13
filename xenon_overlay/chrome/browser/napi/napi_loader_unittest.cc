// Copyright 2026 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "napi_loader.h"

#include "base/base_paths.h"
#include "base/path_service.h"
#include "base/test/task_environment.h"
#include "build/build_config.h"
#include "js_native_api_v8.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "v8/include/libplatform/libplatform.h"
#include "v8/include/v8.h"

namespace xenon {

namespace {

napi_value CreateTemporaryValues(napi_env env, napi_callback_info info) {
  napi_value unused;
  napi_value result;
  EXPECT_EQ(napi_create_int32(env, 1, &unused), napi_ok);
  EXPECT_EQ(napi_create_int32(env, 2, &result), napi_ok);
  return result;
}

void MarkFinalized(napi_env env, void* data, void* hint) {
  *static_cast<bool*>(data) = true;
}

void CountThreadsafeCall(napi_env env,
                         napi_value callback,
                         void* context,
                         void* data) {
  ++*static_cast<int*>(context);
}

}  // namespace

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

TEST_F(NapiLoaderTest, ReferenceTransitionsBetweenWeakAndStrong) {
  v8::Locker locker(isolate());
  v8::Isolate::Scope isolate_scope(isolate());
  v8::HandleScope handle_scope(isolate());
  v8::Context::Scope context_scope(context());

  auto env = std::make_unique<napi_env__>(isolate(), context());
  napi_value object;
  ASSERT_EQ(napi_create_object(env.get(), &object), napi_ok);
  napi_ref reference;
  ASSERT_EQ(napi_create_reference(env.get(), object, 0, &reference), napi_ok);
  EXPECT_TRUE(reference->is_weak);

  uint32_t ref_count = 0;
  EXPECT_EQ(napi_reference_ref(env.get(), reference, &ref_count), napi_ok);
  EXPECT_EQ(ref_count, 1u);
  EXPECT_FALSE(reference->is_weak);
  EXPECT_EQ(napi_reference_unref(env.get(), reference, &ref_count), napi_ok);
  EXPECT_EQ(ref_count, 0u);
  EXPECT_TRUE(reference->is_weak);
  EXPECT_EQ(napi_delete_reference(env.get(), reference), napi_ok);
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
  EXPECT_FALSE(is_buffer);
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
    ASSERT_TRUE(exports->Get(context(), v8::String::NewFromUtf8(isolate(), "Add", v8::NewStringType::kNormal).ToLocalChecked()).ToLocal(&add_val));
    ASSERT_TRUE(add_val->IsFunction());

    v8::Local<v8::Function> add_fn = add_val.As<v8::Function>();
    v8::Local<v8::Value> argv[2] = {
        v8::Number::New(isolate(), 15.0),
        v8::Number::New(isolate(), 27.0)
    };
    
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
