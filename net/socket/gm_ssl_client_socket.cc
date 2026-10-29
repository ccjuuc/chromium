// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/socket/gm_ssl_client_socket.h"

#include <algorithm>
#include <array>
#include <utility>

#include "base/check.h"
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/strings/string_util.h"
#include "base/task/single_thread_task_runner.h"
#include "crypto/sha2.h"
#include "net/base/net_error_details.h"
#include "net/base/net_errors.h"
#include "net/cert/asn1_util.h"
#include "net/cert/x509_certificate.h"
#include "net/gm/gm_backend_loader.h"
#include "net/gm/gm_tls_policy.h"
#include "net/http/transport_security_state.h"
#include "net/ssl/ssl_connection_status_flags.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "third_party/boringssl/src/include/openssl/mem.h"
#include "third_party/boringssl/src/include/openssl/ssl.h"

namespace net {
namespace {

constexpr size_t kRecordBufferSize = 16 * 1024;
constexpr auto kTlsTraffic =
    DefineNetworkTrafficAnnotation("gm_tls_transport", R"(
  semantics {
    sender: "National TLS transport"
    description: "Encrypts an existing browser HTTPS request using a national TLS protocol selected by negotiation or a managed site rule."
    trigger: "An HTTPS connection selects national TLS after a supported protocol incompatibility or matches a managed national TLS site rule."
    data: "TLS handshake, public certificates and encrypted bytes of the original request."
    destination: WEBSITE
  }
  policy {
    cookies_allowed: YES
    cookies_store: "user"
    setting: "Automatic national TLS uses local system trust; managed site rules can set a specific protocol and trust configuration."
    policy_exception_justification: "The originating request's policy applies; this layer only encrypts its transport."
  })");

GmBytes Bytes(base::span<const uint8_t> bytes) {
  return {bytes.data(), bytes.size()};
}

CertStatus CertificateStatus(uint32_t errors) {
  CertStatus status = 0;
  if (errors & GM_CERT_ERROR_AUTHORITY) {
    status |= CERT_STATUS_AUTHORITY_INVALID;
  }
  if (errors & GM_CERT_ERROR_DATE) {
    status |= CERT_STATUS_DATE_INVALID;
  }
  if (errors & GM_CERT_ERROR_NAME) {
    status |= CERT_STATUS_COMMON_NAME_INVALID;
  }
  if (errors & GM_CERT_ERROR_REVOKED) {
    status |= CERT_STATUS_REVOKED;
  }
  if (errors & GM_CERT_ERROR_REVOCATION_UNAVAILABLE) {
    status |= CERT_STATUS_UNABLE_TO_CHECK_REVOCATION;
  }
  if (errors & GM_CERT_ERROR_INVALID) {
    status |= CERT_STATUS_INVALID;
  }
  if (errors & GM_CERT_ERROR_WEAK_SIGNATURE) {
    status |= CERT_STATUS_WEAK_SIGNATURE_ALGORITHM;
  }
  return status;
}

}  // namespace

GmSSLClientSocket::GmSSLClientSocket(
    SSLClientContext* ssl_context,
    gm::ClientContext* gm_context,
    gm::ClientIdentityProvider* identity_provider,
    std::unique_ptr<StreamSocket> transport,
    const HostPortPair& server,
    const SSLConfig& config)
    : ssl_context_(ssl_context),
      gm_context_(gm_context),
      identity_provider_(identity_provider),
      transport_(std::move(transport)),
      server_(server),
      config_(config),
      transport_input_(
          base::MakeRefCounted<IOBufferWithSize>(kRecordBufferSize)) {}

GmSSLClientSocket::~GmSSLClientSocket() {
  Disconnect();
}

int GmSSLClientSocket::Connect(CompletionOnceCallback callback) {
  if (IsConnected()) {
    return OK;
  }
  if (connecting_) {
    return ERR_UNEXPECTED;
  }
  if (!config_.gm_rule || !config_.gm_rule->profile.IsValid() ||
      !transport_->IsConnected()) {
    return ERR_SSL_PROTOCOL_ERROR;
  }
  if (config_.privacy_mode == PRIVACY_MODE_ENABLED_WITHOUT_CLIENT_CERTS &&
      !config_.gm_rule->profile.client_identity_policy_id().empty()) {
    // A required identity cannot override the request's credential policy.
    // Reject before consulting the provider, including its authorization cache.
    return ERR_SSL_CLIENT_AUTH_PRIVATE_KEY_ACCESS_DENIED;
  }
  const auto protocol = config_.gm_rule->profile.protocol();
  const auto& context_config = ssl_context_->config();
  if (protocol == gm::TlsProtocol::kRfc8998) {
    const uint16_t version_min =
        config_.version_min_override.value_or(context_config.version_min);
    const uint16_t version_max =
        config_.version_max_override.value_or(context_config.version_max);
    if (version_min < TLS1_2_VERSION || version_min > TLS1_3_VERSION ||
        version_max < TLS1_3_VERSION) {
      return ERR_SSL_VERSION_OR_CIPHER_MISMATCH;
    }
  } else if (protocol != gm::TlsProtocol::kTlcp || config_.require_ecdhe) {
    // TLCP is an explicitly selected wire protocol, separate from the ordinary
    // TLS version interval. Its currently implemented static ECC suites cannot
    // satisfy a caller's forward-secret key-exchange requirement.
    return ERR_SSL_VERSION_OR_CIPHER_MISMATCH;
  }
  const std::array<uint16_t, 2> candidates =
      protocol == gm::TlsProtocol::kTlcp
          ? std::array<uint16_t, 2>{SSL_CIPHER_TLCP_ECC_SM4_GCM_SM3,
                                    SSL_CIPHER_TLCP_ECC_SM4_CBC_SM3}
          : std::array<uint16_t, 2>{0x00c6, 0x00c7};
  std::vector<uint16_t> cipher_suites;
  for (uint16_t suite : candidates) {
    if (!std::ranges::contains(context_config.disabled_cipher_suites, suite)) {
      cipher_suites.push_back(suite);
    }
  }
  if (cipher_suites.empty()) {
    return ERR_SSL_VERSION_OR_CIPHER_MISMATCH;
  }
  error_ = OK;
  connecting_ = true;
  connect_callback_ = std::move(callback);
  protocol_retry_allowed_ = false;
  // This lookup may only return an existing live authorization. A fresh
  // connection sends ClientHello before enumerating devices or prompting.
  if (config_.privacy_mode != PRIVACY_MODE_ENABLED_WITHOUT_CLIENT_CERTS &&
      identity_provider_ && config_.gm_rule->enable_session_resumption) {
    auto cached = identity_provider_->GetCachedIdentity(
        server_, config_.gm_rule->profile);
    if (cached && cached->is_valid()) {
      SetIdentity(std::move(cached));
    }
  }
  const int result = InitializeTls(cipher_suites);
  if (result != OK) {
    connect_callback_.Reset();
    connecting_ = false;
    return result;
  }
  SchedulePump();
  return ERR_IO_PENDING;
}

void GmSSLClientSocket::SetIdentity(
    scoped_refptr<gm::ClientIdentity> identity) {
  invalidation_subscription_ = {};
  identity_ = std::move(identity);
  if (identity_) {
    invalidation_subscription_ = identity_->AddInvalidationCallback(
        base::BindRepeating(&GmSSLClientSocket::OnIdentityInvalidated,
                            weak_factory_.GetWeakPtr()));
  }
}

void GmSSLClientSocket::OnIdentity(int error,
                                   scoped_refptr<gm::ClientIdentity> identity) {
  identity_cancel_.ReplaceClosure({});
  if (!connecting_ || !connection_ || !identity_request_) {
    return;
  }
  if (error == OK &&
      ((identity && !identity->is_valid()) ||
       (!identity &&
        !config_.gm_rule->profile.client_identity_policy_id().empty()))) {
    error = ERR_SSL_CLIENT_AUTH_PRIVATE_KEY_ACCESS_DENIED;
  }
  const uint64_t request = std::exchange(identity_request_, 0);
  if (error != OK) {
    gm::CallBackendFunction(backend_context_->api().complete_identity_request,
        connection_, request, GM_ERROR_CANCELLED, nullptr, {});
    Fail(error);
    SchedulePump();
    return;
  }
  SetIdentity(std::move(identity));
  GmIdentity selected = {};
  std::vector<GmBytes> intermediates;
  if (identity_) {
    selected.struct_size = sizeof(selected);
    selected.signing_certificate = Bytes(identity_->signing_certificate());
    selected.encryption_certificate =
        Bytes(identity_->encryption_certificate());
    selected.signing_key_id = identity_->signing_key_id();
    selected.encryption_key_id = identity_->encryption_key_id();
    for (const auto& der : identity_->intermediates()) {
      intermediates.push_back(Bytes(der));
    }
    selected.intermediates = intermediates.data();
    selected.intermediate_count = intermediates.size();
  }
  if (session_key_) {
    // Authentication changes the cache partition. Never export the resulting
    // mTLS session under the provisional anonymous identity.
    session_key_->identity_id = identity_ ? identity_->identity_id() : "";
    session_key_->identity_generation = identity_ ? identity_->generation() : 0;
    session_ = gm_context_->GetSession(*session_key_);
    if (!session_.blob.empty()) {
      OPENSSL_cleanse(session_.blob.data(), session_.blob.size());
    }
    session_.blob.clear();
  }
  const auto status = gm::CallBackendFunction(backend_context_->api().complete_identity_request,
      connection_, request, GM_OK, identity_ ? &selected : nullptr,
      Bytes(session_.binding));
  if (status != GM_OK) {
    Fail(status == GM_ERROR_CLIENT_AUTH ? ERR_BAD_SSL_CLIENT_AUTH_CERT
                                        : MapStatus(status));
  }
  SchedulePump();
}

int GmSSLClientSocket::RequestIdentity() {
  if (identity_request_) {
    return OK;
  }
  const auto& api = backend_context_->api();
  GmIdentityRequest pending = {};
  pending.struct_size = sizeof(pending);
  if (gm::CallBackendFunction(api.get_identity_request, connection_, &pending) != GM_OK ||
      !pending.request_id || pending.ca_name_count > 128 ||
      pending.signature_algorithm_count > 128 ||
      pending.certificate_type_count > 256) {
    return ERR_SSL_PROTOCOL_ERROR;
  }
  // The backend has verified both role chains and the server handshake
  // signature. Finish Chromium's host/PSL and pin checks before authorization.
  const int verified = UpdateCertificateInfo(true);
  if (verified != OK) {
    return verified;
  }
  gm::ClientCertificateRequest request;
  request.request_id = pending.request_id;
  request.gm_client_auth_context_resolver = config_.gm_client_auth_context;
  request.protocol = pending.protocol == GM_PROTOCOL_TLCP
                         ? gm::TlsProtocol::kTlcp
                         : gm::TlsProtocol::kRfc8998;
  request.required_roles = pending.required_roles;
  const auto algorithms = base::span(pending.signature_algorithms)
                              .first(pending.signature_algorithm_count);
  request.signature_algorithms.assign(algorithms.begin(), algorithms.end());
  const auto types = base::span(pending.certificate_types)
                         .first(pending.certificate_type_count);
  request.certificate_types.assign(types.begin(), types.end());
  size_t total = 0;
  for (size_t i = 0; i < pending.ca_name_count; ++i) {
    size_t size = 0;
    if (gm::CallBackendFunction(api.get_identity_request_ca, connection_, i, nullptr, 0, &size) !=
            GM_OK ||
        !size || size > 16384 || size > 65535 - total) {
      return ERR_SSL_PROTOCOL_ERROR;
    }
    total += size;
    auto& name = request.certificate_authorities.emplace_back(size);
    if (gm::CallBackendFunction(api.get_identity_request_ca, connection_, i, name.data(), size, &size) !=
        GM_OK) {
      return ERR_SSL_PROTOCOL_ERROR;
    }
  }
  identity_request_ = pending.request_id;
  if (config_.privacy_mode == PRIVACY_MODE_ENABLED_WITHOUT_CLIENT_CERTS ||
      !identity_provider_) {
    if (!config_.gm_rule->profile.client_identity_policy_id().empty()) {
      return ERR_SSL_CLIENT_AUTH_PRIVATE_KEY_ACCESS_DENIED;
    }
    OnIdentity(OK, nullptr);
    return OK;
  }
  identity_cancel_.ReplaceClosure(identity_provider_->AcquireIdentity(
      server_, config_.gm_rule->profile, request,
      base::BindOnce(&GmSSLClientSocket::OnIdentity,
                     weak_factory_.GetWeakPtr())));
  return OK;
}

int GmSSLClientSocket::InitializeTls(base::span<const uint16_t> cipher_suites) {
  backend_context_ = gm_context_->GetContext(config_.gm_rule);
  if (!backend_context_) {
    return ERR_SSL_VERSION_OR_CIPHER_MISMATCH;
  }
  GmIdentity identity = {};
  std::vector<GmBytes> intermediates;
  if (identity_) {
    identity.struct_size = sizeof(identity);
    identity.signing_certificate = Bytes(identity_->signing_certificate());
    identity.encryption_certificate =
        Bytes(identity_->encryption_certificate());
    identity.signing_key_id = identity_->signing_key_id();
    identity.encryption_key_id = identity_->encryption_key_id();
    for (const auto& der : identity_->intermediates()) {
      intermediates.push_back(Bytes(der));
    }
    identity.intermediates = intermediates.data();
    identity.intermediate_count = intermediates.size();
  }
  NextProtoVector protocols;
  for (auto protocol : config_.alpn_protos) {
    if (protocol == NextProto::kProtoHTTP11 ||
        (protocol == NextProto::kProtoHTTP2 &&
         config_.gm_rule->profile.protocol() == gm::TlsProtocol::kRfc8998)) {
      protocols.push_back(protocol);
    }
  }
  if (protocols.empty()) {
    protocols.push_back(NextProto::kProtoHTTP11);
  }
  const auto alpn = SerializeNextProtos(protocols);
  GmConnectionConfig config = {};
  config.struct_size = sizeof(config);
  config.server_name = Bytes(base::as_byte_span(server_.host()));
  config.identity = identity_ ? &identity : nullptr;
  config.hostname_mode = GM_HOSTNAME_BROWSER_SAN;
  config.alpn = Bytes(alpn);
  config.defer_identity = 1;
  config.require_client_auth =
      !config_.gm_rule->profile.client_identity_policy_id().empty();
  if (config_.gm_rule->enable_session_resumption) {
    session_key_ = gm::ClientContext::MakeSessionKey(server_, config_,
                                                     identity_.get(), alpn);
    session_ = gm_context_->GetSession(*session_key_);
    config.session_binding = Bytes(session_.binding);
    config.session_blob = Bytes(session_.blob);
  }
  GmConnection* connection = nullptr;
  auto result = gm::CreateBackendConnection(backend_context_->handle(), &config,
                                            cipher_suites, &connection);
  const bool imported_session = !session_.blob.empty();
  if (!session_.blob.empty()) {
    OPENSSL_cleanse(session_.blob.data(), session_.blob.size());
    session_.blob.clear();
  }
  if (result != GM_OK && imported_session) {
    // Expired/invalid tickets must not poison subsequent connections. Remove
    // the ticket and attempt one full handshake using exactly the same GM
    // protocol, trust and identity. Full certificate verification still runs.
    CHECK(!connection);
    gm_context_->EraseSession(*session_key_);
    session_ = gm_context_->GetSession(*session_key_);
    config.session_binding = Bytes(session_.binding);
    config.session_blob = {};
    result = gm::CreateBackendConnection(backend_context_->handle(), &config,
                                         cipher_suites, &connection);
  }
  if (result != GM_OK) {
    return ERR_SSL_PROTOCOL_ERROR;
  }
  connection_ = connection;
  return OK;
}

void GmSSLClientSocket::SchedulePump() {
  if (pump_scheduled_) {
    return;
  }
  pump_scheduled_ = true;
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&GmSSLClientSocket::PumpAndNotify,
                                weak_factory_.GetWeakPtr()));
}

void GmSSLClientSocket::PumpAndNotify() {
  pump_scheduled_ = false;
  Pump();
  auto alive = weak_factory_.GetWeakPtr();
  if (connect_callback_ && (error_ != OK || (connected_ && !transport_output_ &&
                                             !transport_write_pending_))) {
    connecting_ = false;
    std::move(connect_callback_).Run(error_);
    if (!alive) {
      return;
    }
  }
  if (read_callback_ && (read_result_ || error_ != OK)) {
    int result = read_result_.value_or(error_);
    read_buffer_ = nullptr;
    read_result_.reset();
    std::move(read_callback_).Run(result);
    if (!alive) {
      return;
    }
  }
  if (write_callback_ && (write_result_ || error_ != OK)) {
    int result = error_ != OK ? error_ : *write_result_;
    write_buffer_ = nullptr;
    write_result_.reset();
    std::move(write_callback_).Run(result);
  }
}

bool GmSSLClientSocket::FlushOutput() {
  const auto& api = backend_context_->api();
  for (int iteration = 0; iteration < 64 && !transport_write_pending_;
       ++iteration) {
    if (!transport_output_) {
      auto buffer = base::MakeRefCounted<IOBufferWithSize>(kRecordBufferSize);
      size_t length = 0;
      const auto status = gm::CallBackendFunction(api.drain_ciphertext,
          connection_, buffer->span().data(), buffer->size(), &length);
      if (status != GM_OK) {
        Fail(MapStatus(status));
        return false;
      }
      if (!length) {
        return true;
      }
      transport_output_ = base::MakeRefCounted<DrainableIOBuffer>(
          buffer, static_cast<int>(length));
    }
    int result = transport_->Write(
        transport_output_.get(), transport_output_->BytesRemaining(),
        base::BindOnce(&GmSSLClientSocket::OnTransportWrite,
                       weak_factory_.GetWeakPtr()),
        kTlsTraffic);
    if (result == ERR_IO_PENDING) {
      transport_write_pending_ = true;
      return true;
    }
    if (result <= 0) {
      Fail(result < 0 ? result : ERR_CONNECTION_CLOSED);
      return false;
    }
    transport_output_->DidConsume(result);
    if (!transport_output_->BytesRemaining()) {
      transport_output_ = nullptr;
    }
  }
  if (!transport_write_pending_) {
    SchedulePump();
  }
  return true;
}

void GmSSLClientSocket::Pump() {
  if (error_ != OK || !connection_) {
    return;
  }
  if (identity_ && !identity_->is_valid()) {
    OnIdentityInvalidated();
    return;
  }
  const auto& api = backend_context_->api();
  for (int iteration = 0; iteration < 64 && error_ == OK; ++iteration) {
    if (!FlushOutput()) {
      return;
    }
    bool want_input = false;
    if (!connected_) {
      const auto status = gm::CallBackendFunction(api.handshake, connection_);
      if (status == GM_WANT_IDENTITY) {
        const int result = RequestIdentity();
        if (result != OK) {
          Fail(result);
        }
        FlushOutput();
        return;
      }
      if (status == GM_WANT_KEY) {
        if (!signing_request_) {
          GmKeyRequest request = {};
          request.struct_size = sizeof(request);
          if (!identity_ ||
              gm::CallBackendFunction(api.get_key_request, connection_, &request) != GM_OK ||
              request.key_id != identity_->signing_key_id()) {
            Fail(ERR_SSL_CLIENT_AUTH_PRIVATE_KEY_ACCESS_DENIED);
            return;
          }
          signing_request_ = request.request_id;
          std::array<uint8_t, 32> digest;
          base::span(digest).copy_from(base::span(request.digest));
          identity_->SignDigest(
              request.request_id, digest,
              base::BindOnce(&GmSSLClientSocket::OnSignature,
                             weak_factory_.GetWeakPtr(), request.request_id));
        }
        FlushOutput();
        return;
      }
      if (status == GM_OK) {
        int result = UpdateCertificateInfo(true);
        if (result != OK) {
          Fail(result);
          return;
        }
        connected_ = true;
        MaybeCacheSession();
      } else if (status == GM_WANT_INPUT) {
        want_input = true;
      } else {
        UpdateCertificateInfo(false);
        GmConnectionInfo failure = {};
        failure.struct_size = sizeof(failure);
        protocol_retry_allowed_ =
            status == GM_ERROR_TLS &&
            gm::CallBackendFunction(api.get_connection_info, connection_, &failure) == GM_OK &&
            (failure.flags & GM_INFO_PROTOCOL_RETRY_ALLOWED) != 0;
        Fail(MapStatus(status));
        return;
      }
    }
    if (connected_) {
      if (write_buffer_ && !write_result_) {
        size_t written = 0;
        size_t length = std::min<size_t>(write_length_, kRecordBufferSize);
        const auto status = gm::CallBackendFunction(api.write_plaintext,
            connection_, write_buffer_->span().data(), length, &written);
        if (status == GM_OK) {
          write_result_ = static_cast<int>(written);
        } else if (status == GM_WANT_INPUT) {
          want_input = true;
        } else if (status != GM_ERROR_LIMIT) {
          Fail(MapStatus(status));
          return;
        }
      }
      if (read_buffer_ && !read_result_ && !prefetched_.empty()) {
        const size_t count = std::min<size_t>(read_length_, prefetched_.size());
        read_buffer_->span().first(count).copy_from(
            base::span(prefetched_).first(count));
        prefetched_.erase(prefetched_.begin(), prefetched_.begin() + count);
        read_result_ = static_cast<int>(count);
        received_bytes_ += count;
      }
      if (prefetched_.empty()) {
        // Read post-handshake tickets even without an application Read. At most
        // one record of unsolicited plaintext is retained for a later caller.
        std::array<uint8_t, kRecordBufferSize> buffer;
        size_t written = 0;
        const auto status = gm::CallBackendFunction(api.read_plaintext, connection_, buffer.data(),
                                               buffer.size(), &written);
        // SSL_read can process tickets before returning application bytes.
        // Cache them before the continue below; the caller may release this
        // socket as soon as its pending Read completes.
        if (status == GM_OK || status == GM_WANT_INPUT || status == GM_CLOSED) {
          MaybeCacheSession();
        }
        if (status == GM_OK && written) {
          prefetched_.assign(buffer.begin(), buffer.begin() + written);
          if (read_buffer_ && !read_result_) {
            continue;
          }
        } else if (status == GM_WANT_INPUT) {
          want_input = true;
        } else if (status == GM_CLOSED) {
          if (read_buffer_ && !read_result_) {
            read_result_ = 0;
          }
          transport_eof_ = true;
        } else if (status != GM_OK) {
          Fail(MapStatus(status));
          return;
        }
      }
    }
    if (!FlushOutput()) {
      return;
    }
    if (!want_input || transport_read_pending_ || transport_eof_ ||
        (connected_ && !prefetched_.empty())) {
      return;
    }
    int result =
        transport_->Read(transport_input_.get(), transport_input_->size(),
                         base::BindOnce(&GmSSLClientSocket::OnTransportRead,
                                        weak_factory_.GetWeakPtr()));
    if (result == ERR_IO_PENDING) {
      transport_read_pending_ = true;
      return;
    }
    ProcessInput(result);
  }
  if (error_ == OK) {
    SchedulePump();
  }
}

void GmSSLClientSocket::ProcessInput(int result) {
  if (result < 0) {
    Fail(result);
    return;
  }
  const auto& api = backend_context_->api();
  if (!result) {
    transport_eof_ = true;
    if (gm::CallBackendFunction(api.notify_transport_eof, connection_) != GM_OK) {
      Fail(ERR_SSL_PROTOCOL_ERROR);
    }
    return;
  }
  size_t consumed = 0;
  auto status = gm::CallBackendFunction(api.feed_ciphertext,
      connection_, transport_input_->span().data(), result, &consumed);
  if (status != GM_OK || consumed != static_cast<size_t>(result)) {
    Fail(ERR_SSL_PROTOCOL_ERROR);
  }
}

void GmSSLClientSocket::OnTransportRead(int result) {
  if (error_ != OK || !transport_read_pending_) {
    return;
  }
  transport_read_pending_ = false;
  ProcessInput(result);
  SchedulePump();
}

void GmSSLClientSocket::OnTransportWrite(int result) {
  if (error_ != OK || !transport_write_pending_ || !transport_output_) {
    return;
  }
  transport_write_pending_ = false;
  if (result <= 0) {
    Fail(result < 0 ? result : ERR_CONNECTION_CLOSED);
  } else {
    transport_output_->DidConsume(result);
    if (!transport_output_->BytesRemaining()) {
      transport_output_ = nullptr;
    }
  }
  SchedulePump();
}

void GmSSLClientSocket::OnSignature(uint64_t request_id,
                                    int error,
                                    std::vector<uint8_t> signature) {
  if (request_id != signing_request_ || !connection_) {
    return;
  }
  signing_request_ = 0;
  if (!identity_ || !identity_->is_valid()) {
    OnIdentityInvalidated();
    return;
  }
  auto status = gm::CallBackendFunction(backend_context_->api().complete_key_request,
      connection_, request_id, error == OK ? GM_OK : GM_ERROR_CANCELLED,
      signature.data(), signature.size());
  if (status != GM_OK || error != OK) {
    Fail(error != OK ? error : ERR_SSL_CLIENT_AUTH_SIGNATURE_FAILED);
  }
  SchedulePump();
}

void GmSSLClientSocket::OnIdentityInvalidated() {
  if (identity_) {
    gm_context_->InvalidateIdentity(identity_->identity_id());
  }
  Fail(ERR_SSL_CLIENT_AUTH_PRIVATE_KEY_ACCESS_DENIED);
  SchedulePump();
}

void GmSSLClientSocket::Fail(int error) {
  if (error_ != OK) {
    return;
  }
  error_ = error;
  identity_cancel_.RunAndReset();
  identity_request_ = 0;
  if (error != ERR_SSL_PROTOCOL_ERROR) {
    protocol_retry_allowed_ = false;
  }
  connected_ = false;
  if (identity_ && signing_request_) {
    identity_->Cancel(signing_request_);
  }
  signing_request_ = 0;
  if (connection_) {
    gm::CallBackendFunction(backend_context_->api().cancel, connection_);
  }
  transport_->Disconnect();
  transport_read_pending_ = false;
  transport_write_pending_ = false;
  transport_output_ = nullptr;
}

int GmSSLClientSocket::MapStatus(GmStatus status) {
  if (status == GM_OK) {
    return OK;
  }
  if (status == GM_CLOSED) {
    return ERR_CONNECTION_CLOSED;
  }
  if (status == GM_ERROR_CERTIFICATE) {
    return IsCertStatusError(ssl_info_.cert_status)
               ? MapCertStatusToNetError(ssl_info_.cert_status)
               : ERR_CERT_INVALID;
  }
  if (status == GM_ERROR_CANCELLED) {
    return ERR_ABORTED;
  }
  if (status == GM_ERROR_CLIENT_AUTH) {
    return ERR_BAD_SSL_CLIENT_AUTH_CERT;
  }
  return ERR_SSL_PROTOCOL_ERROR;
}

int GmSSLClientSocket::UpdateCertificateInfo(bool authenticated) {
  const auto& api = backend_context_->api();
  GmConnectionInfo info = {};
  info.struct_size = sizeof(info);
  if (gm::CallBackendFunction(api.get_connection_info, connection_, &info) != GM_OK) {
    return ERR_SSL_PROTOCOL_ERROR;
  }
  ssl_info_.cert_status = CertificateStatus(info.certificate_errors);
  ssl_info_.gm_tls = true;
  ssl_info_.is_fatal_cert_error = IsCertStatusError(ssl_info_.cert_status);
  ssl_info_.public_key_hashes.clear();
  const auto invalid_certificate = [this] {
    ssl_info_.cert_status |= CERT_STATUS_INVALID;
    ssl_info_.is_fatal_cert_error = true;
    return ERR_CERT_INVALID;
  };
  ssl_info_.peer_signature_algorithm = info.peer_signature_algorithm;
  ssl_info_.key_exchange_group = info.key_exchange_group;
  ssl_info_.client_cert_sent = (info.flags & GM_INFO_CLIENT_CERTIFICATE) != 0;
  ssl_info_.is_issued_by_known_root = false;  // Explicit enterprise trust only.
  ssl_info_.handshake_type =
      info.session_reused ? SSLInfo::HANDSHAKE_RESUME : SSLInfo::HANDSHAKE_FULL;
  SSLConnectionStatusSetVersion(info.protocol == GM_PROTOCOL_TLCP
                                    ? SSL_CONNECTION_VERSION_TLCP
                                    : SSL_CONNECTION_VERSION_TLS1_3,
                                &ssl_info_.connection_status);
  SSLConnectionStatusSetCipherSuite(info.cipher_suite,
                                    &ssl_info_.connection_status);
  std::string_view alpn(reinterpret_cast<const char*>(info.alpn),
                        info.alpn_size);
  negotiated_protocol_ =
      alpn == "h2" ? NextProto::kProtoHTTP2 : NextProto::kProtoHTTP11;
  if (info.protocol == GM_PROTOCOL_TLCP &&
      negotiated_protocol_ != NextProto::kProtoHTTP11) {
    return ERR_SSL_PROTOCOL_ERROR;
  }
  for (uint32_t role : {GM_CERTIFICATE_SIGNING, GM_CERTIFICATE_ENCRYPTION}) {
    if (role == GM_CERTIFICATE_ENCRYPTION &&
        info.protocol != GM_PROTOCOL_TLCP) {
      break;
    }
    size_t count = 0;
    if (gm::CallBackendFunction(api.get_peer_certificate_count, connection_, role, &count) != GM_OK ||
        !count || count > 32) {
      return authenticated ? invalid_certificate() : ERR_SSL_PROTOCOL_ERROR;
    }
    std::vector<std::vector<uint8_t>> certificates(count);
    std::vector<std::string_view> views;
    for (size_t i = 0; i < count; ++i) {
      size_t size = 0;
      if (gm::CallBackendFunction(api.get_peer_certificate, connection_, role, i, nullptr, 0, &size) !=
              GM_OK ||
          !size || size > 256 * 1024) {
        return invalid_certificate();
      }
      certificates[i].resize(size);
      if (gm::CallBackendFunction(api.get_peer_certificate, connection_, role, i, certificates[i].data(),
                                   size, &size) != GM_OK) {
        return invalid_certificate();
      }
      views.push_back(base::as_string_view(base::span(certificates[i])));
    }
    auto certificate = X509Certificate::CreateFromDERCertChain(views);
    if (!certificate) {
      return invalid_certificate();
    }
    if (!certificate->VerifyNameMatch(server_.host())) {
      ssl_info_.cert_status |= CERT_STATUS_COMMON_NAME_INVALID;
      ssl_info_.is_fatal_cert_error = true;
    }
    if (role == GM_CERTIFICATE_SIGNING) {
      ssl_info_.cert = certificate;
      ssl_info_.unverified_cert = certificate;
      for (auto der : views) {
        std::string_view spki;
        if (!asn1::ExtractSPKIFromDERCert(der, &spki)) {
          return invalid_certificate();
        }
        ssl_info_.public_key_hashes.push_back(
            crypto::SHA256Hash(base::as_byte_span(spki)));
      }
    }
  }
  if (auto* security_state = ssl_context_->transport_security_state()) {
    const auto pin = security_state->CheckPublicKeyPins(
        server_.host(), false, ssl_info_.public_key_hashes);
    if (pin == TransportSecurityState::PKPStatus::VIOLATED) {
      ssl_info_.cert_status |= CERT_STATUS_PINNED_KEY_MISSING;
    }
    ssl_info_.pkp_bypassed = pin == TransportSecurityState::PKPStatus::BYPASSED;
  }
  // Explicit national TLS policy never treats verification failure as a cue to
  // switch protocols or ignore errors using a developer certificate exception.
  ssl_info_.is_fatal_cert_error = IsCertStatusError(ssl_info_.cert_status);
  if (authenticated && (info.flags & GM_INFO_ESTABLISHED) &&
      !ssl_info_.is_fatal_cert_error &&
      !config_.gm_rule->profile.client_identity_policy_id().empty() &&
      !ssl_info_.client_cert_sent) {
    // An identity rule requires actual mutual authentication, including when
    // resuming. Merely having a Key available does not authenticate the client.
    return ERR_BAD_SSL_CLIENT_AUTH_CERT;
  }
  return ssl_info_.is_fatal_cert_error
             ? MapCertStatusToNetError(ssl_info_.cert_status)
             : OK;
}

void GmSSLClientSocket::MaybeCacheSession() {
  if (!session_key_ || !connected_ || (identity_ && !identity_->is_valid())) {
    return;
  }
  size_t size = 0;
  const auto& api = backend_context_->api();
  GmConnectionInfo info = {};
  info.struct_size = sizeof(info);
  if (gm::CallBackendFunction(api.get_connection_info, connection_, &info) != GM_OK ||
      !info.session_generation ||
      info.session_generation == cached_session_generation_) {
    return;
  }
  if (gm::CallBackendFunction(api.export_session, connection_, nullptr, 0, &size) != GM_OK || !size ||
      size > 1024 * 1024) {
    return;
  }
  std::vector<uint8_t> blob(size);
  if (gm::CallBackendFunction(api.export_session, connection_, blob.data(), blob.size(), &size) ==
      GM_OK) {
    blob.resize(size);
    gm_context_->StoreSession(*session_key_, session_.binding, std::move(blob));
    cached_session_generation_ = info.session_generation;
  }
  if (!blob.empty()) {
    OPENSSL_cleanse(blob.data(), blob.size());
  }
}

void GmSSLClientSocket::Disconnect() {
  weak_factory_.InvalidateWeakPtrs();
  identity_cancel_.RunAndReset();
  if (identity_ && signing_request_) {
    identity_->Cancel(signing_request_);
  }
  signing_request_ = 0;
  identity_request_ = 0;
  invalidation_subscription_ = {};
  if (connection_) {
    CHECK_EQ(gm::CallBackendFunction(backend_context_->api().destroy_connection, connection_), GM_OK);
    connection_ = nullptr;
  }
  backend_context_ = nullptr;
  identity_ = nullptr;
  transport_->Disconnect();
  transport_read_pending_ = transport_write_pending_ = false;
  connecting_ = connected_ = pump_scheduled_ = false;
  transport_eof_ = false;
  transport_output_ = nullptr;
  read_buffer_ = write_buffer_ = nullptr;
  read_result_.reset();
  write_result_.reset();
  connect_callback_.Reset();
  read_callback_.Reset();
  write_callback_.Reset();
  prefetched_.clear();
  received_bytes_ = 0;
}

void GmSSLClientSocket::PopulateNetErrorDetails(
    NetErrorDetails* details) const {
  details->gm_protocol_retry_allowed = protocol_retry_allowed_;
}

bool GmSSLClientSocket::IsConnected() const {
  return connected_ && error_ == OK && (!identity_ || identity_->is_valid()) &&
         (!transport_eof_ || !prefetched_.empty());
}
bool GmSSLClientSocket::IsConnectedAndIdle() const {
  if (!IsConnected() || read_buffer_ || write_buffer_ || !prefetched_.empty() ||
      transport_output_ || transport_write_pending_) {
    return false;
  }
  size_t pending = 0;
  const auto& api = backend_context_->api();
  if (gm::CallBackendFunction(api.get_pending_plaintext, connection_, &pending) != GM_OK || pending) {
    return false;
  }
  if (gm::CallBackendFunction(api.get_pending_ciphertext, connection_, &pending) != GM_OK || pending) {
    return false;
  }
  return transport_->IsConnectedAndIdle();
}
int GmSSLClientSocket::Read(IOBuffer* buffer,
                            int length,
                            CompletionOnceCallback callback) {
  if (read_callback_) {
    return ERR_UNEXPECTED;
  }
  if (error_ != OK) {
    return error_;
  }
  if (!connected_ || !buffer || length <= 0) {
    return ERR_SOCKET_NOT_CONNECTED;
  }
  used_ = true;
  read_buffer_ = buffer;
  read_length_ = length;
  read_callback_ = std::move(callback);
  SchedulePump();
  return ERR_IO_PENDING;
}
int GmSSLClientSocket::Write(IOBuffer* buffer,
                             int length,
                             CompletionOnceCallback callback,
                             const NetworkTrafficAnnotationTag&) {
  if (write_callback_) {
    return ERR_UNEXPECTED;
  }
  if (error_ != OK) {
    return error_;
  }
  if (!connected_ || !buffer || length <= 0) {
    return ERR_SOCKET_NOT_CONNECTED;
  }
  used_ = true;
  write_buffer_ = buffer;
  write_length_ = length;
  write_callback_ = std::move(callback);
  SchedulePump();
  return ERR_IO_PENDING;
}
int GmSSLClientSocket::GetPeerAddress(IPEndPoint* address) const {
  return transport_->GetPeerAddress(address);
}
int GmSSLClientSocket::GetLocalAddress(IPEndPoint* address) const {
  return transport_->GetLocalAddress(address);
}
const NetLogWithSource& GmSSLClientSocket::NetLog() const {
  return transport_->NetLog();
}
bool GmSSLClientSocket::WasEverUsed() const {
  return used_;
}
NextProto GmSSLClientSocket::GetNegotiatedProtocol() const {
  return negotiated_protocol_;
}
bool GmSSLClientSocket::GetSSLInfo(SSLInfo* info) {
  *info = ssl_info_;
  return ssl_info_.is_valid();
}
int64_t GmSSLClientSocket::GetTotalReceivedBytes() const {
  return transport_->GetTotalReceivedBytes();
}
void GmSSLClientSocket::ApplySocketTag(const SocketTag& tag) {
  transport_->ApplySocketTag(tag);
}
int GmSSLClientSocket::SetReceiveBufferSize(int32_t size) {
  return transport_->SetReceiveBufferSize(size);
}
int GmSSLClientSocket::SetSendBufferSize(int32_t size) {
  return transport_->SetSendBufferSize(size);
}
int GmSSLClientSocket::ExportKeyingMaterial(
    std::string_view label,
    std::optional<base::span<const uint8_t>> context,
    base::span<uint8_t> out) {
  if (!IsConnected()) {
    return ERR_SOCKET_NOT_CONNECTED;
  }
  if (!base::IsStringASCII(label)) {
    return ERR_INVALID_ARGUMENT;
  }
  const GmStatus status = gm::CallBackendFunction(backend_context_->api().export_keying_material,
      connection_, Bytes(base::as_byte_span(label)),
      context ? Bytes(*context) : GmBytes{}, context.has_value(), out.data(),
      out.size());
  if (status == GM_ERROR_UNSUPPORTED) {
    return ERR_NOT_IMPLEMENTED;
  }
  return status == GM_OK ? OK : ERR_FAILED;
}
std::vector<uint8_t> GmSSLClientSocket::GetECHRetryConfigs() {
  return {};
}
std::vector<std::vector<uint8_t>> GmSSLClientSocket::GetServerTrustAnchorIDs() {
  return {};
}

}  // namespace net
