// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_SOCKET_LEAF_CONNECT_JOB_H_
#define NET_SOCKET_LEAF_CONNECT_JOB_H_

#include <memory>
#include <set>
#include <string>

#include "base/memory/raw_ptr.h"
#include "base/memory/ref_counted.h"
#include "base/time/time.h"
#include "net/base/completion_once_callback.h"
#include "net/base/host_port_pair.h"
#include "net/base/net_errors.h"
#include "net/base/net_export.h"
#include "net/base/network_isolation_key.h"
#include "net/base/request_priority.h"
#include "net/dns/public/resolve_error_info.h"
#include "net/socket/connect_job.h"
#include "net/socket/connect_job_params.h"
#include "net/socket/leaf_outbound_protocol.h"
#include "net/socket/leaf_client_socket.h"
#include "net/traffic_annotation/network_traffic_annotation.h"

namespace net {

class SocketTag;
class StreamSocket;

class NET_EXPORT_PRIVATE LeafSocketParams
    : public base::RefCounted<LeafSocketParams> {
 public:
  LeafSocketParams(ConnectJobParams nested_params,
                   const HostPortPair& host_port_pair,
                   const NetworkAnonymizationKey& network_anonymization_key,
                   const NetworkTrafficAnnotationTag& traffic_annotation,
                   LeafOutboundProtocol protocol,
                   std::string leaf_credential,
                   std::string leaf_uri_query,
                   std::string leaf_uri_fragment,
                   std::string leaf_proxy_authority_host);

  LeafSocketParams(const LeafSocketParams&) = delete;
  LeafSocketParams& operator=(const LeafSocketParams&) = delete;

  // Transport to the proxy, optionally wrapped in TLS (`SSLSocketParams`).
  const ConnectJobParams& nested_proxy_connect_params() const {
    return nested_proxy_connect_params_;
  }
  const HostPortPair& destination() const { return destination_; }
  const NetworkAnonymizationKey& network_anonymization_key() {
    return network_anonymization_key_;
  }

  const NetworkTrafficAnnotationTag traffic_annotation() {
    return traffic_annotation_;
  }

  LeafOutboundProtocol protocol() const { return protocol_; }

  const std::string& leaf_credential() const { return leaf_credential_; }

  const std::string& leaf_uri_query() const { return leaf_uri_query_; }
  const std::string& leaf_uri_fragment() const { return leaf_uri_fragment_; }

  const std::string& leaf_proxy_authority_host() const {
    return leaf_proxy_authority_host_;
  }

 private:
  friend class base::RefCounted<LeafSocketParams>;
  ~LeafSocketParams();

  const ConnectJobParams nested_proxy_connect_params_;
  const HostPortPair destination_;
  const NetworkAnonymizationKey network_anonymization_key_;
  NetworkTrafficAnnotationTag traffic_annotation_;
  const LeafOutboundProtocol protocol_;
  const std::string leaf_credential_;
  const std::string leaf_uri_query_;
  const std::string leaf_uri_fragment_;
  const std::string leaf_proxy_authority_host_;
};

class NET_EXPORT_PRIVATE LeafConnectJob : public ConnectJob,
                                          public ConnectJob::Delegate {
 public:
  class NET_EXPORT_PRIVATE Factory {
   public:
    Factory() = default;
    virtual ~Factory() = default;

    virtual std::unique_ptr<LeafConnectJob> Create(
        RequestPriority priority,
        const SocketTag& socket_tag,
        const CommonConnectJobParams* common_connect_job_params,
        scoped_refptr<LeafSocketParams> leaf_params,
        ConnectJob::Delegate* delegate,
        const NetLogWithSource* net_log);
  };

  LeafConnectJob(RequestPriority priority,
                 const SocketTag& socket_tag,
                 const CommonConnectJobParams* common_connect_job_params,
                 scoped_refptr<LeafSocketParams> leaf_params,
                 ConnectJob::Delegate* delegate,
                 const NetLogWithSource* net_log);

  LeafConnectJob(const LeafConnectJob&) = delete;
  LeafConnectJob& operator=(const LeafConnectJob&) = delete;

  ~LeafConnectJob() override;

  LoadState GetLoadState() const override;
  bool HasEstablishedConnection() const override;
  ResolveErrorInfo GetResolveErrorInfo() const override;

  static base::TimeDelta HandshakeTimeoutForTesting();

 private:
  enum State {
    STATE_TRANSPORT_CONNECT,
    STATE_TRANSPORT_CONNECT_COMPLETE,
    STATE_LEAF_HANDSHAKE,
    STATE_LEAF_HANDSHAKE_COMPLETE,
    STATE_NONE,
  };

  void OnIOComplete(int result);

  void OnConnectJobComplete(int result, ConnectJob* job) override;
  void OnNeedsProxyAuth(const HttpResponseInfo& response,
                         HttpAuthController* auth_controller,
                         base::OnceClosure restart_with_auth_callback,
                         ConnectJob* job) override;
  int DoLoop(int result);

  int DoTransportConnect();
  int DoTransportConnectComplete(int result);
  int DoLeafHandshake();
  int DoLeafHandshakeComplete(int result);

  int ConnectInternal() override;

  void ChangePriorityInternal(RequestPriority priority) override;

  scoped_refptr<LeafSocketParams> leaf_params_;

  State next_state_;
  std::unique_ptr<ConnectJob> transport_connect_job_;
  std::unique_ptr<StreamSocket> socket_;

  ResolveErrorInfo resolve_error_info_;
};

}  // namespace net

#endif  // NET_SOCKET_LEAF_CONNECT_JOB_H_
