// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_SERVICES_XENON_NET_PIPE_BRIDGE_H_
#define XENON_OVERLAY_SERVICES_XENON_NET_PIPE_BRIDGE_H_

#include <cstdint>
#include <map>
#include <memory>
#include <string>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "uv.h"

namespace xenon {

// Hosts real operating-system named pipes for the renderer's Node-compatible
// `net` module. JavaScript owns the familiar Server/Socket objects; this class
// owns the OS handles and transports only lifecycle events and byte streams.
// No application protocol is interpreted here.
class XenonNetPipeBridge {
 public:
  using EventCallback = base::RepeatingCallback<void(
      const std::string& container_id,
      const std::string& endpoint_id,
      const std::string& channel,
      base::Value payload)>;

  explicit XenonNetPipeBridge(EventCallback event_callback);
  ~XenonNetPipeBridge();

  XenonNetPipeBridge(const XenonNetPipeBridge&) = delete;
  XenonNetPipeBridge& operator=(const XenonNetPipeBridge&) = delete;

  bool Listen(const std::string& container_id,
              const std::string& endpoint_id,
              const std::string& server_id,
              const std::string& path,
              std::string* error);
  bool Connect(const std::string& container_id,
               const std::string& endpoint_id,
               const std::string& client_socket_id,
               const std::string& path,
               std::string* error);
  bool Write(const std::string& socket_id,
             const base::DictValue& wire,
             std::string* error);
  bool CloseSocket(const std::string& socket_id);
  bool CloseServer(const std::string& server_id);
  bool HasSocket(const std::string& socket_id) const;
  bool HasServer(const std::string& server_id) const;
  bool HasListenerForPath(const std::string& path) const;
  void RemoveEndpoint(const std::string& container_id,
                      const std::string& endpoint_id);

 private:
  struct PipeServer;
  struct PipeSocket;

  static void DeleteServerHandle(uv_handle_t* handle);
  static void DeleteSocketHandle(uv_handle_t* handle);
  static void AllocateReadBuffer(uv_handle_t* handle,
                                 size_t suggested_size,
                                 uv_buf_t* buffer);
  static void OnPipeRead(uv_stream_t* stream,
                         ssize_t count,
                         const uv_buf_t* buffer);
  static void OnPipeConnection(uv_stream_t* stream, int status);
  static void OnPipeConnect(uv_connect_t* request, int status);

  void PumpLoop();
  void Dispatch(const std::string& container_id,
                const std::string& endpoint_id,
                const std::string& channel,
                base::DictValue payload);
  void CloseSocket(PipeSocket* socket, bool notify_renderer);
  void CloseServer(PipeServer* server, bool notify_renderer);

  EventCallback event_callback_;
  std::unique_ptr<uv_loop_t> owned_loop_;
  raw_ptr<uv_loop_t> loop_ = nullptr;
  base::RepeatingTimer loop_timer_;
  bool shutting_down_ = false;
  uint64_t next_socket_id_ = 1;

  // Objects delete themselves from their uv_close callback. Maps are only
  // indexes for live handles and never own the pointed-to objects.
  std::map<std::string, PipeServer*> servers_;
  std::map<std::string, PipeSocket*> sockets_;
};

}  // namespace xenon

#endif  // XENON_OVERLAY_SERVICES_XENON_NET_PIPE_BRIDGE_H_
