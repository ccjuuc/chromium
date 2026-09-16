// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/services/xenon_node_executor.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/base_paths.h"
#include "base/functional/bind.h"
#include "base/path_service.h"
#include "base/task/execution_fence.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "gin/function_template.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace xenon {

class XenonNodeExecutorTestPeer {
 public:
  static void RegisterCollectibleCallback(XenonNodeExecutor& executor,
                                          int32_t client_id,
                                          int32_t callback_id) {
    v8::Locker locker(executor.addon_isolate_);
    v8::Isolate::Scope isolate_scope(executor.addon_isolate_);
    v8::HandleScope handle_scope(executor.addon_isolate_);
    // Gin FunctionTemplates are cached for their context's lifetime. A
    // short-lived context lets real V8 GC collect the registered function.
    auto context = v8::Context::New(executor.addon_isolate_);
    v8::Context::Scope context_scope(context);
    executor.GetOrCreateNativeCallback(context, client_id, callback_id, "");
  }

  static void ForceGarbageCollection(XenonNodeExecutor& executor) {
    v8::Locker locker(executor.addon_isolate_);
    v8::Isolate::Scope isolate_scope(executor.addon_isolate_);
    v8::HandleScope handle_scope(executor.addon_isolate_);
    auto context = executor.addon_context_.Get(executor.addon_isolate_);
    v8::Context::Scope context_scope(context);
    // The registration helper has already left its HandleScope. Ignore stale
    // words in that returned C++ stack frame instead of conservatively keeping
    // the otherwise unreachable function alive.
    executor.addon_isolate_->RequestGarbageCollectionForTesting(
        v8::Isolate::kFullGarbageCollection, v8::StackState::kNoHeapPointers);
  }

  static bool IsCollected(const XenonNodeExecutor& executor,
                          int32_t client_id,
                          int32_t callback_id) {
    const auto callback =
        executor.addon_callbacks_.find({client_id, callback_id});
    return callback != executor.addon_callbacks_.end() &&
           callback->second->function.IsEmpty();
  }

  static size_t CallbackCount(const XenonNodeExecutor& executor) {
    return executor.addon_callbacks_.size();
  }

  static size_t InstanceCount(const XenonNodeExecutor& executor,
                              uint64_t owner) {
    size_t count = 0;
    for (const auto& [id, instance] : executor.addon_instances_) {
      count += instance.owner == owner;
    }
    return count;
  }

  static size_t InstanceIndexSize(const XenonNodeExecutor& executor) {
    return executor.addon_instance_ids_by_hash_.size();
  }

  static size_t PromiseCount(const XenonNodeExecutor& executor) {
    return executor.pending_promises_.size() +
           executor.deferred_promises_.size();
  }

  static bool InstallOwnerReentryFixture(XenonNodeExecutor& executor,
                                         const std::string& module_path) {
    v8::Locker locker(executor.addon_isolate_);
    v8::Isolate::Scope isolate_scope(executor.addon_isolate_);
    v8::HandleScope handle_scope(executor.addon_isolate_);
    auto context = executor.addon_context_.Get(executor.addon_isolate_);
    v8::Context::Scope context_scope(context);
    auto close = gin::CreateFunctionTemplate(
                     executor.addon_isolate_,
                     base::BindRepeating(
                         &XenonNodeExecutor::ReleaseInstanceOwner,
                         executor.weak_factory_.GetWeakPtr(), uint64_t{1}))
                     ->GetFunction(context)
                     .ToLocalChecked();
    static constexpr char source[] = R"JS(
      (function(exports, closeOwner) {
        let constructors = 0, secondCalls = 0;
        const shared = { read() { return 42; } };
        exports.SharedOwnerObject = () => shared;
        Object.defineProperty(exports, 'OwnerClosingConstructor', {
          get() { closeOwner(); return function() { ++constructors; }; }
        });
        exports.OwnerConstructorCalls = () => constructors;
        exports.OwnerChainCalls = () => secondCalls;
        exports.MakeOwnerClosingChain = () => ({ first() {
          closeOwner();
          return { second() { ++secondCalls; return { read() { return 1; } }; } };
        } });
        exports.InvokeOwnerClosingPayload = (callback) => {
          callback({ get value() { closeOwner(); return 42; } });
        };
        exports.InvokeOwnerClosingReceiver = (callback) => {
          callback.call({ get value() { closeOwner(); return 42; } });
        };
        let accessorReads = 0;
        class AccessorReceiver {
          get dangerous() { ++accessorReads; throw new Error('must stay lazy'); }
          get count() { ++accessorReads; return 99; }
          read() { return 42; }
        }
        AccessorReceiver.prototype.shadowed = 'inherited';
        exports.InvokeAccessorReceiver = callback => {
          const receiver = new AccessorReceiver();
          receiver.lastID = 11;
          receiver.changes = 1;
          receiver.shadowed = undefined;
          callback.call(receiver, null);
          return receiver;
        };
        exports.AccessorReceiverReads = () => accessorReads;
      })
    )JS";
    v8::Local<v8::Script> script;
    v8::Local<v8::Value> install;
    if (!v8::Script::Compile(
             context, v8::String::NewFromUtf8(executor.addon_isolate_, source)
                          .ToLocalChecked())
             .ToLocal(&script) ||
        !script->Run(context).ToLocal(&install)) {
      return false;
    }
    v8::Local<v8::Value> args[] = {
        executor.FindModule(module_path)->exports.Get(executor.addon_isolate_),
        close};
    return !install.As<v8::Function>()
                ->Call(context, v8::Undefined(executor.addon_isolate_), 2, args)
                .IsEmpty();
  }

  static bool InstallMetadataAccessors(XenonNodeExecutor& executor,
                                       const std::string& module_path) {
    v8::Locker locker(executor.addon_isolate_);
    v8::Isolate::Scope isolate_scope(executor.addon_isolate_);
    v8::HandleScope handle_scope(executor.addon_isolate_);
    auto context = executor.addon_context_.Get(executor.addon_isolate_);
    v8::Context::Scope context_scope(context);
    static constexpr char source[] = R"JS(
      (function(exports) {
        let reads = 0;
        const descriptor = { configurable: true, get() { ++reads; return 42; } };
        Object.defineProperty(exports.PrototypeCallback.prototype, 'hiddenState', descriptor);
        Object.defineProperty(exports.PrototypeCallback, 'hiddenStatic', descriptor);
        exports.MetadataFunction = function() {}.bind(null);
        Object.defineProperty(exports.MetadataFunction, 'prototype', descriptor);
        exports.MetadataGetterReads = () => reads;
      })
    )JS";
    v8::Local<v8::Script> script;
    v8::Local<v8::Value> install;
    if (!v8::Script::Compile(
             context, v8::String::NewFromUtf8(executor.addon_isolate_, source)
                          .ToLocalChecked())
             .ToLocal(&script) ||
        !script->Run(context).ToLocal(&install)) {
      return false;
    }
    v8::Local<v8::Value> args[] = {
        executor.FindModule(module_path)->exports.Get(executor.addon_isolate_)};
    return !install.As<v8::Function>()
                ->Call(context, v8::Undefined(executor.addon_isolate_), 1, args)
                .IsEmpty();
  }
};

namespace {

using InvokeFuture =
    base::test::TestFuture<bool,
                           base::Value,
                           std::vector<mojom::NodeCallbackResultPtr>,
                           const std::string&>;
using LoadFuture = base::test::
    TestFuture<bool, const std::string&, std::vector<mojom::NodeExportInfoPtr>>;

const mojom::NodeExportInfo* FindExportChild(
    const std::vector<mojom::NodeExportInfoPtr>& children,
    const std::string& name) {
  for (const auto& child : children) {
    if (child->name == name) {
      return child.get();
    }
  }
  return nullptr;
}

const mojom::NodeExportInfo* FindExportChild(
    const mojom::NodeExportInfo& description,
    const std::string& name) {
  return FindExportChild(description.children, name);
}

base::Value BinaryWire(std::string kind, base::Value::BlobStorage bytes = {}) {
  return base::Value(base::DictValue()
                         .Set("__xenon_node_wire_type__", "binary")
                         .Set("kind", std::move(kind))
                         .Set("value", base::Value(std::move(bytes))));
}

class XenonNodePromiseTest : public testing::Test {
 protected:
  void SetUp() override {
    // Match gin::V8Test: later tests in this process may adjust V8 flags.
    v8::V8::SetFlagsFromString("--no-freeze-flags-after-init --expose-gc");
    base::FilePath executable_dir;
    ASSERT_TRUE(base::PathService::Get(base::DIR_EXE, &executable_dir));
    addon_path_ = executable_dir.AppendASCII("test_addon.node").AsUTF8Unsafe();
    executor_ = std::make_unique<XenonNodeExecutor>();
    std::string error;
    ASSERT_TRUE(executor_->LoadAddonFromCurrentThread(addon_path_, &error))
        << error;
    executor_->RegisterInstanceOwner(1);
    executor_->RegisterInstanceOwner(2);
  }

  uint64_t Begin(uint64_t owner = 1) {
    base::test::TestFuture<uint64_t> deferred;
    InvokeFuture unexpected_result;
    executor_->InvokeFunction(addon_path_, "BeginControlledPromise", 0, {},
                              unexpected_result.GetCallback(), false,
                              deferred.GetCallback(), owner);
    EXPECT_FALSE(unexpected_result.IsReady());
    return deferred.Get();
  }

  base::Value CallSync(const std::string& name, base::ListValue args = {}) {
    base::Value result;
    std::string error;
    EXPECT_TRUE(executor_->InvokeExportFromCurrentThread(
        addon_path_, name, base::Value(std::move(args)), &result, &error))
        << error;
    return result;
  }

  int Calls() { return CallSync("ControlledPromiseCallCount").GetInt(); }

  std::vector<mojom::NodeInvokeArgPtr> ValueArgs(base::ListValue values) {
    std::vector<mojom::NodeInvokeArgPtr> args;
    for (auto& value : values) {
      auto arg = mojom::NodeInvokeArg::New();
      arg->value = std::move(value);
      args.push_back(std::move(arg));
    }
    return args;
  }

  base::Value CallOwned(const std::string& name,
                        uint64_t owner,
                        base::ListValue args = {}) {
    InvokeFuture result;
    executor_->InvokeFunction(addon_path_, name, 0, ValueArgs(std::move(args)),
                              result.GetCallback(), false, {}, owner);
    EXPECT_TRUE(result.Get<0>()) << result.Get<3>();
    return result.Get<1>().Clone();
  }

  int32_t ConstructOwned(uint64_t owner) {
    base::test::TestFuture<bool, int32_t, const std::string&> result;
    executor_->ConstructExport(addon_path_, "OwnedHandle", 0, {},
                               result.GetCallback(), owner);
    EXPECT_TRUE(result.Get<0>()) << result.Get<2>();
    return result.Get<1>();
  }

  void SettleWithHandles(int operation) {
    base::ListValue args;
    args.Append(operation);
    CallSync("SettleControlledPromiseWithHandles", std::move(args));
    task_environment_.FastForwardBy(base::Milliseconds(10));
  }

  void Settle(int operation, bool reject = false) {
    base::ListValue args;
    args.Append(operation);
    args.Append(reject);
    CallSync("SettleControlledPromise", std::move(args));
    task_environment_.FastForwardBy(base::Milliseconds(10));
  }

  base::test::TaskEnvironment task_environment_{
      base::test::TaskEnvironment::MainThreadType::IO,
      base::test::TaskEnvironment::TimeSource::MOCK_TIME};
  std::unique_ptr<XenonNodeExecutor> executor_;
  std::string addon_path_;
};

TEST_F(XenonNodePromiseTest, BinaryWirePreservesEveryViewKindAndEmptyValues) {
  const char* kinds[] = {
      "Buffer",       "ArrayBuffer",       "DataView",       "Int8Array",
      "Uint8Array",   "Uint8ClampedArray", "Int16Array",     "Uint16Array",
      "Int32Array",   "Uint32Array",       "Float16Array",   "Float32Array",
      "Float64Array", "BigInt64Array",     "BigUint64Array",
  };
  for (const char* kind : kinds) {
    SCOPED_TRACE(kind);
    for (bool empty : {false, true}) {
      const base::Value::BlobStorage bytes =
          empty ? base::Value::BlobStorage()
                : base::Value::BlobStorage{0, 1, 2, 3, 128, 129, 254, 255};
      auto binary = BinaryWire(kind, bytes);
      base::ListValue args;
      args.Append(binary.Clone());
      EXPECT_EQ(binary, CallSync("EchoOwnedHandle", std::move(args)));

      base::ListValue view_args;
      view_args.Append(binary.Clone());
      auto result = CallSync("BinaryEcho", std::move(view_args));
      ASSERT_TRUE(result.is_dict());
      EXPECT_EQ(result.GetDict().FindInt("byteLength"),
                static_cast<int>(bytes.size()));
      EXPECT_EQ(result.GetDict().FindInt("checksum"), empty ? 0 : 772);
      EXPECT_EQ(result.GetDict().FindBool("isBuffer"),
                std::string_view(kind) != "ArrayBuffer");
      const auto* copy = result.GetDict().Find("copy");
      ASSERT_TRUE(copy);
      EXPECT_EQ(*copy, BinaryWire("Buffer", bytes));
    }
  }
}

TEST_F(XenonNodePromiseTest, NestedBinaryWireAndLegacyRawBytesRoundTrip) {
  base::ListValue items;
  items.Append(BinaryWire("Uint16Array", {0, 128, 254, 255}));
  items.Append(BinaryWire("Buffer", {0, 1, 255}));
  items.Append(BinaryWire("DataView"));
  base::Value nested(base::DictValue().Set("payload", std::move(items)));
  base::ListValue args;
  args.Append(nested.Clone());
  EXPECT_EQ(nested, CallOwned("EchoOwnedHandle", 1, std::move(args)));

  base::ListValue legacy_args;
  legacy_args.Append(base::Value(base::Value::BlobStorage{1, 2, 255}));
  EXPECT_EQ(BinaryWire("ArrayBuffer", {1, 2, 255}),
            CallSync("EchoOwnedHandle", std::move(legacy_args)));
}

TEST_F(XenonNodePromiseTest,
       BinarySubviewsKeepActiveBytesInReturnsAndCallbacks) {
  base::Value expected(
      base::DictValue()
          .Set("uint8", BinaryWire("Uint8Array", {2, 3, 4}))
          .Set("uint16", BinaryWire("Uint16Array", {2, 3, 4, 5}))
          .Set("dataView", BinaryWire("DataView", {3, 4, 5}))
          .Set("empty", BinaryWire("Uint8Array"))
          .Set("buffer", BinaryWire("Buffer", {2, 3, 4})));
  EXPECT_EQ(expected, CallSync("BinarySubViews"));
  std::vector<base::Value> callback_args;
  executor_->SetCallbackHandlers(
      base::BindRepeating(
          [](std::vector<base::Value>* output, int32_t client_id,
             int32_t callback_id, std::vector<base::Value> args, base::Value) {
            EXPECT_EQ(-11, client_id);
            EXPECT_EQ(7, callback_id);
            *output = std::move(args);
          },
          &callback_args),
      {});
  std::vector<mojom::NodeInvokeArgPtr> args;
  auto callback = mojom::NodeInvokeArg::New();
  callback->is_callback = true;
  callback->callback_id = 7;
  args.push_back(std::move(callback));
  InvokeFuture result;
  executor_->InvokeFunction(addon_path_, "BinarySubViews", -11, std::move(args),
                            result.GetCallback(), false, {}, 1);
  ASSERT_TRUE(result.Get<0>()) << result.Get<3>();
  EXPECT_EQ(expected, result.Get<1>());
  ASSERT_EQ(1u, callback_args.size());
  EXPECT_EQ(expected, callback_args[0]);
}

TEST_F(XenonNodePromiseTest, MalformedBinaryWireFailsBeforeNativeInvocation) {
  base::ListValue invalid;
  invalid.Append(BinaryWire("SharedArrayBuffer", {1}));
  invalid.Append(BinaryWire("UnknownTypedArray", {1}));
  invalid.Append(BinaryWire("Uint16Array", {1}));
  invalid.Append(BinaryWire("Float64Array", {1, 2, 3, 4}));
  invalid.Append(base::DictValue()
                     .Set("__xenon_node_wire_type__", "binary")
                     .Set("value", base::Value(base::Value::BlobStorage{1})));
  invalid.Append(base::DictValue()
                     .Set("__xenon_node_wire_type__", "binary")
                     .Set("kind", "Buffer")
                     .Set("value", base::ListValue().Append(1)));
  for (auto& value : invalid) {
    base::ListValue args;
    args.Append(std::move(value));
    base::Value result;
    std::string error;
    EXPECT_FALSE(executor_->InvokeExportFromCurrentThread(
        addon_path_, "EchoOwnedHandle", base::Value(std::move(args)), &result,
        &error));
    EXPECT_NE(error.find("binary"), std::string::npos) << error;
  }
}

TEST_F(XenonNodePromiseTest,
       PendingInvocationExecutesOnceAndPreservesSyncCalls) {
  const uint64_t token = Begin();
  EXPECT_EQ(1, Calls());
  InvokeFuture result;
  executor_->AwaitDeferredPromise(token, 1, result.GetCallback());
  EXPECT_FALSE(result.IsReady());
  Settle(1);
  ASSERT_TRUE(result.Get<0>()) << result.Get<3>();
  EXPECT_EQ(1, result.Get<1>().GetInt());
  EXPECT_EQ(1, Calls());
  base::ListValue args;
  args.Append(20);
  args.Append(22);
  EXPECT_EQ(42, CallSync("Add", std::move(args)).GetInt());
}

TEST_F(XenonNodePromiseTest, SettlementBeforeSubscribeAndConcurrentOwners) {
  const uint64_t first = Begin(1);
  const uint64_t second = Begin(2);
  Settle(2);
  InvokeFuture wrong_owner;
  executor_->AwaitDeferredPromise(second, 1, wrong_owner.GetCallback());
  EXPECT_FALSE(wrong_owner.Get<0>());
  InvokeFuture second_result;
  executor_->AwaitDeferredPromise(second, 2, second_result.GetCallback());
  ASSERT_TRUE(second_result.Get<0>());
  EXPECT_EQ(2, second_result.Get<1>().GetInt());
  InvokeFuture first_result;
  executor_->AwaitDeferredPromise(first, 1, first_result.GetCallback());
  Settle(1);
  ASSERT_TRUE(first_result.Get<0>());
  EXPECT_EQ(1, first_result.Get<1>().GetInt());
  InvokeFuture duplicate;
  executor_->AwaitDeferredPromise(first, 1, duplicate.GetCallback());
  EXPECT_FALSE(duplicate.Get<0>());
  EXPECT_EQ(2, Calls());
}

TEST_F(XenonNodePromiseTest, RejectionBeforeAndAfterSubscribe) {
  const uint64_t first = Begin();
  Settle(1, true);
  InvokeFuture first_result;
  executor_->AwaitDeferredPromise(first, 1, first_result.GetCallback());
  EXPECT_FALSE(first_result.Get<0>());
  EXPECT_NE(first_result.Get<3>().find("rejected: 1"), std::string::npos);
  const uint64_t second = Begin();
  InvokeFuture second_result;
  executor_->AwaitDeferredPromise(second, 1, second_result.GetCallback());
  Settle(2, true);
  EXPECT_FALSE(second_result.Get<0>());
  EXPECT_NE(second_result.Get<3>().find("rejected: 2"), std::string::npos);
  EXPECT_EQ(2, Calls());
}

TEST_F(XenonNodePromiseTest, DisconnectCancelsOnlyOwningConnection) {
  const uint64_t waiting = Begin(1);
  const uint64_t unclaimed = Begin(1);
  const uint64_t other = Begin(2);
  InvokeFuture cancelled;
  executor_->AwaitDeferredPromise(waiting, 1, cancelled.GetCallback());
  executor_->CancelPromisesForOwner(1);
  EXPECT_FALSE(cancelled.Get<0>());
  InvokeFuture missing;
  executor_->AwaitDeferredPromise(unclaimed, 1, missing.GetCallback());
  EXPECT_FALSE(missing.Get<0>());
  InvokeFuture live;
  executor_->AwaitDeferredPromise(other, 2, live.GetCallback());
  Settle(1);
  Settle(2);
  Settle(3);
  ASSERT_TRUE(live.Get<0>());
  EXPECT_EQ(3, live.Get<1>().GetInt());
}

TEST_F(XenonNodePromiseTest, UnclaimedTokenExpiresAndDestructionRejectsWaiter) {
  const uint64_t unclaimed = Begin();
  task_environment_.FastForwardBy(base::Seconds(31));
  InvokeFuture expired;
  executor_->AwaitDeferredPromise(unclaimed, 1, expired.GetCallback());
  EXPECT_FALSE(expired.Get<0>());
  const uint64_t waiting = Begin();
  InvokeFuture result;
  executor_->AwaitDeferredPromise(waiting, 1, result.GetCallback());
  executor_.reset();
  EXPECT_FALSE(result.Get<0>());
  EXPECT_EQ("Executor destroyed", result.Get<3>());
}

TEST_F(XenonNodePromiseTest, FullTableRejectsBeforeExecutingNativeCode) {
  for (int i = 0; i < 1024; ++i) {
    ASSERT_NE(0u, Begin());
  }
  InvokeFuture rejected;
  base::test::TestFuture<uint64_t> unexpected_token;
  executor_->InvokeFunction(addon_path_, "BeginControlledPromise", 0, {},
                            rejected.GetCallback(), false,
                            unexpected_token.GetCallback(), 1);
  EXPECT_FALSE(rejected.Get<0>());
  EXPECT_FALSE(unexpected_token.IsReady());
  EXPECT_EQ(1024, Calls());
  executor_->CancelPromisesForOwner(1);
  EXPECT_NE(0u, Begin(2));
  EXPECT_EQ(1025, Calls());
}

TEST_F(XenonNodePromiseTest, InspectsRealRootOwnMembersAndMissingExport) {
  mojom::NodeExportInfoPtr root;
  std::string error;
  ASSERT_TRUE(
      executor_->InspectExportFromCurrentThread(addon_path_, "", &root, &error))
      << error;
  ASSERT_TRUE(root);
  EXPECT_EQ("", root->name);
  EXPECT_EQ("object", root->kind);
  EXPECT_FALSE(root->has_value);
  const auto* add = FindExportChild(*root, "Add");
  ASSERT_TRUE(add);
  EXPECT_EQ("function", add->kind);
  EXPECT_FALSE(add->enumerable);
  EXPECT_FALSE(add->writable);
  EXPECT_TRUE(FindExportChild(*root, "BeginControlledPromise"));
  EXPECT_FALSE(FindExportChild(*root, "toString"));
  EXPECT_FALSE(FindExportChild(*root, "default"));

  mojom::NodeExportInfoPtr missing;
  ASSERT_TRUE(executor_->InspectExportFromCurrentThread(
      addon_path_, "NotAnExport", &missing, &error))
      << error;
  ASSERT_TRUE(missing);
  EXPECT_EQ("undefined", missing->kind);
  EXPECT_TRUE(missing->children.empty());
  EXPECT_EQ(0, Calls());
}

TEST_F(XenonNodePromiseTest, InspectsRealFunctionPropertiesAndNestedValues) {
  mojom::NodeExportInfoPtr function;
  std::string error;
  ASSERT_TRUE(executor_->InspectExportFromCurrentThread(addon_path_, "Add",
                                                        &function, &error))
      << error;
  ASSERT_TRUE(function);
  EXPECT_EQ("function", function->kind);
  const auto* name = FindExportChild(*function, "name");
  ASSERT_TRUE(name);
  EXPECT_EQ("string", name->kind);
  EXPECT_TRUE(name->has_value);
  EXPECT_TRUE(name->value.is_string());
  const auto* length = FindExportChild(*function, "length");
  ASSERT_TRUE(length);
  EXPECT_EQ("number", length->kind);
  ASSERT_TRUE(length->has_value);
  EXPECT_EQ(0, length->value.GetInt());
  EXPECT_FALSE(length->enumerable);
  EXPECT_FALSE(length->writable);

  mojom::NodeExportInfoPtr nested;
  ASSERT_TRUE(executor_->InspectExportFromCurrentThread(
      addon_path_, "Add.length", &nested, &error))
      << error;
  ASSERT_TRUE(nested);
  EXPECT_EQ("number", nested->kind);
  EXPECT_EQ(length->value, nested->value);
  ASSERT_TRUE(executor_->InspectExportFromCurrentThread(
      addon_path_, "Add.prototype", &nested, &error))
      << error;
  ASSERT_TRUE(nested);
  EXPECT_EQ("object", nested->kind);
  const auto* constructor = FindExportChild(*nested, "constructor");
  ASSERT_TRUE(constructor);
  EXPECT_EQ("function", constructor->kind);
  EXPECT_FALSE(constructor->enumerable);
}

TEST_F(XenonNodePromiseTest,
       ExactClassMetadataIncludesNonEnumerableNativeMethods) {
  std::string error;
  mojom::NodeExportInfoPtr root;
  ASSERT_TRUE(
      executor_->InspectExportFromCurrentThread(addon_path_, "", &root, &error))
      << error;
  for (const char* name :
       {"PrototypeCallback", "AsyncPrototypeCallback", "OwnedHandle"}) {
    const auto* child = FindExportChild(*root, name);
    ASSERT_TRUE(child) << name;
    EXPECT_EQ("class", child->kind) << name;
    mojom::NodeExportInfoPtr info;
    ASSERT_TRUE(executor_->InspectExportFromCurrentThread(addon_path_, name,
                                                          &info, &error))
        << error;
    EXPECT_EQ("class", info->kind) << name;
    const auto* read = FindExportChild(info->prototype, "read");
    ASSERT_TRUE(read) << name;
    EXPECT_EQ("function", read->kind);
    EXPECT_FALSE(read->enumerable);
    EXPECT_FALSE(read->writable);
    base::test::TestFuture<bool, const std::string&, mojom::NodeExportInfoPtr>
        legacy;
    executor_->InspectExport(addon_path_, name, legacy.GetCallback());
    ASSERT_TRUE(legacy.Get<0>()) << legacy.Get<1>();
    EXPECT_EQ("class", legacy.Get<2>()->kind);
    ASSERT_TRUE(FindExportChild(legacy.Get<2>()->prototype, "read"));
  }
  const auto* add = FindExportChild(*root, "Add");
  ASSERT_TRUE(add);
  EXPECT_EQ("function", add->kind);
}

TEST_F(XenonNodePromiseTest,
       ClassMetadataDoesNotEvaluatePrototypeOrStaticAccessors) {
  ASSERT_TRUE(XenonNodeExecutorTestPeer::InstallMetadataAccessors(*executor_,
                                                                  addon_path_));
  mojom::NodeExportInfoPtr info;
  std::string error;
  ASSERT_TRUE(executor_->InspectExportFromCurrentThread(
      addon_path_, "PrototypeCallback", &info, &error))
      << error;
  EXPECT_EQ("class", info->kind);
  const auto* state = FindExportChild(info->prototype, "hiddenState");
  ASSERT_TRUE(state);
  EXPECT_EQ("property", state->kind);
  EXPECT_FALSE(state->enumerable);
  const auto* hidden_static = FindExportChild(*info, "hiddenStatic");
  ASSERT_TRUE(hidden_static);
  EXPECT_EQ("property", hidden_static->kind);
  EXPECT_FALSE(hidden_static->enumerable);
  base::test::TestFuture<bool, const std::string&, mojom::NodeExportInfoPtr>
      legacy;
  executor_->InspectExport(addon_path_, "PrototypeCallback",
                           legacy.GetCallback());
  ASSERT_TRUE(legacy.Get<0>()) << legacy.Get<1>();
  EXPECT_EQ("class", legacy.Get<2>()->kind);
  ASSERT_TRUE(executor_->InspectExportFromCurrentThread(
      addon_path_, "MetadataFunction", &info, &error))
      << error;
  EXPECT_EQ("function", info->kind);
  EXPECT_TRUE(info->prototype.empty());
  base::test::TestFuture<bool, const std::string&, mojom::NodeExportInfoPtr>
      function;
  executor_->InspectExport(addon_path_, "MetadataFunction",
                           function.GetCallback());
  ASSERT_TRUE(function.Get<0>()) << function.Get<1>();
  EXPECT_EQ("function", function.Get<2>()->kind);
  EXPECT_TRUE(function.Get<2>()->prototype.empty());
  EXPECT_EQ(0, CallSync("MetadataGetterReads").GetInt());
}

TEST_F(XenonNodePromiseTest, NativePrototypeMethodsKeepBuiltinLikeNames) {
  std::string error;
  mojom::NodeExportInfoPtr exact;
  ASSERT_TRUE(executor_->InspectExportFromCurrentThread(
      addon_path_, "NamedPrototypeMethods", &exact, &error))
      << error;
  base::test::TestFuture<bool, const std::string&, mojom::NodeExportInfoPtr>
      legacy;
  executor_->InspectExport(addon_path_, "NamedPrototypeMethods",
                           legacy.GetCallback());
  ASSERT_TRUE(legacy.Get<0>()) << legacy.Get<1>();
  for (const auto* description : {exact.get(), legacy.Get<2>().get()}) {
    EXPECT_EQ("class", description->kind);
    for (const char* name :
         {"bind", "call", "apply", "toString", "valueOf", "name", "length"}) {
      const auto* method = FindExportChild(description->prototype, name);
      ASSERT_TRUE(method) << name;
      EXPECT_EQ("function", method->kind) << name;
      EXPECT_FALSE(method->enumerable) << name;
    }
    EXPECT_FALSE(FindExportChild(description->prototype, "hasOwnProperty"));
    EXPECT_FALSE(FindExportChild(description->prototype, "constructor"));
  }
  base::test::TestFuture<bool, int32_t, const std::string&> constructed;
  executor_->ConstructExport(addon_path_, "NamedPrototypeMethods", 0, {},
                             constructed.GetCallback(), 1);
  ASSERT_TRUE(constructed.Get<0>()) << constructed.Get<2>();
  InvokeFuture invoked;
  executor_->InvokeInstance(addon_path_, constructed.Get<1>(), "bind", 0, {},
                            invoked.GetCallback(), false, {}, 1);
  ASSERT_TRUE(invoked.Get<0>()) << invoked.Get<3>();
  EXPECT_EQ(42, invoked.Get<1>().GetInt());
}

TEST_F(XenonNodePromiseTest,
       ReleasedClientCallbacksSkipSerializationAndCannotRebind) {
  std::vector<int32_t> delivered_clients;
  executor_->SetCallbackHandlers(
      base::BindRepeating(
          [](std::vector<int32_t>* clients, int32_t client_id,
             int32_t callback_id, std::vector<base::Value> args, base::Value) {
            EXPECT_EQ(7, callback_id);
            ASSERT_EQ(1u, args.size());
            ASSERT_TRUE(args[0].is_dict());
            EXPECT_TRUE(args[0].GetDict().FindInt("value"));
            clients->push_back(client_id);
          },
          &delivered_clients),
      {});
  const auto retain = [&](int slot, int32_t client_id) {
    std::vector<mojom::NodeInvokeArgPtr> args;
    auto slot_arg = mojom::NodeInvokeArg::New();
    slot_arg->value = base::Value(slot);
    args.push_back(std::move(slot_arg));
    auto callback_arg = mojom::NodeInvokeArg::New();
    callback_arg->is_callback = true;
    callback_arg->callback_id = 7;
    args.push_back(std::move(callback_arg));
    InvokeFuture result;
    executor_->InvokeFunction(addon_path_, "RetainCallback", client_id,
                              std::move(args), result.GetCallback());
    EXPECT_TRUE(result.Get<0>()) << result.Get<3>();
  };
  const auto call = [&](int slot) {
    base::ListValue args;
    args.Append(slot);
    return CallSync("CallRetainedCallback", std::move(args)).GetInt();
  };
  retain(1, -11);
  retain(2, 22);
  EXPECT_EQ(1, call(1));
  EXPECT_EQ(2, call(2));
  executor_->ReleaseCallbacksForClient(-11);
  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ(2, call(1));
  }
  EXPECT_EQ(3, call(2));
  retain(3, -11);
  EXPECT_EQ(3, call(1));
  EXPECT_EQ(4, call(3));
  executor_->ReleaseCallbacksForClient(-11);
  executor_->ReleaseCallbacksForClient(-11);
  EXPECT_EQ(4, call(3));
  EXPECT_EQ(5, call(2));
  EXPECT_EQ((std::vector<int32_t>{-11, 22, 22, -11, 22}), delivered_clients);
}

TEST_F(XenonNodePromiseTest, CallbackReceiverKeepsConstructedInstanceIdentity) {
  base::Value received_receiver;
  std::vector<base::Value> received_args;
  executor_->SetCallbackHandlers(
      base::BindRepeating(
          [](base::Value* receiver, std::vector<base::Value>* output,
             int32_t client_id, int32_t callback_id,
             std::vector<base::Value> args, base::Value value) {
            EXPECT_EQ(-11, client_id);
            EXPECT_EQ(7, callback_id);
            *receiver = std::move(value);
            *output = std::move(args);
          },
          &received_receiver, &received_args),
      {});
  const auto callback_arg = [] {
    auto arg = mojom::NodeInvokeArg::New();
    arg->is_callback = true;
    arg->callback_id = 7;
    return arg;
  };
  std::vector<mojom::NodeInvokeArgPtr> constructor_args;
  constructor_args.push_back(callback_arg());
  base::test::TestFuture<bool, int32_t, const std::string&> constructed;
  executor_->ConstructExport(addon_path_, "OwnedHandle", -11,
                             std::move(constructor_args),
                             constructed.GetCallback(), 1);
  ASSERT_TRUE(constructed.Get<0>()) << constructed.Get<2>();
  ASSERT_TRUE(received_receiver.is_dict());
  const int32_t id = constructed.Get<1>();
  EXPECT_EQ(id, received_receiver.GetDict().FindInt("instance_id"));
  EXPECT_EQ(executor_->GetInstanceOwnerToken(1),
            *received_receiver.GetDict().FindString("owner_token"));
  ASSERT_EQ(1u, received_args.size());
  EXPECT_EQ(received_receiver, received_args[0]);
  EXPECT_EQ(2u, XenonNodeExecutorTestPeer::InstanceCount(*executor_, 1));

  for (int iteration = 0; iteration < 3; ++iteration) {
    std::vector<mojom::NodeInvokeArgPtr> args;
    args.push_back(callback_arg());
    auto receiver_arg = mojom::NodeInvokeArg::New();
    receiver_arg->value = received_receiver.Clone();
    args.push_back(std::move(receiver_arg));
    InvokeFuture invoked;
    executor_->InvokeFunction(addon_path_, "InvokeCallbackWithReceiver", -11,
                              std::move(args), invoked.GetCallback(), false, {},
                              1);
    ASSERT_TRUE(invoked.Get<0>()) << invoked.Get<3>();
    EXPECT_EQ(id, invoked.Get<1>().GetDict().FindInt("instance_id"));
    EXPECT_EQ(invoked.Get<1>(), received_receiver);
    EXPECT_EQ(received_receiver, received_args[0]);
    EXPECT_EQ(2u, XenonNodeExecutorTestPeer::InstanceIndexSize(*executor_));
  }
  executor_->ReleaseInstance(addon_path_, id, 1,
                             executor_->GetInstanceOwnerToken(1));
  EXPECT_EQ(1u, XenonNodeExecutorTestPeer::InstanceIndexSize(*executor_));
  executor_->ReleaseInstanceOwner(1);
  EXPECT_EQ(0u, XenonNodeExecutorTestPeer::InstanceIndexSize(*executor_));
}

TEST_F(XenonNodePromiseTest, GlobalCallbackReceiverUsesMarkerAndNoInstance) {
  base::Value received_receiver;
  executor_->SetCallbackHandlers(
      base::BindRepeating(
          [](base::Value* receiver, int32_t, int32_t, std::vector<base::Value>,
             base::Value value) { *receiver = std::move(value); },
          &received_receiver),
      {});
  std::vector<mojom::NodeInvokeArgPtr> args;
  auto callback = mojom::NodeInvokeArg::New();
  callback->is_callback = true;
  callback->callback_id = 7;
  args.push_back(std::move(callback));
  auto receiver = mojom::NodeInvokeArg::New();
  receiver->value = base::Value(
      base::DictValue().Set("__xenon_node_wire_type__", "undefined"));
  args.push_back(std::move(receiver));
  InvokeFuture invoked;
  executor_->InvokeFunction(addon_path_, "InvokeCallbackWithReceiver", -11,
                            std::move(args), invoked.GetCallback(), false, {},
                            1);
  ASSERT_TRUE(invoked.Get<0>()) << invoked.Get<3>();
  EXPECT_EQ(
      base::Value(base::DictValue().Set("__xenon_node_wire_type__", "global")),
      received_receiver);
  EXPECT_EQ(0u, XenonNodeExecutorTestPeer::InstanceIndexSize(*executor_));
}

TEST_F(XenonNodePromiseTest, ReceiverSerializationStopsAfterOwnerRelease) {
  ASSERT_TRUE(XenonNodeExecutorTestPeer::InstallOwnerReentryFixture(
      *executor_, addon_path_));
  int deliveries = 0;
  executor_->SetCallbackHandlers(
      base::BindRepeating(
          [](int* deliveries, int32_t, int32_t, std::vector<base::Value>,
             base::Value) { ++*deliveries; },
          &deliveries),
      {});
  auto callback = mojom::NodeInvokeArg::New();
  callback->is_callback = true;
  callback->callback_id = 7;
  std::vector<mojom::NodeInvokeArgPtr> args;
  args.push_back(std::move(callback));
  InvokeFuture invoked;
  executor_->InvokeFunction(addon_path_, "InvokeOwnerClosingReceiver", -11,
                            std::move(args), invoked.GetCallback(), false, {},
                            1);
  EXPECT_FALSE(invoked.Get<0>());
  EXPECT_EQ(0, deliveries);
  EXPECT_EQ(0u, XenonNodeExecutorTestPeer::CallbackCount(*executor_));
  EXPECT_EQ(0u, XenonNodeExecutorTestPeer::InstanceIndexSize(*executor_));
}

TEST_F(XenonNodePromiseTest, NativeReceiverSnapshotDoesNotExecuteAccessors) {
  ASSERT_TRUE(XenonNodeExecutorTestPeer::InstallOwnerReentryFixture(
      *executor_, addon_path_));
  base::Value receiver;
  executor_->SetCallbackHandlers(
      base::BindRepeating(
          [](base::Value* output, int32_t, int32_t, std::vector<base::Value>,
             base::Value value) { *output = std::move(value); },
          &receiver),
      {});
  auto callback = mojom::NodeInvokeArg::New();
  callback->is_callback = true;
  callback->callback_id = 7;
  std::vector<mojom::NodeInvokeArgPtr> args;
  args.push_back(std::move(callback));
  InvokeFuture invoked;
  executor_->InvokeFunction(addon_path_, "InvokeAccessorReceiver", -11,
                            std::move(args), invoked.GetCallback(), false, {},
                            1);
  ASSERT_TRUE(invoked.Get<0>()) << invoked.Get<3>();
  ASSERT_TRUE(receiver.is_dict());
  const auto* fields = receiver.GetDict().FindDict("fields");
  ASSERT_TRUE(fields);
  EXPECT_EQ(11, fields->FindInt("lastID"));
  EXPECT_EQ(1, fields->FindInt("changes"));
  EXPECT_FALSE(fields->contains("dangerous"));
  EXPECT_FALSE(fields->contains("count"));
  EXPECT_FALSE(fields->contains("shadowed"));
  EXPECT_EQ(0, CallSync("AccessorReceiverReads").GetInt());
  EXPECT_EQ(receiver.GetDict().FindInt("instance_id"),
            invoked.Get<1>().GetDict().FindInt("instance_id"));
}

TEST_F(XenonNodePromiseTest, SharedNativeObjectHasIndependentOwnerIdentities) {
  ASSERT_TRUE(XenonNodeExecutorTestPeer::InstallOwnerReentryFixture(
      *executor_, addon_path_));
  const base::Value first = CallOwned("SharedOwnerObject", 1);
  const base::Value second = CallOwned("SharedOwnerObject", 2);
  ASSERT_TRUE(first.is_dict());
  ASSERT_TRUE(second.is_dict());
  const auto first_id = first.GetDict().FindInt("instance_id");
  const auto second_id = second.GetDict().FindInt("instance_id");
  ASSERT_TRUE(first_id);
  ASSERT_TRUE(second_id);
  EXPECT_NE(first_id, second_id);
  EXPECT_EQ(first, CallOwned("SharedOwnerObject", 1));
  EXPECT_EQ(second, CallOwned("SharedOwnerObject", 2));
  EXPECT_EQ(2u, XenonNodeExecutorTestPeer::InstanceIndexSize(*executor_));
  executor_->ReleaseInstanceOwner(1);
  EXPECT_EQ(second, CallOwned("SharedOwnerObject", 2));
  EXPECT_EQ(1u, XenonNodeExecutorTestPeer::InstanceIndexSize(*executor_));
  executor_->RegisterInstanceOwner(1);
  const base::Value replacement = CallOwned("SharedOwnerObject", 1);
  EXPECT_NE(first_id, replacement.GetDict().FindInt("instance_id"));
  EXPECT_NE(*first.GetDict().FindString("owner_token"),
            *replacement.GetDict().FindString("owner_token"));
  executor_->ReleaseInstanceOwner(1);
  executor_->ReleaseInstanceOwner(2);
  EXPECT_EQ(0u, XenonNodeExecutorTestPeer::InstanceIndexSize(*executor_));
}

TEST_F(XenonNodePromiseTest, MetadataFreeLoadLazilyBuildsLegacyExportTree) {
  executor_ = std::make_unique<XenonNodeExecutor>();
  LoadFuture initial;
  executor_->LoadAddon(addon_path_, initial.GetCallback(), false);
  ASSERT_TRUE(initial.Get<0>()) << initial.Get<1>();
  EXPECT_TRUE(initial.Get<2>().empty());
  mojom::NodeExportInfoPtr root;
  std::string error;
  ASSERT_TRUE(
      executor_->InspectExportFromCurrentThread(addon_path_, "", &root, &error))
      << error;
  ASSERT_TRUE(root);
  EXPECT_EQ("object", root->kind);
  EXPECT_TRUE(FindExportChild(*root, "Add"));

  LoadFuture legacy;
  executor_->LoadAddon(addon_path_, legacy.GetCallback());
  ASSERT_TRUE(legacy.Get<0>()) << legacy.Get<1>();
  const auto* add = FindExportChild(legacy.Get<2>(), "Add");
  ASSERT_TRUE(add);
  EXPECT_EQ("function", add->kind);
  EXPECT_TRUE(FindExportChild(legacy.Get<2>(), "BeginControlledPromise"));
  EXPECT_EQ(0, Calls());
}

TEST_F(XenonNodePromiseTest, SynchronousLoadCanStillReturnLegacyExportTree) {
  // SetUp loaded this addon with LoadAddonFromCurrentThread.
  LoadFuture legacy;
  executor_->LoadAddon(addon_path_, legacy.GetCallback());
  ASSERT_TRUE(legacy.Get<0>()) << legacy.Get<1>();
  const auto* add = FindExportChild(legacy.Get<2>(), "Add");
  ASSERT_TRUE(add);
  EXPECT_EQ("function", add->kind);
  EXPECT_TRUE(FindExportChild(legacy.Get<2>(), "BeginControlledPromise"));
  EXPECT_EQ(0, Calls());
}

TEST_F(XenonNodePromiseTest,
       CollectedCallbackCleanupCannotEraseNewRegistration) {
  std::vector<int32_t> released_clients;
  executor_->SetCallbackHandlers(
      {}, base::BindRepeating(
              [](std::vector<int32_t>* clients, int32_t client_id,
                 int32_t callback_id) {
                EXPECT_EQ(7, callback_id);
                clients->push_back(client_id);
              },
              &released_clients));
  XenonNodeExecutorTestPeer::RegisterCollectibleCallback(*executor_, -11, 7);
  XenonNodeExecutorTestPeer::ForceGarbageCollection(*executor_);
  ASSERT_TRUE(XenonNodeExecutorTestPeer::IsCollected(*executor_, -11, 7));
  EXPECT_TRUE(released_clients.empty());
  executor_->ReleaseCallbacksForClient(-11);
  EXPECT_EQ(0u, XenonNodeExecutorTestPeer::CallbackCount(*executor_));
  XenonNodeExecutorTestPeer::RegisterCollectibleCallback(*executor_, -11, 7);
  task_environment_.RunUntilIdle();
  EXPECT_EQ(1u, XenonNodeExecutorTestPeer::CallbackCount(*executor_));
  EXPECT_TRUE(released_clients.empty());

  XenonNodeExecutorTestPeer::ForceGarbageCollection(*executor_);
  ASSERT_TRUE(XenonNodeExecutorTestPeer::IsCollected(*executor_, -11, 7));
  task_environment_.RunUntilIdle();
  EXPECT_EQ(0u, XenonNodeExecutorTestPeer::CallbackCount(*executor_));
  EXPECT_EQ((std::vector<int32_t>{-11}), released_clients);
}

TEST_F(XenonNodePromiseTest, ExecutorDestructionCancelsCollectedCallbackTask) {
  bool released = false;
  executor_->SetCallbackHandlers(
      {}, base::BindRepeating(
              [](bool* released, int32_t, int32_t) { *released = true; },
              &released));
  XenonNodeExecutorTestPeer::RegisterCollectibleCallback(*executor_, -11, 7);
  XenonNodeExecutorTestPeer::ForceGarbageCollection(*executor_);
  ASSERT_TRUE(XenonNodeExecutorTestPeer::IsCollected(*executor_, -11, 7));
  executor_.reset();
  task_environment_.RunUntilIdle();
  EXPECT_FALSE(released);
}

TEST_F(XenonNodePromiseTest, InstanceOwnersIsolateConstructorsAndProperties) {
  const int32_t main_instance = ConstructOwned(0);
  const int32_t first = ConstructOwned(1);
  const int32_t second = ConstructOwned(2);
  using PropertyFuture =
      base::test::TestFuture<bool, base::Value, const std::string&>;
  PropertyFuture wrong_property;
  executor_->GetInstanceProperty(addon_path_, first, "value",
                                 wrong_property.GetCallback(), 2);
  EXPECT_FALSE(wrong_property.Get<0>());
  InvokeFuture wrong_method;
  executor_->InvokeInstance(addon_path_, first, "read", 0, {},
                            wrong_method.GetCallback(), false, {}, 2);
  EXPECT_FALSE(wrong_method.Get<0>());
  base::test::TestFuture<bool, const std::string&> wrong_write;
  executor_->SetInstanceProperty(addon_path_, first, "value", base::Value(99),
                                 wrong_write.GetCallback(), 2);
  EXPECT_FALSE(wrong_write.Get<0>());

  PropertyFuture child;
  executor_->InspectInstanceMember(addon_path_, first, "child",
                                   child.GetCallback(), 1);
  ASSERT_TRUE(child.Get<0>()) << child.Get<2>();
  EXPECT_EQ(2u, XenonNodeExecutorTestPeer::InstanceCount(*executor_, 1));
  const std::string token = executor_->GetInstanceOwnerToken(1);
  ASSERT_FALSE(token.empty());
  executor_->ReleaseInstance(addon_path_, first, 2,
                             executor_->GetInstanceOwnerToken(2));
  executor_->ReleaseInstance(addon_path_, first, 1, "stale-token");
  EXPECT_EQ(2u, XenonNodeExecutorTestPeer::InstanceCount(*executor_, 1));
  executor_->ReleaseInstance(addon_path_, first, 1, token);
  EXPECT_EQ(1u, XenonNodeExecutorTestPeer::InstanceCount(*executor_, 1));
  executor_->ReleaseInstanceOwner(1);
  executor_->ReleaseInstanceOwner(0);
  EXPECT_EQ(0u, XenonNodeExecutorTestPeer::InstanceCount(*executor_, 1));
  for (const auto& [id, owner] : std::vector<std::pair<int32_t, uint64_t>>{
           {main_instance, 0}, {second, 2}}) {
    InvokeFuture value;
    executor_->InvokeInstance(addon_path_, id, "read", 0, {},
                              value.GetCallback(), false, {}, owner);
    ASSERT_TRUE(value.Get<0>()) << value.Get<3>();
    EXPECT_EQ(42, value.Get<1>().GetInt());
  }
}

TEST_F(XenonNodePromiseTest, NestedReturnsAndWireArgumentsKeepOwnerTokens) {
  const base::Value returns = CallOwned("MakeOwnedReturns", 1);
  ASSERT_TRUE(returns.is_list());
  ASSERT_EQ(2u, returns.GetList().size());
  EXPECT_EQ(3u, XenonNodeExecutorTestPeer::InstanceCount(*executor_, 1));
  const std::string token = executor_->GetInstanceOwnerToken(1);
  const auto& object = returns.GetList()[0].GetDict();
  const auto& function = returns.GetList()[1].GetDict();
  ASSERT_TRUE(object.FindString("owner_token"));
  EXPECT_EQ(token, *object.FindString("owner_token"));
  ASSERT_TRUE(function.FindString("owner_token"));
  EXPECT_EQ(token, *function.FindString("owner_token"));
  const base::Value settled = CallOwned("ResolvedOwnedPromise", 2);
  ASSERT_TRUE(settled.is_list());
  EXPECT_EQ(3u, XenonNodeExecutorTestPeer::InstanceCount(*executor_, 2));
  EXPECT_EQ(executor_->GetInstanceOwnerToken(2),
            *settled.GetList()[0].GetDict().FindString("owner_token"));
  const base::DictValue* fields = object.FindDict("fields");
  ASSERT_TRUE(fields);
  const base::DictValue* child = fields->FindDict("child");
  ASSERT_TRUE(child);
  EXPECT_EQ(token, *child->FindString("owner_token"));

  base::ListValue valid;
  valid.Append(returns.GetList()[1].Clone());
  EXPECT_TRUE(CallOwned("EchoOwnedHandle", 1, std::move(valid)).is_dict());
  const size_t count = XenonNodeExecutorTestPeer::InstanceCount(*executor_, 1);
  for (uint64_t owner : {0u, 2u}) {
    base::ListValue args;
    args.Append(returns.GetList()[0].Clone());
    InvokeFuture rejected;
    executor_->InvokeFunction(addon_path_, "EchoOwnedHandle", 0,
                              ValueArgs(std::move(args)),
                              rejected.GetCallback(), false, {}, owner);
    EXPECT_FALSE(rejected.Get<0>());
  }
  base::Value stale = returns.GetList()[0].Clone();
  stale.GetDict().Set("owner_token", "stale-token");
  base::ListValue stale_args;
  stale_args.Append(std::move(stale));
  InvokeFuture rejected;
  executor_->InvokeFunction(addon_path_, "EchoOwnedHandle", 0,
                            ValueArgs(std::move(stale_args)),
                            rejected.GetCallback(), false, {}, 1);
  EXPECT_FALSE(rejected.Get<0>());
  EXPECT_EQ(count, XenonNodeExecutorTestPeer::InstanceCount(*executor_, 1));
  executor_->ReleaseInstanceOwner(1);
  EXPECT_EQ(0u, XenonNodeExecutorTestPeer::InstanceCount(*executor_, 1));
  EXPECT_EQ(3u, XenonNodeExecutorTestPeer::InstanceCount(*executor_, 2));
  executor_->RegisterInstanceOwner(1);
  EXPECT_NE(token, executor_->GetInstanceOwnerToken(1));
  EXPECT_FALSE(executor_->ValidateInstanceOwnerToken(1, token));
  XenonNodeExecutor replacement;
  replacement.RegisterInstanceOwner(1);
  EXPECT_FALSE(replacement.ValidateInstanceOwnerToken(
      1, executor_->GetInstanceOwnerToken(1)));
}

TEST_F(XenonNodePromiseTest, OwnerReleasePreventsLateDeferredPromiseAdoption) {
  const uint64_t live = Begin(1);
  InvokeFuture result;
  executor_->AwaitDeferredPromise(live, 1, result.GetCallback());
  SettleWithHandles(1);
  ASSERT_TRUE(result.Get<0>()) << result.Get<3>();
  EXPECT_EQ(3u, XenonNodeExecutorTestPeer::InstanceCount(*executor_, 1));
  executor_->ReleaseInstanceOwner(1);
  EXPECT_EQ(0u, XenonNodeExecutorTestPeer::InstanceCount(*executor_, 1));

  const uint64_t subscribed = Begin(2);
  const uint64_t unclaimed = Begin(2);
  InvokeFuture cancelled;
  executor_->AwaitDeferredPromise(subscribed, 2, cancelled.GetCallback());
  executor_->ReleaseInstanceOwner(2);
  EXPECT_FALSE(cancelled.Get<0>());
  SettleWithHandles(2);
  SettleWithHandles(3);
  InvokeFuture missing;
  executor_->AwaitDeferredPromise(unclaimed, 2, missing.GetCallback());
  EXPECT_FALSE(missing.Get<0>());
  EXPECT_EQ(0u, XenonNodeExecutorTestPeer::InstanceCount(*executor_, 2));
  const int calls = Calls();
  InvokeFuture dead;
  executor_->InvokeFunction(addon_path_, "BeginControlledPromise", 0, {},
                            dead.GetCallback(), true, {}, 2);
  EXPECT_FALSE(dead.Get<0>());
  EXPECT_EQ(calls, Calls());
}

TEST_F(XenonNodePromiseTest, AsyncCallsAndCallbacksAdoptOnlyForLiveOwner) {
  InvokeFuture live;
  executor_->InvokeFunction(addon_path_, "BeginControlledPromise", 0, {},
                            live.GetCallback(), true, {}, 1);
  EXPECT_FALSE(live.IsReady());
  SettleWithHandles(1);
  ASSERT_TRUE(live.Get<0>()) << live.Get<3>();
  EXPECT_EQ(3u, XenonNodeExecutorTestPeer::InstanceCount(*executor_, 1));
  std::vector<base::Value> callback_args;
  executor_->SetCallbackHandlers(
      base::BindRepeating([](std::vector<base::Value>* output, int32_t, int32_t,
                             std::vector<base::Value> args,
                             base::Value) { *output = std::move(args); },
                          &callback_args),
      {});
  auto callback_arg = mojom::NodeInvokeArg::New();
  callback_arg->is_callback = true;
  callback_arg->callback_id = 7;
  std::vector<mojom::NodeInvokeArgPtr> args;
  args.push_back(std::move(callback_arg));
  InvokeFuture callback_call;
  executor_->InvokeFunction(addon_path_, "InvokeCallbackWithFunction", -11,
                            std::move(args), callback_call.GetCallback(), true,
                            {}, 1);
  ASSERT_TRUE(callback_call.Get<0>());
  ASSERT_EQ(1u, callback_args.size());
  EXPECT_EQ(executor_->GetInstanceOwnerToken(1),
            *callback_args[0].GetDict().FindString("owner_token"));
  EXPECT_EQ(4u, XenonNodeExecutorTestPeer::InstanceCount(*executor_, 1));
  InvokeFuture cancelled;
  executor_->InvokeFunction(addon_path_, "BeginControlledPromise", 0, {},
                            cancelled.GetCallback(), true, {}, 1);
  executor_->ReleaseInstanceOwner(1);
  EXPECT_FALSE(cancelled.Get<0>());
  EXPECT_EQ(0u, XenonNodeExecutorTestPeer::CallbackCount(*executor_));
  SettleWithHandles(2);
  EXPECT_EQ(0u, XenonNodeExecutorTestPeer::InstanceCount(*executor_, 1));
}

TEST_F(XenonNodePromiseTest,
       MicrotaskOwnerReleaseCannotRecreatePendingPromise) {
  executor_->SetCallbackHandlers(
      base::BindRepeating(
          [](XenonNodeExecutor* executor, int32_t, int32_t,
             std::vector<base::Value>,
             base::Value) { executor->ReleaseInstanceOwner(1); },
          executor_.get()),
      {});
  for (bool deferred : {false, true}) {
    executor_->RegisterInstanceOwner(1);
    auto arg = mojom::NodeInvokeArg::New();
    arg->is_callback = true;
    arg->callback_id = 7;
    std::vector<mojom::NodeInvokeArgPtr> args;
    args.push_back(std::move(arg));
    InvokeFuture result;
    base::test::TestFuture<uint64_t> unexpected_token;
    executor_->InvokeFunction(
        addon_path_, "PendingPromiseWithMicrotaskCallback", -11,
        std::move(args), result.GetCallback(), !deferred,
        deferred ? unexpected_token.GetCallback()
                 : XenonNodeExecutor::DeferPromiseCallback(),
        1);
    ASSERT_TRUE(result.IsReady());
    EXPECT_FALSE(result.Get<0>());
    EXPECT_FALSE(unexpected_token.IsReady());
    EXPECT_TRUE(executor_->GetInstanceOwnerToken(1).empty());
    EXPECT_EQ(0u, XenonNodeExecutorTestPeer::PromiseCount(*executor_));
  }
}

TEST_F(XenonNodePromiseTest,
       ReentrantOwnerReleaseStopsConstructorsChainsAndEvents) {
  ASSERT_TRUE(XenonNodeExecutorTestPeer::InstallOwnerReentryFixture(
      *executor_, addon_path_));
  base::test::TestFuture<bool, int32_t, const std::string&> constructor;
  executor_->ConstructExport(addon_path_, "OwnerClosingConstructor", 0, {},
                             constructor.GetCallback(), 1);
  EXPECT_FALSE(constructor.Get<0>());
  EXPECT_EQ(0, CallSync("OwnerConstructorCalls").GetInt());

  executor_->RegisterInstanceOwner(1);
  auto constructor_arg = mojom::NodeInvokeArg::New();
  constructor_arg->is_callback = true;
  constructor_arg->callback_id = 8;
  std::vector<mojom::NodeInvokeArgPtr> constructor_args;
  constructor_args.push_back(std::move(constructor_arg));
  base::test::TestFuture<bool, int32_t, const std::string&>
      callback_constructor;
  executor_->ConstructExport(addon_path_, "OwnerClosingConstructor", -11,
                             std::move(constructor_args),
                             callback_constructor.GetCallback(), 1);
  EXPECT_FALSE(callback_constructor.Get<0>());
  EXPECT_EQ(0u, XenonNodeExecutorTestPeer::CallbackCount(*executor_));

  executor_->RegisterInstanceOwner(1);
  const base::Value chain = CallOwned("MakeOwnerClosingChain", 1);
  ASSERT_TRUE(chain.is_dict());
  const auto instance_id = chain.GetDict().FindInt("instance_id");
  ASSERT_TRUE(instance_id);
  InvokeFuture chain_call;
  executor_->InvokeInstance(addon_path_, *instance_id,
                            "$xenonInvokePath:first.second.read", 0, {},
                            chain_call.GetCallback(), false, {}, 1);
  EXPECT_FALSE(chain_call.Get<0>());
  EXPECT_EQ(0, CallSync("OwnerChainCalls").GetInt());

  executor_->RegisterInstanceOwner(1);
  int deliveries = 0;
  executor_->SetCallbackHandlers(
      base::BindRepeating(
          [](int* deliveries, int32_t, int32_t, std::vector<base::Value>,
             base::Value) { ++*deliveries; },
          &deliveries),
      {});
  auto arg = mojom::NodeInvokeArg::New();
  arg->is_callback = true;
  arg->callback_id = 7;
  std::vector<mojom::NodeInvokeArgPtr> args;
  args.push_back(std::move(arg));
  InvokeFuture callback_call;
  executor_->InvokeFunction(addon_path_, "InvokeOwnerClosingPayload", -11,
                            std::move(args), callback_call.GetCallback(), false,
                            {}, 1);
  EXPECT_FALSE(callback_call.Get<0>());
  EXPECT_EQ(0, deliveries);
  EXPECT_EQ(0u, XenonNodeExecutorTestPeer::InstanceCount(*executor_, 1));
}

base::DictValue PrototypeCallbacks(int callback_id) {
  base::DictValue properties;
  properties.Set("emit", base::DictValue()
                             .Set("__xenon_node_wire_type__", "callback")
                             .Set("callback_id", callback_id));
  // A caller cannot replace the original native method with a callback.
  properties.Set("read", base::DictValue()
                             .Set("__xenon_node_wire_type__", "callback")
                             .Set("callback_id", callback_id + 100));
  return properties;
}

TEST_F(XenonNodePromiseTest,
       NativePrototypeCallbackIsPresentDuringConstruction) {
  std::vector<std::string> events;
  std::vector<base::Value> receivers;
  executor_->SetCallbackHandlers(
      base::BindRepeating(
          [](std::vector<std::string>* events,
             std::vector<base::Value>* receivers, int32_t client,
             int32_t callback, std::vector<base::Value> args,
             base::Value receiver) {
            EXPECT_EQ(-11, client);
            EXPECT_EQ(7, callback);
            ASSERT_EQ(1u, args.size());
            events->push_back(args[0].GetString());
            receivers->push_back(std::move(receiver));
          },
          &events, &receivers),
      {});
  base::test::TestFuture<bool, int32_t, const std::string&> constructed;
  executor_->ConstructExport(addon_path_, "PrototypeCallback", -11, {},
                             constructed.GetCallback(), 1,
                             PrototypeCallbacks(7));
  ASSERT_TRUE(constructed.Get<0>()) << constructed.Get<2>();
  const int id = constructed.Get<1>();
  ASSERT_EQ(1u, receivers.size());
  EXPECT_EQ(id, receivers[0].GetDict().FindInt("instance_id"));
  EXPECT_EQ(executor_->GetInstanceOwnerToken(1),
            *receivers[0].GetDict().FindString("owner_token"));
  InvokeFuture read;
  executor_->InvokeInstance(addon_path_, id, "read", -11, {},
                            read.GetCallback(), false, {}, 1);
  ASSERT_TRUE(read.Get<0>()) << read.Get<3>();
  EXPECT_EQ(42, read.Get<1>().GetInt());
  InvokeFuture fire;
  executor_->InvokeInstance(addon_path_, id, "fire", -11, {},
                            fire.GetCallback(), false, {}, 1);
  ASSERT_TRUE(fire.Get<0>()) << fire.Get<3>();
  EXPECT_TRUE(fire.Get<1>().GetBool());
  EXPECT_EQ((std::vector<std::string>{"constructed", "later"}), events);
  ASSERT_EQ(2u, receivers.size());
  EXPECT_EQ(id, receivers[1].GetDict().FindInt("instance_id"));
}

TEST_F(XenonNodePromiseTest,
       NativePrototypeAdditionsAreIsolatedPerInstanceAndOwner) {
  std::vector<int32_t> delivered;
  executor_->SetCallbackHandlers(
      base::BindRepeating([](std::vector<int32_t>* output, int32_t,
                             int32_t callback, std::vector<base::Value>,
                             base::Value) { output->push_back(callback); },
                          &delivered),
      {});
  auto construct = [&](uint64_t owner, int callback_id) {
    base::test::TestFuture<bool, int32_t, const std::string&> result;
    executor_->ConstructExport(
        addon_path_, "PrototypeCallback", -11, {}, result.GetCallback(), owner,
        callback_id ? PrototypeCallbacks(callback_id) : base::DictValue());
    EXPECT_TRUE(result.Get<0>()) << result.Get<2>();
    return result.Get<1>();
  };
  const int first = construct(1, 7);
  const int second = construct(2, 8);
  const int bare = construct(2, 0);
  const auto fire = [&](int id, uint64_t owner) {
    InvokeFuture result;
    executor_->InvokeInstance(addon_path_, id, "fire", -11, {},
                              result.GetCallback(), false, {}, owner);
    EXPECT_TRUE(result.Get<0>()) << result.Get<3>();
    return result.Get<0>() && result.Get<1>().GetBool();
  };
  EXPECT_TRUE(fire(first, 1));
  EXPECT_TRUE(fire(second, 2));
  EXPECT_FALSE(fire(bare, 2));
  EXPECT_EQ((std::vector<int32_t>{7, 8, 7, 8}), delivered);
  executor_->ReleaseInstanceOwner(1);
  InvokeFuture closed;
  executor_->InvokeInstance(addon_path_, first, "fire", -11, {},
                            closed.GetCallback(), false, {}, 1);
  EXPECT_FALSE(closed.Get<0>());
  EXPECT_TRUE(fire(second, 2));
  EXPECT_FALSE(fire(bare, 2));
  EXPECT_EQ((std::vector<int32_t>{7, 8, 7, 8, 8}), delivered);
}

TEST_F(XenonNodePromiseTest,
       NativePrototypeRejectsNonFunctionsAndClosedOwners) {
  base::test::TestFuture<bool, int32_t, const std::string&> invalid;
  executor_->ConstructExport(addon_path_, "PrototypeCallback", -11, {},
                             invalid.GetCallback(), 1,
                             base::DictValue().Set("emit", 42));
  EXPECT_FALSE(invalid.Get<0>());
  EXPECT_NE(std::string::npos, invalid.Get<2>().find("must be functions"));
  executor_->ReleaseInstanceOwner(1);
  base::test::TestFuture<bool, int32_t, const std::string&> closed;
  executor_->ConstructExport(addon_path_, "PrototypeCallback", -11, {},
                             closed.GetCallback(), 1, PrototypeCallbacks(7));
  EXPECT_FALSE(closed.Get<0>());
  EXPECT_EQ(0u, XenonNodeExecutorTestPeer::CallbackCount(*executor_));
}

TEST_F(XenonNodePromiseTest,
       AsyncWrappedPrototypeSurvivesGcAndPreservesReceiver) {
  std::vector<int32_t> callbacks;
  std::vector<base::Value> receivers;
  executor_->SetCallbackHandlers(
      base::BindRepeating(
          [](std::vector<int32_t>* callbacks,
             std::vector<base::Value>* receivers, int32_t client,
             int32_t callback, std::vector<base::Value> args,
             base::Value receiver) {
            EXPECT_EQ(-11, client);
            callbacks->push_back(callback);
            receivers->push_back(std::move(receiver));
            ASSERT_EQ(1u, args.size());
            if (callback == 7) {
              EXPECT_TRUE(args[0].is_none());
            } else {
              EXPECT_EQ("open", args[0].GetString());
            }
          },
          &callbacks, &receivers),
      {});
  int id;
  {
    base::ScopedThreadPoolExecutionFence fence;
    auto callback = mojom::NodeInvokeArg::New();
    callback->is_callback = true;
    callback->callback_id = 7;
    std::vector<mojom::NodeInvokeArgPtr> args;
    args.push_back(std::move(callback));
    base::test::TestFuture<bool, int32_t, const std::string&> constructed;
    executor_->ConstructExport(addon_path_, "AsyncPrototypeCallback", -11,
                               std::move(args), constructed.GetCallback(), 1,
                               PrototypeCallbacks(8));
    ASSERT_TRUE(constructed.Get<0>()) << constructed.Get<2>();
    id = constructed.Get<1>();
    XenonNodeExecutorTestPeer::ForceGarbageCollection(*executor_);
    EXPECT_TRUE(callbacks.empty());
  }
  task_environment_.RunUntilIdle();
  EXPECT_EQ((std::vector<int32_t>{7, 8}), callbacks);
  ASSERT_EQ(2u, receivers.size());
  for (const auto& receiver : receivers) {
    ASSERT_TRUE(receiver.is_dict());
    EXPECT_EQ(id, receiver.GetDict().FindInt("instance_id"));
    EXPECT_EQ(executor_->GetInstanceOwnerToken(1),
              *receiver.GetDict().FindString("owner_token"));
  }
  InvokeFuture read;
  executor_->InvokeInstance(addon_path_, id, "read", -11, {},
                            read.GetCallback(), false, {}, 1);
  ASSERT_TRUE(read.Get<0>()) << read.Get<3>();
  EXPECT_EQ(42, read.Get<1>().GetInt());
}

}  // namespace
}  // namespace xenon
