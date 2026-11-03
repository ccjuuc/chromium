#include "cross_transfer/auto_transport.h"

#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <thread>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

#include "cross_transfer/tls_transport_channel.h"

namespace cross_transfer {
namespace {
using Clock = std::chrono::steady_clock;

bool Cancelled(const std::atomic<bool>* cancellation, TransportError* error) {
  if (!cancellation || !cancellation->load()) {
    return false;
  }
  if (error) {
    *error = TransportError::kCancelled;
  }
  return true;
}

int Remaining(Clock::time_point deadline) {
  return static_cast<int>(std::clamp<int64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(deadline -
                                                            Clock::now())
          .count(),
      0, INT_MAX));
}

bool Pause(const std::atomic<bool>* cancellation,
           TransportError* error,
           Clock::time_point deadline) {
  const auto until =
      std::min(deadline, Clock::now() + std::chrono::milliseconds(250));
  while (Clock::now() < until) {
    if (Cancelled(cancellation, error)) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return !Cancelled(cancellation, error);
}

bool IsId(const std::string& value) {
  return value.size() == 32 && std::ranges::all_of(value, [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

bool UsableEndpoint(const CandidateEndpoint& endpoint) {
  if (endpoint.port == 0 || endpoint.scope_id != 0 ||
      endpoint.ip.find('%') != std::string::npos) {
    return false;
  }
  in_addr ipv4{};
  if (inet_pton(AF_INET, endpoint.ip.c_str(), &ipv4) == 1) {
    const uint32_t ip = ntohl(ipv4.s_addr);
    return (ip >> 24) != 0 && (ip >> 24) != 127 && (ip >> 16) != 0xa9fe &&
           (ip >> 28) < 0xe;
  }
  in6_addr ipv6{};
  return inet_pton(AF_INET6, endpoint.ip.c_str(), &ipv6) == 1 &&
         !IN6_IS_ADDR_UNSPECIFIED(&ipv6) && !IN6_IS_ADDR_LOOPBACK(&ipv6) &&
         !IN6_IS_ADDR_LINKLOCAL(&ipv6) && !IN6_IS_ADDR_MULTICAST(&ipv6) &&
         !IN6_IS_ADDR_V4MAPPED(&ipv6);
}

std::string ReadPem(BIO* bio) {
  const size_t size = BIO_pending(bio);
  if (size == 0 || size > 8192) {
    return {};
  }
  std::string result(size, '\0');
  if (BIO_read(bio, result.data(), static_cast<int>(size)) !=
      static_cast<int>(size)) {
    return {};
  }
  return result;
}

bool Extension(X509* certificate,
               int nid,
               bool critical,
               const std::vector<uint8_t>& der) {
  std::unique_ptr<ASN1_OCTET_STRING, decltype(&ASN1_OCTET_STRING_free)> value(
      ASN1_OCTET_STRING_new(), ASN1_OCTET_STRING_free);
  if (!value ||
      ASN1_OCTET_STRING_set(value.get(), der.data(), der.size()) != 1) {
    return false;
  }
  std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)> extension(
      X509_EXTENSION_create_by_NID(nullptr, nid, critical, value.get()),
      X509_EXTENSION_free);
  return extension && X509_add_ext(certificate, extension.get(), -1) == 1;
}

bool MakeIdentity(const std::string& name, TlsConfig* config) {
  std::unique_ptr<EC_KEY, decltype(&EC_KEY_free)> ec(
      EC_KEY_new_by_curve_name(NID_X9_62_prime256v1), EC_KEY_free);
  std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(EVP_PKEY_new(),
                                                          EVP_PKEY_free);
  std::unique_ptr<X509, decltype(&X509_free)> certificate(X509_new(),
                                                          X509_free);
  if (!ec || !key || !certificate || EC_KEY_generate_key(ec.get()) != 1 ||
      EVP_PKEY_set1_EC_KEY(key.get(), ec.get()) != 1 ||
      X509_set_version(certificate.get(), 2) != 1) {
    return false;
  }
  std::array<uint8_t, 16> serial{};
  if (RAND_bytes(serial.data(), serial.size()) != 1) {
    return false;
  }
  serial.front() &= 0x7f;
  serial.front() |= 1;
  std::unique_ptr<BIGNUM, decltype(&BN_free)> number(
      BN_bin2bn(serial.data(), serial.size(), nullptr), BN_free);
  if (!number ||
      !BN_to_ASN1_INTEGER(number.get(),
                          X509_get_serialNumber(certificate.get())) ||
      !X509_gmtime_adj(X509_get_notBefore(certificate.get()), -300) ||
      !X509_gmtime_adj(X509_get_notAfter(certificate.get()), 3600) ||
      X509_set_pubkey(certificate.get(), key.get()) != 1) {
    return false;
  }
  auto* subject = X509_get_subject_name(certificate.get());
  // The generated name is an ASCII role, 32 hex digits and a fixed suffix,
  // so both DER lengths fit in one byte. Encode one dNSName GeneralName.
  if (name.size() > 100) {
    return false;
  }
  std::vector<uint8_t> san{0x30, static_cast<uint8_t>(name.size() + 2), 0x82,
                           static_cast<uint8_t>(name.size())};
  san.insert(san.end(), name.begin(), name.end());
  if (X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC,
                                 reinterpret_cast<const uint8_t*>(name.c_str()),
                                 -1, -1, 0) != 1 ||
      X509_set_issuer_name(certificate.get(), subject) != 1 ||
      // Each public certificate is its peer's sole temporary trust anchor.
      // BoringSSL's chain builder requires a self-issued anchor to permit
      // certificate signing, even when that exact certificate is the leaf.
      // pathLen:0 prohibits subordinate CA chains; SAN and EKU still bind the
      // authenticated endpoint to this invitation and role.
      !Extension(certificate.get(), NID_basic_constraints, true,
                 {0x30, 0x06, 0x01, 0x01, 0xff, 0x02, 0x01, 0x00}) ||
      !Extension(certificate.get(), NID_key_usage, true,
                 {0x03, 0x02, 0x02, 0x84}) ||
      // ExtendedKeyUsage: id-kp-serverAuth and id-kp-clientAuth.
      !Extension(
          certificate.get(), NID_ext_key_usage, false,
          {0x30, 0x14, 0x06, 0x08, 0x2b, 0x06, 0x01, 0x05, 0x05, 0x07, 0x03,
           0x01, 0x06, 0x08, 0x2b, 0x06, 0x01, 0x05, 0x05, 0x07, 0x03, 0x02}) ||
      !Extension(certificate.get(), NID_subject_alt_name, false, san) ||
      X509_sign(certificate.get(), key.get(), EVP_sha256()) == 0) {
    return false;
  }
  std::unique_ptr<BIO, decltype(&BIO_free)> certificate_bio(
      BIO_new(BIO_s_mem()), BIO_free);
  std::unique_ptr<BIO, decltype(&BIO_free)> key_bio(BIO_new(BIO_s_mem()),
                                                    BIO_free);
  if (!certificate_bio || !key_bio ||
      PEM_write_bio_X509(certificate_bio.get(), certificate.get()) != 1 ||
      PEM_write_bio_PrivateKey(key_bio.get(), key.get(), nullptr, nullptr, 0,
                               nullptr, nullptr) != 1) {
    return false;
  }
  config->certificate_pem = ReadPem(certificate_bio.get());
  config->private_key_pem = ReadPem(key_bio.get());
  return !config->certificate_pem.empty() && !config->private_key_pem.empty();
}

bool ValidState(const PeerConnectionState& state) {
  return state.ok &&
         (state.route == "pending" || state.route == "direct" ||
          state.route == "relay") &&
         state.peer_certificate.size() <= 8192 && state.endpoints.size() <= 8;
}
}  // namespace

std::unique_ptr<ITransportChannel> ConnectAutomaticRelay(
    const AutoConnectionOptions& options,
    const std::atomic<bool>* cancellation,
    TransportError* error) {
  if (Cancelled(cancellation, error)) {
    return nullptr;
  }
  if (!options.select_route || !options.relay) {
    if (error) {
      *error = TransportError::kProtocol;
    }
    return nullptr;
  }
  const auto state = options.select_route("relay");
  if (Cancelled(cancellation, error)) {
    return nullptr;
  }
  if (!ValidState(state) || state.route != "relay") {
    if (error) {
      *error = TransportError::kIo;
    }
    return nullptr;
  }
  auto channel = options.relay(cancellation, error);
  if (Cancelled(cancellation, error)) {
    return nullptr;
  }
  if (!channel || !channel->IsConnected() || !channel->IsSecure() ||
      channel->GetType() != TransportType::kCloudRelay443) {
    if (error && channel) {
      *error = TransportError::kSecurity;
    }
    return nullptr;
  }
  return channel;
}

std::unique_ptr<ITransportChannel> ConnectAutomatically(
    const AutoConnectionOptions& options,
    const std::atomic<bool>* cancellation,
    TransportError* error) {
  TransportError local_error = TransportError::kNone;
  if (!error) error = &local_error;
  *error = TransportError::kProtocol;
  if (Cancelled(cancellation, error)) {
    return nullptr;
  }
  if (!IsId(options.invitation_id) || (!options.exchange && !options.webrtc) ||
      !options.select_route || !options.relay ||
      options.negotiation_timeout_ms <= 0 || options.direct_timeout_ms <= 0) {
    return nullptr;
  }
  if (options.try_reuse) {
    *error = TransportError::kNone;
    auto reused = options.try_reuse(cancellation, error);
    if (Cancelled(cancellation, error) || *error == TransportError::kCancelled) {
      return nullptr;
    }
    if (reused && reused->IsConnected() && reused->IsSecure() &&
        reused->GetType() == TransportType::kWebRtcData) {
      *error = TransportError::kNone;
      return reused;
    }
    reused.reset();
    *error = TransportError::kNone;
  }
  if (options.prefer_relay) {
    return ConnectAutomaticRelay(options, cancellation, error);
  }
  if (options.webrtc) {
    *error = TransportError::kNone;
    auto direct = options.webrtc(cancellation, error);
    if (Cancelled(cancellation, error) || *error == TransportError::kCancelled) {
      return nullptr;
    }
    if (direct && direct->IsConnected() && direct->IsSecure() &&
        direct->GetType() == TransportType::kWebRtcData) {
      if (error) {
        *error = TransportError::kNone;
      }
      return direct;
    }
    direct.reset();
    return ConnectAutomaticRelay(options, cancellation, error);
  }

  LanTcpServer listener;
  listener.SetCancellationFlag(cancellation);
  std::unique_ptr<ITransportChannel> direct;
  const auto fallback = [&]() {
    direct.reset();
    listener.Stop();
    return ConnectAutomaticRelay(options, cancellation, error);
  };
  TlsConfig tls;
  const std::string suffix = "." + options.invitation_id + ".cross-transfer";
  tls.peer_name = (options.sending ? "receiver" : "sender") + suffix;
  if (!MakeIdentity((options.sending ? "sender" : "receiver") + suffix, &tls)) {
    return fallback();
  }
  std::vector<CandidateEndpoint> endpoints;
  if (!options.sending) {
    if (!listener.Start(0)) {
      return fallback();
    }
    endpoints =
        NetworkNicHelper::GetCandidateEndpoints(listener.listening_port());
    std::erase_if(endpoints, [](const auto& endpoint) {
      return !UsableEndpoint(endpoint);
    });
    if (endpoints.size() > 8) {
      endpoints.resize(8);
    }
    if (endpoints.empty()) {
      return fallback();
    }
  }
  auto state = options.exchange(tls.certificate_pem, endpoints);
  const auto negotiation_deadline =
      Clock::now() + std::chrono::milliseconds(options.negotiation_timeout_ms);
  while (!Cancelled(cancellation, error)) {
    if (!ValidState(state)) {
      return fallback();
    }
    if (state.route == "relay") {
      return fallback();
    }
    if (!state.peer_certificate.empty() &&
        (!options.sending || !state.endpoints.empty())) {
      break;
    }
    if (Remaining(negotiation_deadline) == 0) {
      return fallback();
    }
    if (!Pause(cancellation, error, negotiation_deadline)) {
      return nullptr;
    }
    state = options.exchange(tls.certificate_pem, endpoints);
  }
  if (Cancelled(cancellation, error)) {
    return nullptr;
  }
  tls.ca_pem = state.peer_certificate;
  // Receiver starts its short connection budget only after the sender has
  // published, which happens after the sender's potentially lengthy SHA-256.
  const auto deadline =
      Clock::now() + std::chrono::milliseconds(options.direct_timeout_ms);
  if (options.sending) {
    for (const auto& candidate : state.endpoints) {
      if (Cancelled(cancellation, error)) {
        return nullptr;
      }
      if (!UsableEndpoint(candidate)) {
        continue;
      }
      const int budget = std::min(1500, Remaining(deadline));
      if (budget == 0) {
        break;
      }
      direct = TlsTransportChannel::Connect(candidate.ip, candidate.port, tls,
                                            budget, cancellation, error);
      if (direct) {
        break;
      }
    }
  } else {
    while (!Cancelled(cancellation, error) && Remaining(deadline) > 0) {
      auto tcp = listener.Accept(std::min(250, Remaining(deadline)));
      if (!tcp) {
        state = options.exchange(tls.certificate_pem, endpoints);
        if (!ValidState(state) || state.route == "relay") {
          return fallback();
        }
        continue;
      }
      direct = TlsTransportChannel::Accept(
          std::move(tcp), tls, std::max(1, std::min(1500, Remaining(deadline))),
          cancellation, error);
      if (direct) {
        break;
      }
    }
  }
  if (Cancelled(cancellation, error)) {
    return nullptr;
  }
  if (!direct) {
    return fallback();
  }
  listener.Stop();
  state = options.select_route("direct");
  while (!Cancelled(cancellation, error)) {
    if (!ValidState(state) || state.route == "relay") {
      return fallback();
    }
    if (state.route == "direct") {
      if (error) {
        *error = TransportError::kNone;
      }
      return direct;
    }
    if (Remaining(deadline) == 0) {
      return fallback();
    }
    if (!Pause(cancellation, error, deadline)) {
      return nullptr;
    }
    state = options.exchange(tls.certificate_pem, endpoints);
  }
  return nullptr;
}
}  // namespace cross_transfer
