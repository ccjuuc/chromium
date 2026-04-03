// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_SOCKET_LEAF_CLIENT_SOCKET_H_
#define NET_SOCKET_LEAF_CLIENT_SOCKET_H_

#include <stdint.h>

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "net/base/completion_once_callback.h"
#include "net/base/io_buffer.h"
#include "net/base/host_port_pair.h"
#include "net/base/net_export.h"
#include "net/log/net_log_with_source.h"
#include "net/socket/leaf_outbound_protocol.h"
#include "net/socket/stream_socket.h"
#include "net/traffic_annotation/network_traffic_annotation.h"

namespace net {

// StreamSocket: TCP (and optional WebSocket-framed) VLESS outbound handshake on
// the transport matches SOCKS5ClientSocket — async Read/Write, no raw
// PlatformSocketDescriptor.
class NET_EXPORT_PRIVATE LeafClientSocket : public StreamSocket {
 public:
  LeafClientSocket(std::unique_ptr<StreamSocket> transport_socket,
                   const HostPortPair& destination,
                   LeafOutboundProtocol protocol,
                   std::string leaf_credential,
                   std::string leaf_uri_query,
                   std::string leaf_uri_fragment,
                   std::string leaf_proxy_authority_host,
                   const NetworkTrafficAnnotationTag& traffic_annotation);

  LeafClientSocket(const LeafClientSocket&) = delete;
  LeafClientSocket& operator=(const LeafClientSocket&) = delete;

  ~LeafClientSocket() override;

  int Connect(CompletionOnceCallback callback) override;
  void Disconnect() override;
  bool IsConnected() const override;
  bool IsConnectedAndIdle() const override;
  const NetLogWithSource& NetLog() const override;
  bool WasEverUsed() const override;
  NextProto GetNegotiatedProtocol() const override;
  bool GetSSLInfo(SSLInfo* ssl_info) override;
  int64_t GetTotalReceivedBytes() const override;
  void ApplySocketTag(const SocketTag& tag) override;

  int Read(IOBuffer* buf,
           int buf_len,
           CompletionOnceCallback callback) override;
  int ReadIfReady(IOBuffer* buf,
                  int buf_len,
                  CompletionOnceCallback callback) override;
  int CancelReadIfReady() override;
  int Write(IOBuffer* buf,
            int buf_len,
            CompletionOnceCallback callback,
            const NetworkTrafficAnnotationTag& traffic_annotation) override;

  int SetReceiveBufferSize(int32_t size) override;
  int SetSendBufferSize(int32_t size) override;

  int GetPeerAddress(IPEndPoint* address) const override;
  int GetLocalAddress(IPEndPoint* address) const override;

 private:
  enum class HandshakeState {
    kNone = 0,
    kTcpWriteVless,
    kTcpWriteVlessComplete,
    kWsWriteHttp,
    kWsWriteHttpComplete,
    kWsReadHeaders,
    kWsReadHeadersComplete,
    kWsWriteFrameComplete,
  };

  int ReadWithoutFraming(IOBuffer* buf,
                         int buf_len,
                         CompletionOnceCallback callback);
  int ReadWithWsFraming(IOBuffer* buf,
                        int buf_len,
                        CompletionOnceCallback callback);
  void OnWsTransportRead(int result);
  void OnWsTransportWrite(int result);
  void OnTcpVlessStripReadComplete(scoped_refptr<IOBuffer> user_buf,
                                   int user_buf_len,
                                   int result);
  void OnTcpStripFinalReadComplete(int result);
  void CompleteTcpVlessStripAndIssueUserRead(scoped_refptr<IOBuffer> user_buf,
                                             int user_buf_len);
  void IssueTcpVlessAddonRead(scoped_refptr<IOBuffer> user_buf,
                              int user_buf_len);

  void OnHandshakeIOComplete(int result);
  int DoHandshakeLoop(int result);

  bool PopOneWebSocketFrame(std::vector<uint8_t>* accumulator,
                            std::vector<uint8_t>* payload_out,
                            bool* fin_out,
                            uint8_t* opcode_out);

  bool PullNextCompleteWsMessageIntoPending();

  std::unique_ptr<StreamSocket> transport_socket_;
  HostPortPair destination_;
  const LeafOutboundProtocol protocol_;
  const std::string leaf_credential_;
  std::string leaf_uri_query_;
  std::string leaf_uri_fragment_;
  std::string leaf_proxy_authority_host_;
  NetworkTrafficAnnotationTag traffic_annotation_;
  NetLogWithSource net_log_;
  bool completed_handshake_ = false;

  HandshakeState handshake_state_ = HandshakeState::kNone;
  CompletionOnceCallback user_connect_callback_;
  base::RepeatingCallback<void(int)> handshake_io_callback_;

  scoped_refptr<DrainableIOBuffer> handshake_write_buf_;
  scoped_refptr<IOBufferWithSize> handshake_read_buf_;
  std::string handshake_http_headers_;
  std::vector<uint8_t> handshake_vless_hdr_;
  std::string handshake_type_;
  std::string handshake_ws_http_str_;
  std::array<uint8_t, 2> handshake_tcp_prefix_{};
  int handshake_tcp_prefix_read_ = 0;
  // -1 = have not yet applied addon length from the 2-byte VLESS response
  // header; 0 = no addon bytes left to discard; >0 = bytes of addon still to
  // read and discard (after prefix is complete).
  int vless_tcp_addon_remaining_ = -1;

  bool ws_framing_ = false;
  bool need_strip_vless_response_ = false;
  std::vector<uint8_t> pending_plaintext_;
  std::vector<uint8_t> ws_rx_accumulator_;
  bool ws_in_fragment_message_ = false;
  std::vector<uint8_t> ws_message_fragment_;

  scoped_refptr<IOBuffer> ws_read_buf_;
  static constexpr int kWsReadChunk = 32 * 1024;
  static constexpr int kHandshakeHeaderReadChunk = 4096;
  raw_ptr<char> ws_user_read_dst_ = nullptr;
  int ws_user_read_len_ = 0;
  // Pending user Read() callback (WS path or TCP VLESS response strip).
  CompletionOnceCallback pending_read_callback_;

  // WS write state: keeps the framed buffer alive during async transport writes.
  scoped_refptr<DrainableIOBuffer> ws_pending_write_buf_;
  int ws_pending_write_original_len_ = 0;
  CompletionOnceCallback ws_pending_write_callback_;

  base::WeakPtrFactory<LeafClientSocket> weak_factory_{this};
};

}  // namespace net

#endif  // NET_SOCKET_LEAF_CLIENT_SOCKET_H_
