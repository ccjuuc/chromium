// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_SERVICES_XENON_CHILD_PROCESS_BRIDGE_H_
#define XENON_OVERLAY_SERVICES_XENON_CHILD_PROCESS_BRIDGE_H_

#include <map>
#include <memory>
#include <string>
#include <tuple>

#include "base/functional/callback.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "uv.h"

namespace xenon {

// Owns OS child processes for one service. Handles are scoped to their creating
// renderer (or main endpoint); no caller can address an arbitrary system PID.
class XenonChildProcessBridge {
 public:
  using EventCallback = base::RepeatingCallback<void(
      const std::string& container_id,
      const std::string& endpoint_id,
      base::Value payload)>;

  explicit XenonChildProcessBridge(EventCallback event_callback);
  ~XenonChildProcessBridge();
  XenonChildProcessBridge(const XenonChildProcessBridge&) = delete;
  XenonChildProcessBridge& operator=(const XenonChildProcessBridge&) = delete;

  base::DictValue Call(const std::string& container_id,
                      const std::string& endpoint_id,
                      const base::DictValue& request);
  void RemoveEndpoint(const std::string& container_id,
                      const std::string& endpoint_id);

 private:
  struct Child;
  struct Pipe;
  using Key = std::tuple<std::string, std::string, std::string>;
  static void AllocateReadBuffer(uv_handle_t* handle,
                                 size_t suggested_size,
                                 uv_buf_t* buffer);
  static void OnRead(uv_stream_t* stream, ssize_t count, const uv_buf_t* buffer);
  static void OnExit(uv_process_t* process, int64_t exit_status, int signal);
  static void OnPipeClosed(uv_handle_t* handle);
  static void OnProcessClosed(uv_handle_t* handle);
  static void ClosePipe(Pipe* pipe);
  void HandleClosed(Child* child);
  void Dispatch(Child* child, base::DictValue event);
  void Terminate(Child* child);
  void PumpLoop();
  base::DictValue Spawn(const Key& key, const base::DictValue& request);

  EventCallback event_callback_;
  uv_loop_t loop_{};
  uv_async_t pump_handle_{};
  base::RepeatingTimer loop_timer_;
  bool shutting_down_ = false;
  std::map<Key, std::unique_ptr<Child>> children_;
};

}  // namespace xenon

#endif  // XENON_OVERLAY_SERVICES_XENON_CHILD_PROCESS_BRIDGE_H_
