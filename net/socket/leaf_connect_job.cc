// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/socket/leaf_connect_job.h"

#include <memory>
#include <string>
#include <utility>
#include <variant>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/notreached.h"
#include "build/buildflag.h"
#include "net/base/net_errors.h"
#include "net/net_buildflags.h"
#include "net/log/net_log_event_type.h"
#include "net/log/net_log_source_type.h"
#include "net/log/net_log_with_source.h"
#include "net/base/host_port_pair.h"
#include "net/socket/client_socket_factory.h"
#include "net/socket/connect_job_params.h"
#include "net/socket/ssl_connect_job.h"
#include "net/socket/transport_connect_job.h"
#include "url/scheme_host_port.h"

namespace net {

static constexpr base::TimeDelta kLeafConnectJobTimeout = base::Seconds(30);

LeafSocketParams::LeafSocketParams(
    ConnectJobParams nested_params,
    const HostPortPair& host_port_pair,
    const NetworkAnonymizationKey& network_anonymization_key,
    const NetworkTrafficAnnotationTag& traffic_annotation,
    LeafOutboundProtocol protocol,
    std::string leaf_credential,
    std::string leaf_uri_query,
    std::string leaf_uri_fragment,
    std::string leaf_proxy_authority_host)
    : nested_proxy_connect_params_(std::move(nested_params)),
      destination_(host_port_pair),
      network_anonymization_key_(network_anonymization_key),
      traffic_annotation_(traffic_annotation),
      protocol_(protocol),
      leaf_credential_(std::move(leaf_credential)),
      leaf_uri_query_(std::move(leaf_uri_query)),
      leaf_uri_fragment_(std::move(leaf_uri_fragment)),
      leaf_proxy_authority_host_(std::move(leaf_proxy_authority_host)) {
  DCHECK(nested_proxy_connect_params_.is_transport() ||
         nested_proxy_connect_params_.is_ssl());
}

LeafSocketParams::~LeafSocketParams() = default;

std::unique_ptr<LeafConnectJob> LeafConnectJob::Factory::Create(
    RequestPriority priority,
    const SocketTag& socket_tag,
    const CommonConnectJobParams* common_connect_job_params,
    scoped_refptr<LeafSocketParams> leaf_params,
    ConnectJob::Delegate* delegate,
    const NetLogWithSource* net_log) {
  return std::make_unique<LeafConnectJob>(
      priority, socket_tag, common_connect_job_params, std::move(leaf_params),
      delegate, net_log);
}

LeafConnectJob::LeafConnectJob(
    RequestPriority priority,
    const SocketTag& socket_tag,
    const CommonConnectJobParams* common_connect_job_params,
    scoped_refptr<LeafSocketParams> leaf_params,
    ConnectJob::Delegate* delegate,
    const NetLogWithSource* net_log)
    : ConnectJob(priority,
                 socket_tag,
                 base::TimeDelta(),
                 common_connect_job_params,
                 delegate,
                 net_log,
                 NetLogSourceType::LEAF_CONNECT_JOB,
                 NetLogEventType::LEAF_CONNECT_JOB_CONNECT),
      leaf_params_(std::move(leaf_params)) {}

LeafConnectJob::~LeafConnectJob() {
  transport_connect_job_.reset();
}

LoadState LeafConnectJob::GetLoadState() const {
  switch (next_state_) {
    case STATE_TRANSPORT_CONNECT:
      return LOAD_STATE_IDLE;
    case STATE_TRANSPORT_CONNECT_COMPLETE:
      return transport_connect_job_->GetLoadState();
    case STATE_LEAF_HANDSHAKE:
    case STATE_LEAF_HANDSHAKE_COMPLETE:
      return LOAD_STATE_CONNECTING;
    default:
      NOTREACHED();
  }
}

bool LeafConnectJob::HasEstablishedConnection() const {
  return next_state_ == STATE_LEAF_HANDSHAKE ||
         next_state_ == STATE_LEAF_HANDSHAKE_COMPLETE;
}

ResolveErrorInfo LeafConnectJob::GetResolveErrorInfo() const {
  return resolve_error_info_;
}

base::TimeDelta LeafConnectJob::HandshakeTimeoutForTesting() {
  return kLeafConnectJobTimeout;
}

void LeafConnectJob::OnIOComplete(int result) {
  int rv = DoLoop(result);
  if (rv != ERR_IO_PENDING) {
    NotifyDelegateOfCompletion(rv);
  }
}

void LeafConnectJob::OnConnectJobComplete(int result, ConnectJob* job) {
  DCHECK(transport_connect_job_);
  DCHECK_EQ(next_state_, STATE_TRANSPORT_CONNECT_COMPLETE);
  OnIOComplete(result);
}

void LeafConnectJob::OnNeedsProxyAuth(
    const HttpResponseInfo& response,
    HttpAuthController* auth_controller,
    base::OnceClosure restart_with_auth_callback,
    ConnectJob* job) {
  NOTREACHED();
}

int LeafConnectJob::DoLoop(int result) {
  DCHECK_NE(next_state_, STATE_NONE);

  int rv = result;
  do {
    State state = next_state_;
    next_state_ = STATE_NONE;
    switch (state) {
      case STATE_TRANSPORT_CONNECT:
        DCHECK_EQ(OK, rv);
        rv = DoTransportConnect();
        break;
      case STATE_TRANSPORT_CONNECT_COMPLETE:
        rv = DoTransportConnectComplete(rv);
        break;
      case STATE_LEAF_HANDSHAKE:
        DCHECK_EQ(OK, rv);
        rv = DoLeafHandshake();
        break;
      case STATE_LEAF_HANDSHAKE_COMPLETE:
        rv = DoLeafHandshakeComplete(rv);
        break;
      default:
        NOTREACHED() << "bad state";
    }
  } while (rv != ERR_IO_PENDING && next_state_ != STATE_NONE);

  return rv;
}

int LeafConnectJob::DoTransportConnect() {
  DCHECK(!transport_connect_job_);

#if BUILDFLAG(ENABLE_CHROMIUM_LEAF)
  {
    std::string transport_target;
    const auto& nested = leaf_params_->nested_proxy_connect_params();
    if (nested.is_transport()) {
      const auto& ep = nested.transport()->destination();
      if (std::holds_alternative<HostPortPair>(ep)) {
        transport_target = std::get<HostPortPair>(ep).ToString();
      } else {
        transport_target = std::get<url::SchemeHostPort>(ep).Serialize();
      }
    } else {
      DCHECK(nested.is_ssl());
      transport_target =
          nested.ssl()->host_and_port().ToString() + " (TLS to proxy)";
    }
    VLOG(1) << "[LEAF_PROXY_DEBUG] LeafConnectJob transport to "
            << transport_target << " then Leaf handshake for final dest="
            << leaf_params_->destination().ToString();
  }
#endif

  next_state_ = STATE_TRANSPORT_CONNECT_COMPLETE;
  const auto& nested = leaf_params_->nested_proxy_connect_params();
  if (nested.is_transport()) {
    transport_connect_job_ = std::make_unique<TransportConnectJob>(
        priority(), socket_tag(), common_connect_job_params(),
        nested.transport(), this, &net_log());
  } else {
    transport_connect_job_ = std::make_unique<SSLConnectJob>(
        priority(), socket_tag(), common_connect_job_params(), nested.ssl(),
        this, &net_log());
  }
  return transport_connect_job_->Connect();
}

int LeafConnectJob::DoTransportConnectComplete(int result) {
  resolve_error_info_ = transport_connect_job_->GetResolveErrorInfo();
  if (result != OK) {
    return ERR_PROXY_CONNECTION_FAILED;
  }

  ResetTimer(kLeafConnectJobTimeout);
  next_state_ = STATE_LEAF_HANDSHAKE;
  return result;
}

int LeafConnectJob::DoLeafHandshake() {
  next_state_ = STATE_LEAF_HANDSHAKE_COMPLETE;

  socket_ = std::make_unique<LeafClientSocket>(
      transport_connect_job_->PassSocket(), leaf_params_->destination(),
      leaf_params_->protocol(), leaf_params_->leaf_credential(),
      leaf_params_->leaf_uri_query(), leaf_params_->leaf_uri_fragment(),
      leaf_params_->leaf_proxy_authority_host(),
      leaf_params_->traffic_annotation());
  transport_connect_job_.reset();

  return socket_->Connect(
      base::BindOnce(&LeafConnectJob::OnIOComplete, base::Unretained(this)));
}

int LeafConnectJob::DoLeafHandshakeComplete(int result) {
#if BUILDFLAG(ENABLE_CHROMIUM_LEAF)
  VLOG(1) << "[LEAF_PROXY_DEBUG] LeafConnectJob handshake complete net_error="
          << result << " dest=" << leaf_params_->destination().ToString();
#endif
  if (result != OK) {
    socket_->Disconnect();
    return result;
  }

  SetSocket(std::move(socket_), std::nullopt);
  return result;
}

int LeafConnectJob::ConnectInternal() {
  next_state_ = STATE_TRANSPORT_CONNECT;
  return DoLoop(OK);
}

void LeafConnectJob::ChangePriorityInternal(RequestPriority priority) {
  if (transport_connect_job_) {
    transport_connect_job_->ChangePriority(priority);
  }
}

}  // namespace net
