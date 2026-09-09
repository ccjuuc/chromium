// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/services/xenon_net_pipe_bridge.h"

#include <algorithm>
#include <cstring>
#include <string_view>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/check.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/strings/string_util.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "uv.h"

namespace xenon {

namespace {

constexpr base::TimeDelta kUvPollInterval = base::Milliseconds(5);
constexpr int kPipeBacklog = 64;

std::string UvError(int status) {
  const char* text = uv_strerror(status);
  return text ? text : "unknown libuv error";
}

bool WireToBytes(const base::DictValue& wire,
                 std::string* bytes,
                 std::string* error) {
  const std::string* type = wire.FindString("t");
  const std::string* data = wire.FindString("d");
  if (!data) {
    *error = "net payload is missing data";
    return false;
  }
  if (type && *type == "b64") {
    if (!base::Base64Decode(*data, bytes)) {
      *error = "net payload contains invalid base64";
      return false;
    }
    return true;
  }
  // `s` is UTF-8 text. `b` is the legacy byte-string representation used by
  // the bootstrap; its characters are already transported one byte per code
  // unit for the protocol payloads that predate the OS bridge.
  *bytes = *data;
  return true;
}

}  // namespace

struct XenonNetPipeBridge::PipeServer {
  raw_ptr<XenonNetPipeBridge> owner;
  std::string container_id;
  std::string endpoint_id;
  std::string server_id;
  std::string path;
  uv_pipe_t handle{};
};

struct XenonNetPipeBridge::PipeSocket {
  raw_ptr<XenonNetPipeBridge> owner;
  std::string container_id;
  std::string endpoint_id;
  std::string server_id;
  std::string socket_id;
  // Renderer `net.Socket._id` for client-initiated connects. Incoming OS
  // reads/close must be delivered to this id, not the native handle id.
  std::string js_socket_id;
  uv_pipe_t handle{};
  uv_connect_t connect_request{};
};

namespace {

struct PipeWriteRequest {
  uv_write_t request{};
  std::string bytes;
};

bool PipePathsMatch(const std::string& left, const std::string& right) {
#if BUILDFLAG(IS_WIN)
  return base::EqualsCaseInsensitiveASCII(left, right);
#else
  return left == right;
#endif
}

void OnPipeWrite(uv_write_t* request, int status) {
  std::unique_ptr<PipeWriteRequest> write(
      reinterpret_cast<PipeWriteRequest*>(request));
  if (status < 0) {
    LOG(WARNING) << "Named-pipe write failed: " << UvError(status);
  }
}

}  // namespace

// static
void XenonNetPipeBridge::DeleteServerHandle(uv_handle_t* handle) {
  delete static_cast<PipeServer*>(handle->data);
}

// static
void XenonNetPipeBridge::DeleteSocketHandle(uv_handle_t* handle) {
  delete static_cast<PipeSocket*>(handle->data);
}

// static
void XenonNetPipeBridge::AllocateReadBuffer(uv_handle_t*,
                                            size_t suggested_size,
                                            uv_buf_t* buffer) {
  const size_t size = std::max<size_t>(suggested_size, 4096u);
  buffer->base = new char[size];
  buffer->len = static_cast<decltype(buffer->len)>(size);
}

// static
void XenonNetPipeBridge::OnPipeRead(uv_stream_t* stream,
                                    ssize_t count,
                                    const uv_buf_t* buffer) {
  std::unique_ptr<char[]> storage(buffer->base);
  auto* socket = static_cast<XenonNetPipeBridge::PipeSocket*>(stream->data);
  if (!socket || !socket->owner) {
    return;
  }
  if (count > 0) {
    base::DictValue wire;
    wire.Set("t", "b64");
    wire.Set("d", base::Base64Encode(
                      std::string_view(buffer->base, static_cast<size_t>(count))));
    base::DictValue payload;
    payload.Set("toId", socket->js_socket_id.empty() ? socket->socket_id
                                                     : socket->js_socket_id);
    payload.Set("wire", std::move(wire));
    socket->owner->Dispatch(socket->container_id, socket->endpoint_id,
                            "__xenon:net:data", std::move(payload));
    return;
  }
  if (count < 0) {
    socket->owner->CloseSocket(socket, true);
  }
}

// static
void XenonNetPipeBridge::OnPipeConnection(uv_stream_t* stream, int status) {
  auto* server = static_cast<PipeServer*>(stream->data);
  if (!server || !server->owner || status < 0) {
    if (server && server->owner && status < 0) {
      base::DictValue payload;
      payload.Set("serverId", server->server_id);
      payload.Set("code", UvError(status));
      server->owner->Dispatch(server->container_id, server->endpoint_id,
                              "__xenon:net:error", std::move(payload));
    }
    return;
  }

  auto* socket = new PipeSocket();
  socket->owner = server->owner;
  socket->container_id = server->container_id;
  socket->endpoint_id = server->endpoint_id;
  socket->server_id = server->server_id;
  socket->socket_id = "native-" +
                      std::to_string(server->owner->next_socket_id_++);
  const int init_status =
      uv_pipe_init(server->owner->loop_, &socket->handle, /*ipc=*/0);
  if (init_status < 0) {
    delete socket;
    return;
  }
  socket->handle.data = socket;
  const int accept_status =
      uv_accept(stream, reinterpret_cast<uv_stream_t*>(&socket->handle));
  if (accept_status < 0) {
    uv_close(reinterpret_cast<uv_handle_t*>(&socket->handle),
             DeleteSocketHandle);
    return;
  }
  server->owner->sockets_.emplace(socket->socket_id, socket);

  base::DictValue payload;
  payload.Set("serverId", server->server_id);
  payload.Set("socketId", socket->socket_id);
  server->owner->Dispatch(server->container_id, server->endpoint_id,
                          "__xenon:net:connection", std::move(payload));
  const int read_status = uv_read_start(
      reinterpret_cast<uv_stream_t*>(&socket->handle), AllocateReadBuffer,
      OnPipeRead);
  if (read_status < 0) {
    server->owner->CloseSocket(socket, true);
  }
}

// static
void XenonNetPipeBridge::OnPipeConnect(uv_connect_t* request, int status) {
  auto* socket = request && request->handle
                     ? static_cast<PipeSocket*>(request->handle->data)
                     : nullptr;
  if (!socket || !socket->owner) {
    return;
  }
  XenonNetPipeBridge* owner = socket->owner;
  if (owner->shutting_down_) {
    owner->CloseSocket(socket, false);
    return;
  }
  if (status < 0) {
    LOG(WARNING) << "Named-pipe connect failed: " << UvError(status);
    base::DictValue payload;
    payload.Set("toId", socket->js_socket_id.empty() ? socket->socket_id
                                                     : socket->js_socket_id);
    payload.Set("code", "ECONNREFUSED");
    owner->Dispatch(socket->container_id, socket->endpoint_id,
                    "__xenon:net:error", std::move(payload));
    owner->CloseSocket(socket, false);
    return;
  }

  base::DictValue payload;
  payload.Set("toId", socket->js_socket_id);
  payload.Set("peerId", socket->socket_id);
  owner->Dispatch(socket->container_id, socket->endpoint_id,
                  "__xenon:net:connected", std::move(payload));
  const int read_status = uv_read_start(
      reinterpret_cast<uv_stream_t*>(&socket->handle), AllocateReadBuffer,
      OnPipeRead);
  if (read_status < 0) {
    owner->CloseSocket(socket, true);
  }
}

XenonNetPipeBridge::XenonNetPipeBridge(EventCallback event_callback)
    : event_callback_(std::move(event_callback)),
      owned_loop_(std::make_unique<uv_loop_t>()),
      loop_(owned_loop_.get()) {
  const int status = uv_loop_init(loop_);
  CHECK_EQ(status, 0) << UvError(status);
  loop_timer_.Start(FROM_HERE, kUvPollInterval, this,
                    &XenonNetPipeBridge::PumpLoop);
}

XenonNetPipeBridge::~XenonNetPipeBridge() {
  shutting_down_ = true;
  loop_timer_.Stop();
  std::vector<PipeSocket*> sockets;
  sockets.reserve(sockets_.size());
  for (const auto& [id, socket] : sockets_) {
    sockets.push_back(socket);
  }
  for (PipeSocket* socket : sockets) {
    CloseSocket(socket, false);
  }
  std::vector<PipeServer*> servers;
  servers.reserve(servers_.size());
  for (const auto& [id, server] : servers_) {
    servers.push_back(server);
  }
  for (PipeServer* server : servers) {
    CloseServer(server, false);
  }
  while (uv_run(loop_, UV_RUN_NOWAIT) != 0) {
  }
  CHECK_EQ(uv_loop_close(loop_), 0);
  loop_ = nullptr;
}

bool XenonNetPipeBridge::Listen(const std::string& container_id,
                                const std::string& endpoint_id,
                                const std::string& server_id,
                                const std::string& path,
                                std::string* error) {
  if (server_id.empty() || path.empty()) {
    *error = "named-pipe server id and path are required";
    return false;
  }
  if (servers_.contains(server_id)) {
    *error = "named-pipe server id is already listening";
    return false;
  }
  for (const auto& [id, existing] : servers_) {
    if (existing->path == path) {
      *error = "EADDRINUSE";
      return false;
    }
  }

  auto* server = new PipeServer();
  server->owner = this;
  server->container_id = container_id;
  server->endpoint_id = endpoint_id;
  server->server_id = server_id;
  server->path = path;
  int status = uv_pipe_init(loop_, &server->handle, /*ipc=*/0);
  if (status < 0) {
    *error = UvError(status);
    delete server;
    return false;
  }
  server->handle.data = server;
  status = uv_pipe_bind(&server->handle, path.c_str());
  if (status >= 0) {
    status = uv_listen(reinterpret_cast<uv_stream_t*>(&server->handle),
                       kPipeBacklog, OnPipeConnection);
  }
  if (status < 0) {
    *error = UvError(status);
    uv_close(reinterpret_cast<uv_handle_t*>(&server->handle),
             DeleteServerHandle);
    uv_run(loop_, UV_RUN_NOWAIT);
    return false;
  }
  servers_.emplace(server_id, server);
  return true;
}

bool XenonNetPipeBridge::Connect(const std::string& container_id,
                                 const std::string& endpoint_id,
                                 const std::string& client_socket_id,
                                 const std::string& path,
                                 std::string* error) {
  if (client_socket_id.empty() || path.empty()) {
    *error = "named-pipe client id and path are required";
    return false;
  }

  auto* socket = new PipeSocket();
  socket->owner = this;
  socket->container_id = container_id;
  socket->endpoint_id = endpoint_id;
  socket->js_socket_id = client_socket_id;
  socket->socket_id = "native-" + std::to_string(next_socket_id_++);
  const int init_status = uv_pipe_init(loop_, &socket->handle, /*ipc=*/0);
  if (init_status < 0) {
    *error = UvError(init_status);
    delete socket;
    return false;
  }
  socket->handle.data = socket;
  sockets_.emplace(socket->socket_id, socket);
  uv_pipe_connect(&socket->connect_request, &socket->handle, path.c_str(),
                  OnPipeConnect);
  PumpLoop();
  return true;
}

bool XenonNetPipeBridge::Write(const std::string& socket_id,
                               const base::DictValue& wire,
                               std::string* error) {
  auto found = sockets_.find(socket_id);
  if (found == sockets_.end()) {
    return false;
  }
  auto write = std::make_unique<PipeWriteRequest>();
  if (!WireToBytes(wire, &write->bytes, error)) {
    return true;
  }
  uv_buf_t buffer =
      uv_buf_init(write->bytes.data(), static_cast<unsigned int>(write->bytes.size()));
  const int status = uv_write(
      &write->request,
      reinterpret_cast<uv_stream_t*>(&found->second->handle), &buffer, 1,
      OnPipeWrite);
  if (status < 0) {
    *error = UvError(status);
    return true;
  }
  write.release();
  return true;
}

bool XenonNetPipeBridge::CloseSocket(const std::string& socket_id) {
  auto found = sockets_.find(socket_id);
  if (found == sockets_.end()) {
    return false;
  }
  CloseSocket(found->second, false);
  return true;
}

bool XenonNetPipeBridge::CloseServer(const std::string& server_id) {
  auto found = servers_.find(server_id);
  if (found == servers_.end()) {
    return false;
  }
  CloseServer(found->second, true);
  return true;
}

bool XenonNetPipeBridge::HasSocket(const std::string& socket_id) const {
  return sockets_.contains(socket_id);
}

bool XenonNetPipeBridge::HasServer(const std::string& server_id) const {
  return servers_.contains(server_id);
}

bool XenonNetPipeBridge::HasListenerForPath(const std::string& path) const {
  for (const auto& [id, server] : servers_) {
    if (server && PipePathsMatch(server->path, path)) {
      return true;
    }
  }
  return false;
}

void XenonNetPipeBridge::RemoveEndpoint(const std::string& container_id,
                                        const std::string& endpoint_id) {
  std::vector<PipeSocket*> sockets;
  for (const auto& [id, socket] : sockets_) {
    if (socket->container_id == container_id &&
        socket->endpoint_id == endpoint_id) {
      sockets.push_back(socket);
    }
  }
  for (PipeSocket* socket : sockets) {
    CloseSocket(socket, false);
  }
  std::vector<PipeServer*> servers;
  for (const auto& [id, server] : servers_) {
    if (server->container_id == container_id &&
        server->endpoint_id == endpoint_id) {
      servers.push_back(server);
    }
  }
  for (PipeServer* server : servers) {
    CloseServer(server, false);
  }
}

void XenonNetPipeBridge::PumpLoop() {
  uv_run(loop_, UV_RUN_NOWAIT);
}

void XenonNetPipeBridge::Dispatch(const std::string& container_id,
                                  const std::string& endpoint_id,
                                  const std::string& channel,
                                  base::DictValue payload) {
  if (!shutting_down_ && event_callback_) {
    event_callback_.Run(container_id, endpoint_id, channel,
                        base::Value(std::move(payload)));
  }
}

void XenonNetPipeBridge::CloseSocket(PipeSocket* socket,
                                     bool notify_renderer) {
  if (!socket || uv_is_closing(
                     reinterpret_cast<uv_handle_t*>(&socket->handle))) {
    return;
  }
  sockets_.erase(socket->socket_id);
  uv_read_stop(reinterpret_cast<uv_stream_t*>(&socket->handle));
  if (notify_renderer) {
    base::DictValue payload;
    payload.Set("toId", socket->js_socket_id.empty() ? socket->socket_id
                                                     : socket->js_socket_id);
    Dispatch(socket->container_id, socket->endpoint_id,
             "__xenon:net:close", std::move(payload));
  }
  uv_close(reinterpret_cast<uv_handle_t*>(&socket->handle),
           DeleteSocketHandle);
}

void XenonNetPipeBridge::CloseServer(PipeServer* server,
                                     bool notify_renderer) {
  if (!server || uv_is_closing(
                     reinterpret_cast<uv_handle_t*>(&server->handle))) {
    return;
  }
  servers_.erase(server->server_id);
  std::vector<PipeSocket*> sockets;
  for (const auto& [id, socket] : sockets_) {
    if (socket->server_id == server->server_id) {
      sockets.push_back(socket);
    }
  }
  for (PipeSocket* socket : sockets) {
    CloseSocket(socket, notify_renderer);
  }
  if (notify_renderer) {
    base::DictValue payload;
    payload.Set("serverId", server->server_id);
    Dispatch(server->container_id, server->endpoint_id,
             "__xenon:net:server-closed", std::move(payload));
  }
  uv_close(reinterpret_cast<uv_handle_t*>(&server->handle),
           DeleteServerHandle);
}

}  // namespace xenon
