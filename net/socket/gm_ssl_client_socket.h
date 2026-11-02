// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_SOCKET_GM_SSL_CLIENT_SOCKET_H_
#define NET_SOCKET_GM_SSL_CLIENT_SOCKET_H_

#include <memory>
#include <optional>
#include <vector>

#include "base/callback_list.h"
#include "base/containers/span.h"
#include "base/functional/callback_helpers.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "net/base/host_port_pair.h"
#include "net/base/io_buffer.h"
#include "net/gm/gm_client_context.h"
#include "net/gm/gm_client_identity.h"
#include "net/socket/ssl_client_socket.h"
#include "net/ssl/ssl_config.h"
#include "net/ssl/ssl_info.h"

namespace net {

// Transport-neutral national TLS adapter. It wraps the same TCP/proxy stream
// as the ordinary TLS socket; no HTTP proxy conversion or vendor SDK runs here.
class NET_EXPORT GmSSLClientSocket final : public SSLClientSocket {
 public:
  GmSSLClientSocket(SSLClientContext* ssl_context,
                    gm::ClientContext* gm_context,
                    gm::ClientIdentityProvider* identity_provider,
                    std::unique_ptr<StreamSocket> transport,
                    const HostPortPair& server,
                    const SSLConfig& config);
  ~GmSSLClientSocket() override;

  int Connect(CompletionOnceCallback callback) override;
  void Disconnect() override;
  bool IsConnected() const override;
  bool IsConnectedAndIdle() const override;
  int GetPeerAddress(IPEndPoint* address) const override;
  int GetLocalAddress(IPEndPoint* address) const override;
  const NetLogWithSource& NetLog() const override;
  bool WasEverUsed() const override;
  NextProto GetNegotiatedProtocol() const override;
  bool GetSSLInfo(SSLInfo* info) override;
  void PopulateNetErrorDetails(NetErrorDetails* details) const override;
  int64_t GetTotalReceivedBytes() const override;
  void ApplySocketTag(const SocketTag& tag) override;
  int Read(IOBuffer* buffer,
           int length,
           CompletionOnceCallback callback) override;
  int Write(IOBuffer* buffer,
            int length,
            CompletionOnceCallback callback,
            const NetworkTrafficAnnotationTag& traffic_annotation) override;
  int SetReceiveBufferSize(int32_t size) override;
  int SetSendBufferSize(int32_t size) override;
  int ExportKeyingMaterial(std::string_view label,
                           std::optional<base::span<const uint8_t>> context,
                           base::span<uint8_t> out) override;
  std::vector<uint8_t> GetECHRetryConfigs() override;
  std::vector<std::vector<uint8_t>> GetServerTrustAnchorIDs() override;

 private:
  void OnIdentity(int error, scoped_refptr<gm::ClientIdentity> identity);
  int RequestIdentity();
  void SetIdentity(scoped_refptr<gm::ClientIdentity> identity);
  int InitializeTls(base::span<const uint16_t> cipher_suites);
  void SchedulePump();
  void PumpAndNotify();
  void Pump();
  void OnTransportRead(int result);
  void OnTransportWrite(int result);
  void OnSignature(uint64_t request_id,
                   int error,
                   std::vector<uint8_t> signature);
  void OnIdentityInvalidated();
  void Fail(int error);
  int MapStatus(GmStatus status);
  int UpdateCertificateInfo(bool authenticated);
  void MaybeCacheSession();
  void ProcessInput(int result);
  bool FlushOutput();

  raw_ptr<SSLClientContext> ssl_context_;
  raw_ptr<gm::ClientContext> gm_context_;
  raw_ptr<gm::ClientIdentityProvider> identity_provider_;
  std::unique_ptr<StreamSocket> transport_;
  const HostPortPair server_;
  const SSLConfig config_;
  scoped_refptr<gm::BackendContext> backend_context_;
  raw_ptr<GmConnection> connection_ = nullptr;
  scoped_refptr<gm::ClientIdentity> identity_;
  base::ScopedClosureRunner identity_cancel_;
  base::CallbackListSubscription invalidation_subscription_;
  std::optional<gm::ClientContext::SessionKey> session_key_;
  gm::ClientContext::Session session_;
  scoped_refptr<IOBufferWithSize> transport_input_;
  scoped_refptr<DrainableIOBuffer> transport_output_;
  bool transport_read_pending_ = false;
  bool transport_write_pending_ = false;
  bool transport_eof_ = false;
  bool pump_scheduled_ = false;
  bool connecting_ = false;
  bool connected_ = false;
  bool used_ = false;
  int error_ = OK;
  uint64_t signing_request_ = 0;
  uint64_t identity_request_ = 0;
  bool protocol_retry_allowed_ = false;
  uint64_t cached_session_generation_ = 0;
  int64_t received_bytes_ = 0;
  std::vector<uint8_t> prefetched_;
  scoped_refptr<IOBuffer> read_buffer_;
  int read_length_ = 0;
  std::optional<int> read_result_;
  scoped_refptr<IOBuffer> write_buffer_;
  int write_length_ = 0;
  std::optional<int> write_result_;
  CompletionOnceCallback connect_callback_;
  CompletionOnceCallback read_callback_;
  CompletionOnceCallback write_callback_;
  NextProto negotiated_protocol_ = NextProto::kProtoUnknown;
  SSLInfo ssl_info_;
  base::WeakPtrFactory<GmSSLClientSocket> weak_factory_{this};
};

}  // namespace net

#endif  // NET_SOCKET_GM_SSL_CLIENT_SOCKET_H_
