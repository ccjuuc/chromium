// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license.
#ifndef XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_UDP_SOCKET_BRIDGE_H_
#define XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_UDP_SOCKET_BRIDGE_H_

#include <map>
#include <string>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/sequence_checker.h"
#include "base/types/expected.h"
#include "base/values.h"
#include "xenon_overlay/third_party/libuv/include/uv.h"

namespace xenon::ipc {

// Per-runtime OS datagram sockets. The owner pumps its existing native addon
// loop, calls Shutdown() before draining it, and retains this bridge until all
// close callbacks have completed. DNS resolution belongs to the JS dns API.
class XenonUdpSocketBridge {
 public:
  using EventCallback = base::RepeatingCallback<void(base::Value)>;
  XenonUdpSocketBridge(uv_loop_t* loop, EventCallback callback);
  ~XenonUdpSocketBridge();
  XenonUdpSocketBridge(const XenonUdpSocketBridge&) = delete;
  XenonUdpSocketBridge& operator=(const XenonUdpSocketBridge&) = delete;

  base::expected<base::Value, std::string> Call(const base::DictValue& request);
  void Shutdown();
  bool IsDrained() const;

 private:
  struct Socket;
  struct Send;
  struct Lookup;
  static void Allocate(uv_handle_t*, size_t suggested, uv_buf_t* buffer);
  static void Receive(uv_udp_t*,
                      ssize_t count,
                      const uv_buf_t*,
                      const sockaddr*,
                      unsigned flags);
  static void Sent(uv_udp_send_t*, int status);
  static void Closed(uv_handle_t*);
  static void Resolved(uv_getaddrinfo_t*, int status, addrinfo* addresses);
  void Emit(Socket* socket, base::DictValue event);
  void Close(Socket* socket);

  raw_ptr<uv_loop_t> loop_;
  EventCallback callback_;
  std::map<int, Socket*> sockets_;
  std::map<int, Lookup*> lookups_;
  int next_id_ = 1;
  bool shutting_down_ = false;
  SEQUENCE_CHECKER(sequence_checker_);
};
}  // namespace xenon::ipc
#endif
