// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_RENDERER_IPC_XENON_CONTEXT_BRIDGE_H_
#define XENON_OVERLAY_CHROME_RENDERER_IPC_XENON_CONTEXT_BRIDGE_H_

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/memory/ref_counted.h"
#include "gin/arguments.h"
#include "v8/include/v8-persistent-handle.h"

namespace xenon::ipc {

// Proxies functions and Promises; copies other values into their destination
// realm. Call Shutdown before either context is released. Retained proxies
// are then revoked and cannot reconnect to another document/worker.
class XenonContextBridge final : public base::RefCounted<XenonContextBridge> {
 public:
  static scoped_refptr<XenonContextBridge> Create(
      v8::Local<v8::Context> preload_context,
      v8::Local<v8::Context> main_context);
  XenonContextBridge(v8::Local<v8::Context> preload_context,
                     v8::Local<v8::Context> main_context);
  v8::Local<v8::Object> GetApi();
  void Shutdown();

 private:
  friend class base::RefCounted<XenonContextBridge>;
  ~XenonContextBridge();
  void ExposeInMainWorld(gin::Arguments* args);
  void ExecuteInMainWorld(gin::Arguments* args);
  void CallBridgedFunction(size_t id, gin::Arguments* args);
  void SettleBridgedPromise(uint64_t id, bool reject, gin::Arguments* args);
  v8::MaybeLocal<v8::Value> CopyValue(
      v8::Local<v8::Context> source,
      v8::Local<v8::Context> target,
      v8::Local<v8::Value> value,
      int depth = 0,
      bool freeze = false,
      std::vector<std::pair<v8::Local<v8::Object>, v8::Local<v8::Object>>>*
          copies = nullptr);
  void ThrowError(const std::string& message);
  v8::Local<v8::Function> BindFunction(auto method, auto... bound_args);
  struct BridgedFunction {
    scoped_refptr<XenonContextBridge> owner;
    size_t id = 0;
    bool source_is_main;
    v8::Global<v8::Function> original;
    v8::Global<v8::Function> proxy;
  };
  static void FirstFunctionWeakCallback(
      const v8::WeakCallbackInfo<BridgedFunction>& data);
  static void SecondFunctionWeakCallback(
      const v8::WeakCallbackInfo<BridgedFunction>& data);
  struct BridgedPromise {
    bool target_is_main;
    v8::Global<v8::Promise::Resolver> resolver;
  };
  v8::Isolate* const isolate_;
  v8::Global<v8::Context> main_context_;
  v8::Global<v8::Context> preload_context_;
  v8::Global<v8::Private> original_function_key_;
  std::map<size_t, std::unique_ptr<BridgedFunction>> functions_;
  std::map<uint64_t, BridgedPromise> promises_;
  uint64_t next_id_ = 1;
  size_t next_function_id_ = 1;
  bool closed_ = false;
};

}  // namespace xenon::ipc

#endif  // XENON_OVERLAY_CHROME_RENDERER_IPC_XENON_CONTEXT_BRIDGE_H_
