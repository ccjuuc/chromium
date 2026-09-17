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
  const char* text = uv_err_name(status);
  return text ? text : "unknown libuv error";
}

std::string ServerKey(const std::string& container_id,
                      const std::string& endpoint_id,
                      const std::string& server_id) {
  return container_id + '\0' + endpoint_id + '\0' + server_id;
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
  uv_tcp_t tcp_handle{};
  bool tcp = false;
  bool closing = false;
  bool handle_closed = false;
  bool registered = false;
  bool notify_close = false;
  size_t connections = 0;
  uv_stream_t* stream() {
    return tcp ? reinterpret_cast<uv_stream_t*>(&tcp_handle)
               : reinterpret_cast<uv_stream_t*>(&handle);
  }
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
  uv_tcp_t tcp_handle{};
  bool tcp = false;
  bool read_ended = false;
  bool read_paused = false;
  bool shutdown_started = false;
  bool shutdown_complete = false;
  raw_ptr<PipeServer> server = nullptr;
  uv_connect_t connect_request{};
  uv_shutdown_t shutdown_request{};
  uv_stream_t* stream() {
    return tcp ? reinterpret_cast<uv_stream_t*>(&tcp_handle)
               : reinterpret_cast<uv_stream_t*>(&handle);
  }
};

namespace {

struct PipeWriteRequest {
  uv_write_t request{};
  std::string bytes;
  int write_id = 0;
};

bool PipePathsMatch(const std::string& left, const std::string& right) {
#if BUILDFLAG(IS_WIN)
  return base::EqualsCaseInsensitiveASCII(left, right);
#else
  return left == right;
#endif
}

bool TcpAddress(const std::string& host, int port, sockaddr_storage* address) {
  if (port < 0 || port > 65535) {
    return false;
  }
  return uv_ip4_addr(host.c_str(), port,
                     reinterpret_cast<sockaddr_in*>(address)) == 0 ||
         uv_ip6_addr(host.c_str(), port,
                     reinterpret_cast<sockaddr_in6*>(address)) == 0;
}

base::DictValue AddressValue(const sockaddr_storage& address) {
  char host[64] = {};
  int port = 0;
  if (address.ss_family == AF_INET6) {
    const auto* value = reinterpret_cast<const sockaddr_in6*>(&address);
    uv_ip6_name(value, host, sizeof(host));
    port = ntohs(value->sin6_port);
  } else {
    const auto* value = reinterpret_cast<const sockaddr_in*>(&address);
    uv_ip4_name(value, host, sizeof(host));
    port = ntohs(value->sin_port);
  }
  return base::DictValue()
      .Set("address", host)
      .Set("port", port)
      .Set("family", address.ss_family == AF_INET6 ? "IPv6" : "IPv4");
}

void AddSocketAddresses(uv_tcp_t* handle, base::DictValue* payload) {
  sockaddr_storage address{};
  int length = sizeof(address);
  if (uv_tcp_getsockname(handle, reinterpret_cast<sockaddr*>(&address),
                         &length) == 0) {
    payload->Set("local", AddressValue(address));
  }
  length = sizeof(address);
  if (uv_tcp_getpeername(handle, reinterpret_cast<sockaddr*>(&address),
                         &length) == 0) {
    payload->Set("remote", AddressValue(address));
  }
}

}  // namespace

// static
void XenonNetPipeBridge::DeleteServerHandle(uv_handle_t* handle) {
  auto* server = static_cast<PipeServer*>(handle->data);
  if (!server->registered) {
    delete server;
    return;
  }
  server->handle_closed = true;
  server->owner->CompleteServerClose(server);
}

// static
void XenonNetPipeBridge::DeleteSocketHandle(uv_handle_t* handle) {
  auto* socket = static_cast<PipeSocket*>(handle->data);
  auto* server = socket->server.get();
  auto* owner = socket->owner.get();
  delete socket;
  if (server) {
    --server->connections;
    owner->CompleteServerClose(server);
  }
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
    wire.Set("d", base::Base64Encode(std::string_view(
                      buffer->base, static_cast<size_t>(count))));
    base::DictValue payload;
    payload.Set("toId", socket->js_socket_id.empty() ? socket->socket_id
                                                     : socket->js_socket_id);
    payload.Set("wire", std::move(wire));
    socket->owner->Dispatch(socket->container_id, socket->endpoint_id,
                            "__xenon:net:data", std::move(payload));
    return;
  }
  if (count == UV_EOF && socket->tcp) {
    uv_read_stop(stream);
    socket->read_ended = true;
    base::DictValue payload;
    payload.Set("toId", socket->js_socket_id.empty() ? socket->socket_id
                                                     : socket->js_socket_id);
    socket->owner->Dispatch(socket->container_id, socket->endpoint_id,
                            "__xenon:net:end", std::move(payload));
    if (socket->shutdown_complete) {
      socket->owner->CloseSocket(socket, true);
    }
  } else if (count < 0) {
    if (count != UV_EOF) {
      base::DictValue payload;
      payload.Set("toId", socket->js_socket_id.empty() ? socket->socket_id
                                                       : socket->js_socket_id);
      payload.Set("code", UvError(static_cast<int>(count)));
      socket->owner->Dispatch(socket->container_id, socket->endpoint_id,
                              "__xenon:net:error", std::move(payload));
    }
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
  socket->socket_id =
      "native-" + std::to_string(server->owner->next_socket_id_++);
  socket->tcp = server->tcp;
  const int init_status =
      socket->tcp
          ? uv_tcp_init(server->owner->loop_, &socket->tcp_handle)
          : uv_pipe_init(server->owner->loop_, &socket->handle, /*ipc=*/0);
  if (init_status < 0) {
    delete socket;
    return;
  }
  socket->stream()->data = socket;
  const int accept_status = uv_accept(stream, socket->stream());
  if (accept_status < 0) {
    uv_close(reinterpret_cast<uv_handle_t*>(socket->stream()),
             DeleteSocketHandle);
    return;
  }
  socket->server = server;
  ++server->connections;
  server->owner->sockets_.emplace(socket->socket_id, socket);

  base::DictValue payload;
  payload.Set("serverId", server->server_id);
  payload.Set("socketId", socket->socket_id);
  if (socket->tcp) {
    AddSocketAddresses(&socket->tcp_handle, &payload);
  }
  server->owner->Dispatch(server->container_id, server->endpoint_id,
                          "__xenon:net:connection", std::move(payload));
  // A synchronous native event consumer may pause/resume or close the socket
  // while handling connection. Do not reopen it or treat an existing read as
  // a connection failure when control returns here.
  if (uv_is_closing(reinterpret_cast<uv_handle_t*>(socket->stream()))) {
    return;
  }
  const int read_status =
      socket->read_paused
          ? 0
          : uv_read_start(socket->stream(), AllocateReadBuffer, OnPipeRead);
  if (read_status < 0 && read_status != UV_EALREADY) {
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
    payload.Set("code", socket->tcp ? UvError(status) : "ECONNREFUSED");
    owner->Dispatch(socket->container_id, socket->endpoint_id,
                    "__xenon:net:error", std::move(payload));
    owner->CloseSocket(socket, false);
    return;
  }

  base::DictValue payload;
  payload.Set("toId", socket->js_socket_id);
  payload.Set("peerId", socket->socket_id);
  if (socket->tcp) {
    AddSocketAddresses(&socket->tcp_handle, &payload);
  }
  owner->Dispatch(socket->container_id, socket->endpoint_id,
                  "__xenon:net:connected", std::move(payload));
  if (uv_is_closing(reinterpret_cast<uv_handle_t*>(socket->stream()))) {
    return;
  }
  const int read_status =
      socket->read_paused
          ? 0
          : uv_read_start(socket->stream(), AllocateReadBuffer, OnPipeRead);
  if (read_status < 0 && read_status != UV_EALREADY) {
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
                                std::string* error,
                                int permission_flags) {
  if (server_id.empty() || path.empty()) {
    *error = "named-pipe server id and path are required";
    return false;
  }
  if (servers_.contains(ServerKey(container_id, endpoint_id, server_id))) {
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
  if (status >= 0 && permission_flags != 0) {
    status = uv_pipe_chmod(&server->handle, permission_flags);
  }
  if (status < 0) {
    *error = UvError(status);
    uv_close(reinterpret_cast<uv_handle_t*>(&server->handle),
             DeleteServerHandle);
    uv_run(loop_, UV_RUN_NOWAIT);
    return false;
  }
  server->registered = true;
  servers_.emplace(ServerKey(container_id, endpoint_id, server_id), server);
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
  socket->stream()->data = socket;
  sockets_.emplace(socket->socket_id, socket);
  uv_pipe_connect(&socket->connect_request, &socket->handle, path.c_str(),
                  OnPipeConnect);
  PumpLoop();
  return true;
}

bool XenonNetPipeBridge::ListenTcp(const std::string& container_id,
                                   const std::string& endpoint_id,
                                   const std::string& server_id,
                                   const std::string& host,
                                   int port,
                                   std::string* error) {
  sockaddr_storage address{};
  if (server_id.empty() ||
      servers_.contains(ServerKey(container_id, endpoint_id, server_id))) {
    *error = "EADDRINUSE";
    return false;
  }
  if (!TcpAddress(host.empty() ? "0.0.0.0" : host, port, &address)) {
    *error = "ERR_NOT_SUPPORTED";
    return false;
  }
  auto* server = new PipeServer();
  server->owner = this;
  server->container_id = container_id;
  server->endpoint_id = endpoint_id;
  server->server_id = server_id;
  server->tcp = true;
  int status = uv_tcp_init(loop_, &server->tcp_handle);
  if (status < 0) {
    *error = UvError(status);
    delete server;
    return false;
  }
  server->stream()->data = server;
  status = uv_tcp_bind(&server->tcp_handle,
                       reinterpret_cast<sockaddr*>(&address), 0);
  if (status >= 0) {
    status = uv_listen(server->stream(), kPipeBacklog, OnPipeConnection);
  }
  if (status < 0) {
    *error = UvError(status);
    uv_close(reinterpret_cast<uv_handle_t*>(server->stream()),
             DeleteServerHandle);
    return false;
  }
  server->registered = true;
  servers_.emplace(ServerKey(container_id, endpoint_id, server_id), server);
  return true;
}

bool XenonNetPipeBridge::ConnectTcp(const std::string& container_id,
                                    const std::string& endpoint_id,
                                    const std::string& client_socket_id,
                                    const std::string& host,
                                    int port,
                                    std::string* error) {
  sockaddr_storage address{};
  if (!TcpAddress(host.empty() ? "127.0.0.1" : host, port, &address)) {
    *error = "ERR_NOT_SUPPORTED";
    return false;
  }
  auto* socket = new PipeSocket();
  socket->owner = this;
  socket->container_id = container_id;
  socket->endpoint_id = endpoint_id;
  socket->js_socket_id = client_socket_id;
  socket->socket_id = "native-" + std::to_string(next_socket_id_++);
  socket->tcp = true;
  int status = uv_tcp_init(loop_, &socket->tcp_handle);
  if (status < 0) {
    *error = UvError(status);
    delete socket;
    return false;
  }
  socket->stream()->data = socket;
  sockets_.emplace(socket->socket_id, socket);
  status = uv_tcp_connect(&socket->connect_request, &socket->tcp_handle,
                          reinterpret_cast<sockaddr*>(&address), OnPipeConnect);
  if (status < 0) {
    *error = UvError(status);
    CloseSocket(socket, false);
    return false;
  }
  return true;
}

XenonNetPipeBridge::PipeServer* XenonNetPipeBridge::FindServer(
    const std::string& server_id,
    const std::string& container_id,
    const std::string& endpoint_id) const {
  if (!container_id.empty() || !endpoint_id.empty()) {
    const auto found =
        servers_.find(ServerKey(container_id, endpoint_id, server_id));
    return found == servers_.end() ? nullptr : found->second;
  }
  for (const auto& [key, server] : servers_) {
    if (server->server_id == server_id) {
      return server;
    }
  }
  return nullptr;
}

base::Value XenonNetPipeBridge::ServerAddress(
    const std::string& server_id,
    const std::string& container_id,
    const std::string& endpoint_id) const {
  auto* server = FindServer(server_id, container_id, endpoint_id);
  if (!server) {
    return base::Value();
  }
  if (!server->tcp) {
    return base::Value(server->path);
  }
  sockaddr_storage address{};
  int size = sizeof(address);
  if (uv_tcp_getsockname(&server->tcp_handle,
                         reinterpret_cast<sockaddr*>(&address), &size)) {
    return base::Value();
  }
  return base::Value(AddressValue(address));
}

void XenonNetPipeBridge::OnSocketWrite(uv_write_t* request, int status) {
  std::unique_ptr<PipeWriteRequest> write(
      reinterpret_cast<PipeWriteRequest*>(request));
  auto* socket = static_cast<PipeSocket*>(request->handle->data);
  if (write->write_id) {
    base::DictValue payload;
    payload.Set("toId", socket->js_socket_id.empty() ? socket->socket_id
                                                     : socket->js_socket_id);
    payload.Set("writeId", write->write_id);
    if (status < 0) {
      payload.Set("code", UvError(status));
    }
    socket->owner->Dispatch(socket->container_id, socket->endpoint_id,
                            "__xenon:net:written", std::move(payload));
  }
  if (status < 0 && status != UV_ECANCELED &&
      !uv_is_closing(reinterpret_cast<uv_handle_t*>(socket->stream()))) {
    // A write failure is a socket error even when the caller did not request
    // a per-write callback. Report it before close so JS sets hadError.
    base::DictValue payload;
    payload.Set("toId", socket->js_socket_id.empty() ? socket->socket_id
                                                     : socket->js_socket_id);
    payload.Set("code", UvError(status));
    socket->owner->Dispatch(socket->container_id, socket->endpoint_id,
                            "__xenon:net:error", std::move(payload));
    // A synchronous event consumer may have already removed the endpoint.
    // CloseSocket checks uv_is_closing before touching the handle again.
    socket->owner->CloseSocket(socket, true);
  }
}

bool XenonNetPipeBridge::SetSocketReadPaused(const std::string& socket_id,
                                             bool paused) {
  auto found = sockets_.find(socket_id);
  if (found == sockets_.end()) {
    return false;
  }
  auto* socket = found->second;
  if (socket->read_ended || socket->read_paused == paused) {
    return true;
  }
  socket->read_paused = paused;
  const int status =
      paused ? uv_read_stop(socket->stream())
             : uv_read_start(socket->stream(), AllocateReadBuffer, OnPipeRead);
  if (status < 0 && status != UV_EALREADY) {
    base::DictValue payload;
    payload.Set("toId", socket->js_socket_id.empty() ? socket->socket_id
                                                     : socket->js_socket_id);
    payload.Set("code", UvError(status));
    Dispatch(socket->container_id, socket->endpoint_id, "__xenon:net:error",
             std::move(payload));
    CloseSocket(socket, true);
  }
  return true;
}

bool XenonNetPipeBridge::EndSocket(const std::string& socket_id) {
  auto found = sockets_.find(socket_id);
  if (found == sockets_.end()) {
    return false;
  }
  auto* socket = found->second;
  if (socket->shutdown_started) {
    return true;
  }
  socket->shutdown_started = true;
  int status = uv_shutdown(&socket->shutdown_request, socket->stream(),
                           OnSocketShutdown);
  if (status < 0) {
    CloseSocket(socket, true);
  }
  return true;
}

void XenonNetPipeBridge::OnSocketShutdown(uv_shutdown_t* request, int status) {
  auto* socket = static_cast<PipeSocket*>(request->handle->data);
  if (uv_is_closing(reinterpret_cast<uv_handle_t*>(socket->stream()))) {
    return;
  }
  if (status < 0) {
    socket->owner->CloseSocket(socket, true);
    return;
  }
  socket->shutdown_complete = true;
  base::DictValue payload;
  payload.Set("toId", socket->js_socket_id.empty() ? socket->socket_id
                                                   : socket->js_socket_id);
  socket->owner->Dispatch(socket->container_id, socket->endpoint_id,
                          "__xenon:net:finish", std::move(payload));
  if (socket->read_ended || !socket->tcp) {
    socket->owner->CloseSocket(socket, true);
  }
}

bool XenonNetPipeBridge::Write(const std::string& socket_id,
                               const base::DictValue& wire,
                               std::string* error,
                               int write_id) {
  auto found = sockets_.find(socket_id);
  if (found == sockets_.end()) {
    return false;
  }
  auto write = std::make_unique<PipeWriteRequest>();
  write->write_id = write_id;
  if (!WireToBytes(wire, &write->bytes, error)) {
    return true;
  }
  uv_buf_t buffer = uv_buf_init(write->bytes.data(),
                                static_cast<unsigned int>(write->bytes.size()));
  const int status = uv_write(&write->request, found->second->stream(), &buffer,
                              1, OnSocketWrite);
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
  CloseSocket(found->second, true);
  return true;
}

bool XenonNetPipeBridge::CloseServer(const std::string& server_id,
                                     const std::string& container_id,
                                     const std::string& endpoint_id) {
  auto* server = FindServer(server_id, container_id, endpoint_id);
  if (!server) {
    return false;
  }
  CloseServer(server, true);
  return true;
}

bool XenonNetPipeBridge::OwnsSocket(const std::string& socket_id,
                                    const std::string& container_id,
                                    const std::string& endpoint_id) const {
  auto found = sockets_.find(socket_id);
  return found != sockets_.end() &&
         found->second->container_id == container_id &&
         found->second->endpoint_id == endpoint_id;
}

bool XenonNetPipeBridge::HasSocket(const std::string& socket_id) const {
  return sockets_.contains(socket_id);
}

bool XenonNetPipeBridge::HasServer(const std::string& server_id) const {
  return FindServer(server_id, "", "") != nullptr;
}

bool XenonNetPipeBridge::HasListenerForPath(const std::string& path) const {
  for (const auto& [id, server] : servers_) {
    if (server && !server->closing && !server->tcp &&
        PipePathsMatch(server->path, path)) {
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

void XenonNetPipeBridge::CloseSocket(PipeSocket* socket, bool notify_renderer) {
  if (!socket ||
      uv_is_closing(reinterpret_cast<uv_handle_t*>(socket->stream()))) {
    return;
  }
  sockets_.erase(socket->socket_id);
  uv_read_stop(socket->stream());
  if (notify_renderer) {
    base::DictValue payload;
    payload.Set("toId", socket->js_socket_id.empty() ? socket->socket_id
                                                     : socket->js_socket_id);
    Dispatch(socket->container_id, socket->endpoint_id, "__xenon:net:close",
             std::move(payload));
  }
  uv_close(reinterpret_cast<uv_handle_t*>(socket->stream()),
           DeleteSocketHandle);
}

void XenonNetPipeBridge::CloseServer(PipeServer* server, bool notify_renderer) {
  if (!server || server->closing) {
    return;
  }
  server->closing = true;
  server->notify_close = notify_renderer;
  // TCP stops accepting while established sockets drain; preserve pipe
  // behavior.
  if (!server->tcp || !notify_renderer) {
    std::vector<PipeSocket*> sockets;
    for (const auto& [id, socket] : sockets_) {
      if (socket->server == server) {
        sockets.push_back(socket);
      }
    }
    for (auto* socket : sockets) {
      CloseSocket(socket, notify_renderer);
    }
  }
  uv_close(reinterpret_cast<uv_handle_t*>(server->stream()),
           DeleteServerHandle);
}

void XenonNetPipeBridge::CompleteServerClose(PipeServer* server) {
  if (!server->handle_closed || server->connections) {
    return;
  }
  if (server->notify_close) {
    base::DictValue payload;
    payload.Set("serverId", server->server_id);
    Dispatch(server->container_id, server->endpoint_id,
             "__xenon:net:server-closed", std::move(payload));
  }
  servers_.erase(
      ServerKey(server->container_id, server->endpoint_id, server->server_id));
  delete server;
}

}  // namespace xenon
