// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_RENDERER_IPC_XENON_SCRIPT_EXECUTOR_H_
#define XENON_OVERLAY_CHROME_RENDERER_IPC_XENON_SCRIPT_EXECUTOR_H_

#include <stdint.h>

#include <map>
#include <string>

#include "base/memory/raw_ptr.h"
#include "base/memory/ref_counted.h"
#include "gin/arguments.h"
#include "v8/include/v8-persistent-handle.h"
#include "v8/include/v8-promise.h"

namespace xenon::ipc {

// Runs a trusted main-process request in its owned document's main world. The
// caller chooses that exact context; this helper does not find frames, grant
// permissions, or enable browser-wide JavaScript injection. Results and errors
// are structured-cloned to the caller realm, including when worlds coincide.
// The document owner must call Shutdown before either context is released.
class XenonScriptExecutor : public base::RefCounted<XenonScriptExecutor> {
 public:
  explicit XenonScriptExecutor(v8::Isolate* isolate);
  v8::MaybeLocal<v8::Promise> Execute(uint64_t request_id,
                                      v8::Local<v8::Context> main_context,
                                      v8::Local<v8::Context> caller_context,
                                      const std::string& source);
  void Cancel(uint64_t request_id);
  void Shutdown();

 private:
  friend class base::RefCounted<XenonScriptExecutor>;
  ~XenonScriptExecutor();
  v8::MaybeLocal<v8::Function> CreateSettlementCallback(
      v8::Local<v8::Context> context,
      uint64_t id,
      uint64_t generation,
      bool rejected);
  void Settle(uint64_t id,
              uint64_t generation,
              bool rejected,
              gin::Arguments* args);
  void Complete(uint64_t id,
                uint64_t generation,
                bool rejected,
                v8::Local<v8::Context> source_context,
                v8::Local<v8::Value> value);

  struct Pending {
    uint64_t generation;
    v8::Global<v8::Context> context;
    v8::Global<v8::Promise::Resolver> resolver;
  };
  const raw_ptr<v8::Isolate> isolate_;
  std::map<uint64_t, Pending> pending_;
  uint64_t next_generation_ = 1;
  bool closed_ = false;
};

}  // namespace xenon::ipc

#endif  // XENON_OVERLAY_CHROME_RENDERER_IPC_XENON_SCRIPT_EXECUTOR_H_
