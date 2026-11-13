// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license.
#include "xenon_overlay/chrome/browser/ipc/xenon_udp_socket_bridge.h"

#include <cstdlib>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/memory/raw_ptr.h"
#include "base/strings/string_number_conversions.h"

namespace xenon::ipc {
namespace {
std::string Error(int code, std::string_view operation) {
  return std::string(uv_err_name(code)) + ": " + std::string(operation) + " " +
         uv_strerror(code);
}
base::expected<base::Value, std::string> Result(int status,
                                                std::string_view operation) {
  if (status < 0) {
    return base::unexpected(Error(status, operation));
  }
  return base::Value();
}
int Endpoint(const base::DictValue& request,
             int family,
             sockaddr_storage* address) {
  const auto port = request.FindInt("port");
  const auto* host = request.FindString("address");
  if (!port || *port < 0 || *port > 65535 || !host ||
      host->find('\0') != std::string::npos) {
    return UV_EINVAL;
  }
  return family == AF_INET6
             ? uv_ip6_addr(host->c_str(), *port,
                           reinterpret_cast<sockaddr_in6*>(address))
             : uv_ip4_addr(host->c_str(), *port,
                           reinterpret_cast<sockaddr_in*>(address));
}
base::DictValue Address(const sockaddr* address) {
  char text[INET6_ADDRSTRLEN] = {};
  int port;
  const bool ipv6 = address->sa_family == AF_INET6;
  if (ipv6) {
    const auto* value = reinterpret_cast<const sockaddr_in6*>(address);
    uv_ip6_name(value, text, sizeof(text));
    port = ntohs(value->sin6_port);
  } else {
    const auto* value = reinterpret_cast<const sockaddr_in*>(address);
    uv_ip4_name(value, text, sizeof(text));
    port = ntohs(value->sin_port);
  }
  return base::DictValue()
      .Set("address", text)
      .Set("family", ipv6 ? "IPv6" : "IPv4")
      .Set("port", port);
}
}  // namespace

struct XenonUdpSocketBridge::Socket {
  uv_udp_t handle;
  raw_ptr<XenonUdpSocketBridge> owner;
  int id;
  int family;
  bool bound = false;
  bool connected = false;
  bool closing = false;
};
struct XenonUdpSocketBridge::Send {
  uv_udp_send_t request;
  raw_ptr<Socket> socket;
  int id;
  std::vector<char> bytes;
};
struct XenonUdpSocketBridge::Lookup {
  uv_getaddrinfo_t request;
  raw_ptr<XenonUdpSocketBridge> owner;
  int socket_id;
  int id;
};

XenonUdpSocketBridge::XenonUdpSocketBridge(uv_loop_t* loop,
                                           EventCallback callback)
    : loop_(loop), callback_(std::move(callback)) {
  CHECK(loop_);
}
XenonUdpSocketBridge::~XenonUdpSocketBridge() {
  CHECK(IsDrained());
}

base::expected<base::Value, std::string> XenonUdpSocketBridge::Call(
    const base::DictValue& request) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (shutting_down_) {
    return base::unexpected("ERR_SOCKET_DGRAM_NOT_RUNNING: runtime is closing");
  }
  const auto* operation = request.FindString("op");
  if (!operation) {
    return base::unexpected("EINVAL: missing UDP operation");
  }
  if (*operation == "create") {
    const auto family = request.FindInt("family");
    if (!family || (*family != 4 && *family != 6)) {
      return base::unexpected("EINVAL: UDP family must be 4 or 6");
    }
    if (sockets_.size() >= 1024 ||
        next_id_ == std::numeric_limits<int>::max()) {
      return base::unexpected("EMFILE: UDP socket limit reached");
    }
    auto socket = std::make_unique<Socket>();
    socket->owner = this;
    socket->family = *family == 6 ? AF_INET6 : AF_INET;
    socket->id = next_id_++;
    const int status = uv_udp_init_ex(loop_, &socket->handle, socket->family);
    if (status < 0) {
      return base::unexpected(Error(status, "socket"));
    }
    socket->handle.data = socket.get();
    const int id = socket->id;
    sockets_.emplace(id, socket.release());
    return base::Value(id);
  }
  const auto id = request.FindInt("id");
  const auto found = id ? sockets_.find(*id) : sockets_.end();
  if (found == sockets_.end() || found->second->closing) {
    return base::unexpected("ERR_SOCKET_DGRAM_NOT_RUNNING: socket is closed");
  }
  Socket* socket = found->second;
  if (*operation == "lookup") {
    const auto* host = request.FindString("hostname");
    const auto lookup_id = request.FindInt("lookupId");
    if (!host || host->empty() || host->size() > 4096 ||
        host->find('\0') != std::string::npos || !lookup_id ||
        *lookup_id <= 0 || lookups_.contains(*lookup_id)) {
      return base::unexpected("EINVAL: invalid UDP hostname lookup");
    }
    if (lookups_.size() >= 1024) {
      return base::unexpected("EAGAIN: too many pending UDP lookups");
    }
    auto lookup = std::make_unique<Lookup>();
    lookup->owner = this;
    lookup->socket_id = socket->id;
    lookup->id = *lookup_id;
    lookup->request.data = lookup.get();
    addrinfo hints = {};
    hints.ai_family = socket->family;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    const int status = uv_getaddrinfo(loop_, &lookup->request, Resolved,
                                      host->c_str(), nullptr, &hints);
    if (status < 0) {
      return base::unexpected(Error(status, "getaddrinfo"));
    }
    lookups_.emplace(*lookup_id, lookup.release());
    return base::Value();
  }
  if (*operation == "close") {
    Close(socket);
    return base::Value();
  }
  if (*operation == "ref" || *operation == "unref") {
    auto* handle = reinterpret_cast<uv_handle_t*>(&socket->handle);
    if (*operation == "ref") {
      uv_ref(handle);
    } else {
      uv_unref(handle);
    }
    return base::Value();
  }
  if (*operation == "address" || *operation == "remoteAddress") {
    sockaddr_storage address = {};
    int size = sizeof(address);
    int status =
        *operation == "address"
            ? uv_udp_getsockname(&socket->handle,
                                 reinterpret_cast<sockaddr*>(&address), &size)
            : uv_udp_getpeername(&socket->handle,
                                 reinterpret_cast<sockaddr*>(&address), &size);
    if (status < 0) {
      return base::unexpected(Error(status, *operation));
    }
    return base::Value(Address(reinterpret_cast<sockaddr*>(&address)));
  }
  if (*operation == "bind") {
    sockaddr_storage address = {};
    int status = Endpoint(request, socket->family, &address);
    unsigned flags =
        request.FindBool("reuseAddr").value_or(false) ? UV_UDP_REUSEADDR : 0;
    if (request.FindBool("ipv6Only").value_or(false)) {
      flags |= UV_UDP_IPV6ONLY;
    }
    if (request.FindBool("reusePort").value_or(false)) {
      return base::unexpected(
          "ENOTSUP: reusePort is unavailable in this libuv version");
    }
    if (status == 0) {
      status = uv_udp_bind(&socket->handle,
                           reinterpret_cast<const sockaddr*>(&address), flags);
    }
    if (status == 0) {
      socket->bound = true;
      status = uv_udp_recv_start(&socket->handle, Allocate, Receive);
    }
    return Result(status, "bind");
  }
  if (*operation == "connect" || *operation == "disconnect") {
    sockaddr_storage address = {};
    int status = *operation == "disconnect"
                     ? uv_udp_connect(&socket->handle, nullptr)
                     : Endpoint(request, socket->family, &address);
    if (*operation == "connect" && status == 0) {
      status = uv_udp_connect(&socket->handle,
                              reinterpret_cast<const sockaddr*>(&address));
    }
    if (status == 0) {
      socket->connected = *operation == "connect";
    }
    return Result(status, *operation);
  }
  if (*operation == "send") {
    const auto* bytes = request.FindBlob("bytes");
    const auto send_id = request.FindInt("sendId");
    if (!bytes || !send_id || *send_id <= 0 || bytes->size() > 65536) {
      return base::unexpected("EMSGSIZE: invalid UDP datagram");
    }
    sockaddr_storage address = {};
    if (!socket->connected) {
      const int status = Endpoint(request, socket->family, &address);
      if (status < 0) {
        return base::unexpected(Error(status, "send"));
      }
    }
    auto send = std::make_unique<Send>();
    send->socket = socket;
    send->id = *send_id;
    send->bytes.assign(bytes->begin(), bytes->end());
    send->request.data = send.get();
    uv_buf_t buffer = uv_buf_init(send->bytes.data(), send->bytes.size());
    const int status = uv_udp_send(
        &send->request, &socket->handle, &buffer, 1,
        socket->connected ? nullptr : reinterpret_cast<sockaddr*>(&address),
        Sent);
    if (status < 0) {
      return base::unexpected(Error(status, "send"));
    }
    send.release();
    return base::Value();
  }
  if (*operation == "getSendQueueSize") {
    return base::Value(
        static_cast<double>(uv_udp_get_send_queue_size(&socket->handle)));
  }
  if (*operation == "getSendQueueCount") {
    return base::Value(
        static_cast<double>(uv_udp_get_send_queue_count(&socket->handle)));
  }
  if (*operation == "setRecvBufferSize" || *operation == "setSendBufferSize" ||
      *operation == "getRecvBufferSize" || *operation == "getSendBufferSize") {
    int size = request.FindInt("value").value_or(0);
    if (operation->starts_with("set") && size <= 0) {
      return base::unexpected("EINVAL: buffer size must be positive");
    }
    const int status =
        operation->find("Recv") != std::string::npos
            ? uv_recv_buffer_size(
                  reinterpret_cast<uv_handle_t*>(&socket->handle), &size)
            : uv_send_buffer_size(
                  reinterpret_cast<uv_handle_t*>(&socket->handle), &size);
    if (status < 0) {
      return base::unexpected(Error(status, *operation));
    }
    return base::Value(size);
  }
  if (*operation == "setMulticastInterface") {
    const auto* value = request.FindString("interface");
    if (!value || value->find('\0') != std::string::npos) {
      return base::unexpected("EINVAL: invalid multicast interface");
    }
    return Result(
        uv_udp_set_multicast_interface(&socket->handle, value->c_str()),
        *operation);
  }
  if (*operation == "addMembership" || *operation == "dropMembership" ||
      *operation == "addSourceSpecificMembership" ||
      *operation == "dropSourceSpecificMembership") {
    const auto* group = request.FindString("group");
    const auto* interface = request.FindString("interface");
    const auto* source = request.FindString("source");
    if (!group || group->empty() || group->find('\0') != std::string::npos ||
        (interface && interface->find('\0') != std::string::npos) ||
        (source &&
         (source->empty() || source->find('\0') != std::string::npos))) {
      return base::unexpected("EINVAL: invalid multicast membership");
    }
    const auto action =
        operation->starts_with("add") ? UV_JOIN_GROUP : UV_LEAVE_GROUP;
    const int status =
        operation->find("Source") == std::string::npos
            ? uv_udp_set_membership(&socket->handle, group->c_str(),
                                    interface ? interface->c_str() : nullptr,
                                    action)
        : source ? uv_udp_set_source_membership(
                       &socket->handle, group->c_str(),
                       interface ? interface->c_str() : nullptr,
                       source->c_str(), action)
                 : UV_EINVAL;
    return Result(status, *operation);
  }
  const auto value = request.FindInt("value");
  if (!value) {
    return base::unexpected("EINVAL: missing UDP option value");
  }
  int status;
  if (*operation == "setBroadcast") {
    status = uv_udp_set_broadcast(&socket->handle, *value);
  } else if (*operation == "setTTL") {
    status = uv_udp_set_ttl(&socket->handle, *value);
  } else if (*operation == "setMulticastTTL") {
    status = uv_udp_set_multicast_ttl(&socket->handle, *value);
  } else if (*operation == "setMulticastLoopback") {
    status = uv_udp_set_multicast_loop(&socket->handle, *value);
  } else {
    return base::unexpected("ERR_NOT_SUPPORTED: unsupported UDP operation");
  }
  return Result(status, *operation);
}

void XenonUdpSocketBridge::Allocate(uv_handle_t*, size_t, uv_buf_t* buffer) {
  buffer->base = static_cast<char*>(malloc(65536));
  buffer->len = buffer->base ? 65536 : 0;
}
void XenonUdpSocketBridge::Receive(uv_udp_t* handle,
                                   ssize_t count,
                                   const uv_buf_t* buffer,
                                   const sockaddr* address,
                                   unsigned flags) {
  auto* socket = static_cast<Socket*>(handle->data);
  std::unique_ptr<char, decltype(&free)> bytes(buffer->base, free);
  if (count < 0) {
    socket->owner->Emit(socket, base::DictValue()
                                    .Set("type", "error")
                                    .Set("error", Error(count, "recvmsg")));
  } else if (address) {
    if (flags & UV_UDP_PARTIAL) {
      socket->owner->Emit(
          socket, base::DictValue()
                      .Set("type", "error")
                      .Set("error", "EMSGSIZE: truncated UDP datagram"));
      return;
    }
    base::Value::BlobStorage data;
    if (count) {
      // SAFETY: libuv reports at most buffer->len bytes in this allocation.
      data.assign(UNSAFE_BUFFERS(buffer->base),
                  UNSAFE_BUFFERS(buffer->base + count));
    }
    auto info = Address(address);
    info.Set("size", static_cast<int>(count));
    socket->owner->Emit(socket, base::DictValue()
                                    .Set("type", "message")
                                    .Set("bytes", base::Value(std::move(data)))
                                    .Set("rinfo", std::move(info)));
  }
}
void XenonUdpSocketBridge::Sent(uv_udp_send_t* request, int status) {
  std::unique_ptr<Send> send(static_cast<Send*>(request->data));
  auto event = base::DictValue()
                   .Set("type", "sent")
                   .Set("sendId", send->id)
                   .Set("bytes", static_cast<int>(send->bytes.size()));
  if (status < 0) {
    event.Set("error", Error(status, "send"));
  }
  send->socket->owner->Emit(send->socket, std::move(event));
}
void XenonUdpSocketBridge::Closed(uv_handle_t* handle) {
  std::unique_ptr<Socket> socket(static_cast<Socket*>(handle->data));
  auto* owner = socket->owner.get();
  owner->sockets_.erase(socket->id);
  owner->Emit(socket.get(), base::DictValue().Set("type", "close"));
}
void XenonUdpSocketBridge::Resolved(uv_getaddrinfo_t* request,
                                    int status,
                                    addrinfo* addresses) {
  std::unique_ptr<Lookup> lookup(static_cast<Lookup*>(request->data));
  auto* owner = lookup->owner.get();
  owner->lookups_.erase(lookup->id);
  auto event =
      base::DictValue().Set("type", "lookup").Set("lookupId", lookup->id);
  if (status < 0 || !addresses) {
    event.Set("error",
              Error(status < 0 ? status : UV_EAI_NONAME, "getaddrinfo"));
  } else {
    event.Set("result", Address(addresses->ai_addr));
  }
  if (addresses) {
    uv_freeaddrinfo(addresses);
  }
  const auto found = owner->sockets_.find(lookup->socket_id);
  if (found != owner->sockets_.end() && !found->second->closing) {
    owner->Emit(found->second, std::move(event));
  }
}
void XenonUdpSocketBridge::Emit(Socket* socket, base::DictValue event) {
  if (!shutting_down_) {
    event.Set("id", socket->id);
    callback_.Run(base::Value(std::move(event)));
  }
}
void XenonUdpSocketBridge::Close(Socket* socket) {
  if (socket->closing) {
    return;
  }
  socket->closing = true;
  for (const auto& [id, lookup] : lookups_) {
    if (lookup->socket_id == socket->id) {
      uv_cancel(reinterpret_cast<uv_req_t*>(&lookup->request));
    }
  }
  uv_udp_recv_stop(&socket->handle);
  uv_close(reinterpret_cast<uv_handle_t*>(&socket->handle), Closed);
}
void XenonUdpSocketBridge::Shutdown() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  shutting_down_ = true;
  for (const auto& [id, socket] : sockets_) {
    Close(socket);
  }
}
bool XenonUdpSocketBridge::IsDrained() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return sockets_.empty() && lookups_.empty();
}
}  // namespace xenon::ipc
