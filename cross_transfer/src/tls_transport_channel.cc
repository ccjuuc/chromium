#include "cross_transfer/tls_transport_channel.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cerrno>
#include <string_view>

#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace cross_transfer {
namespace {

using Clock = std::chrono::steady_clock;

void CloseSocket(NativeSocket socket) {
  if (socket == kInvalidSocket) return;
#if defined(_WIN32)
  closesocket(socket);
#else
  close(socket);
#endif
}

bool IsRetryableSocketError() {
#if defined(_WIN32)
  const int error = WSAGetLastError();
  return error == WSAEWOULDBLOCK || error == WSAEINTR;
#else
  return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}

// A custom BIO keeps Linux sends free of SIGPIPE without changing the process's
// signal policy. macOS sockets have SO_NOSIGPIPE set by LanTcpConnection.
const BIO_METHOD* SocketBioMethod() {
  static const BIO_METHOD* method = [] {
    BIO_METHOD* result = BIO_meth_new(BIO_TYPE_SOURCE_SINK, "CrossTransfer socket");
    if (!result) return static_cast<BIO_METHOD*>(nullptr);
    BIO_meth_set_create(result, [](BIO* bio) {
      BIO_set_init(bio, 1);
      return 1;
    });
    BIO_meth_set_destroy(result, [](BIO*) { return 1; });
    BIO_meth_set_ctrl(result, [](BIO*, int cmd, long, void*) -> long {
      return cmd == BIO_CTRL_FLUSH ? 1 : 0;
    });
    BIO_meth_set_read(result, [](BIO* bio, char* data, int size) {
      BIO_clear_retry_flags(bio);
      auto socket = *static_cast<NativeSocket*>(BIO_get_data(bio));
      int n = static_cast<int>(recv(socket, data, size, 0));
      if (n < 0 && IsRetryableSocketError()) BIO_set_retry_read(bio);
      return n;
    });
    BIO_meth_set_write(result, [](BIO* bio, const char* data, int size) {
      BIO_clear_retry_flags(bio);
      auto socket = *static_cast<NativeSocket*>(BIO_get_data(bio));
#if defined(MSG_NOSIGNAL)
      constexpr int flags = MSG_NOSIGNAL;
#else
      constexpr int flags = 0;
#endif
      int n = static_cast<int>(send(socket, data, size, flags));
      if (n < 0 && IsRetryableSocketError()) BIO_set_retry_write(bio);
      return n;
    });
    return result;
  }();
  return method;
}

bool ConfigurePeerName(SSL* ssl, const std::string& name) {
  if (name.empty() || name.find('\0') != std::string::npos) return false;
  std::array<uint8_t, 16> address;
  if (inet_pton(AF_INET, name.c_str(), address.data()) == 1 ||
      inet_pton(AF_INET6, name.c_str(), address.data()) == 1) {
    return X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl), name.c_str()) == 1;
  }
  return SSL_set1_host(ssl, name.c_str()) == 1;
}

bool LoadTrustAnchors(SSL_CTX* context, const TlsConfig& config) {
  if (config.ca_pem.empty()) {
    return config.ca_file.empty()
               ? SSL_CTX_set_default_verify_paths(context) == 1
               : SSL_CTX_load_verify_locations(
                     context, config.ca_file.c_str(), nullptr) == 1;
  }
  if (!config.ca_file.empty() || config.ca_pem.size() > 256 * 1024) {
    return false;
  }
  // Accept only a PEM certificate bundle, including whitespace between
  // certificates. Do not silently ignore malformed entries or trailing data.
  std::string_view remaining(config.ca_pem);
  constexpr std::string_view kBegin = "-----BEGIN CERTIFICATE-----";
  constexpr std::string_view kEnd = "-----END CERTIFICATE-----";
  bool loaded = false;
  while (true) {
    const auto start = remaining.find_first_not_of(" \t\r\n");
    if (start == std::string_view::npos) return loaded;
    remaining.remove_prefix(start);
    if (!remaining.starts_with(kBegin)) return false;
    const auto end = remaining.find(kEnd);
    if (end == std::string_view::npos) return false;
    const auto pem = remaining.substr(0, end + kEnd.size());
    std::unique_ptr<BIO, decltype(&BIO_free)> bio(
        BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())), BIO_free);
    if (!bio) return false;
    std::unique_ptr<X509, decltype(&X509_free)> certificate(
        PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr), X509_free);
    if (!certificate ||
        X509_STORE_add_cert(SSL_CTX_get_cert_store(context), certificate.get()) != 1) {
      return false;
    }
    loaded = true;
    remaining.remove_prefix(pem.size());
  }
}

}  // namespace

namespace internal {

struct TlsStream::Impl {
  NativeSocket socket{kInvalidSocket};
  std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> context{nullptr, SSL_CTX_free};
  std::unique_ptr<SSL, decltype(&SSL_free)> ssl{nullptr, SSL_free};
  BorrowedPtr<const std::atomic<bool>> cancellation{nullptr};
  TransportError error{TransportError::kNone};

  ~Impl() {
    ssl.reset();
    context.reset();
    CloseSocket(socket);
  }

  bool Check(Deadline deadline) {
    if (cancellation && cancellation->load()) {
      error = TransportError::kCancelled;
      return false;
    }
    if (Clock::now() >= deadline) {
      error = TransportError::kTimeout;
      return false;
    }
    return true;
  }

  bool Wait(int ssl_error, Deadline deadline) {
    if (ssl_error != SSL_ERROR_WANT_READ && ssl_error != SSL_ERROR_WANT_WRITE) {
      error = TransportError::kIo;
      return false;
    }
    while (Check(deadline)) {
      auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - Clock::now()).count();
      int wait_ms = static_cast<int>(std::clamp<int64_t>(remaining, 1, 50));
#if defined(_WIN32)
      WSAPOLLFD pfd{};
#else
      pollfd pfd{};
#endif
      pfd.fd = socket;
      pfd.events = ssl_error == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT;
#if defined(_WIN32)
      int result = WSAPoll(&pfd, 1, wait_ms);
#else
      int result = poll(&pfd, 1, wait_ms);
#endif
      if (result > 0) return true;  // SSL consumes EOF/error and pending records.
      if (result < 0 && !IsRetryableSocketError()) {
        error = TransportError::kIo;
        return false;
      }
    }
    return false;
  }
};

TlsStream::TlsStream(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
TlsStream::~TlsStream() = default;

std::unique_ptr<TlsStream> TlsStream::Connect(
    const std::string& host, uint16_t port, const TlsConfig& config,
    int timeout_ms, const std::atomic<bool>* cancellation,
    TransportError* error) {
  const auto deadline = Clock::now() + std::chrono::milliseconds(std::max(1, timeout_ms));
  auto connection = LanTcpClient::Connect(host, port, timeout_ms, cancellation, error);
  if (!connection) return nullptr;
  return Establish(std::move(connection), config, false, deadline, cancellation, error);
}

std::unique_ptr<TlsStream> TlsStream::Accept(
    std::unique_ptr<LanTcpConnection> connection, const TlsConfig& config,
    int timeout_ms, const std::atomic<bool>* cancellation,
    TransportError* error) {
  return Establish(std::move(connection), config, true,
                   Clock::now() + std::chrono::milliseconds(std::max(1, timeout_ms)),
                   cancellation, error);
}

std::unique_ptr<TlsStream> TlsStream::Establish(
    std::unique_ptr<LanTcpConnection> connection, const TlsConfig& config,
    bool server, Deadline deadline, const std::atomic<bool>* cancellation,
    TransportError* error) {
  if (error) *error = TransportError::kSecurity;
  if (!connection || !connection->IsConnected() || config.peer_name.empty()) return nullptr;
  auto impl = std::make_unique<Impl>();
  impl->socket = connection->ReleaseSocket();
  impl->cancellation = cancellation;
  impl->context.reset(SSL_CTX_new(TLS_method()));
  if (!impl->context || !SSL_CTX_set_min_proto_version(impl->context.get(), TLS1_2_VERSION)) return nullptr;
  SSL_CTX_set_verify(impl->context.get(),
                     SSL_VERIFY_PEER | (server ? SSL_VERIFY_FAIL_IF_NO_PEER_CERT : 0),
                     nullptr);
  if (!LoadTrustAnchors(impl->context.get(), config)) return nullptr;
  if (config.certificate_file.empty() != config.private_key_file.empty()) return nullptr;
  if (server && config.certificate_file.empty()) return nullptr;
  if (!config.certificate_file.empty() &&
      (SSL_CTX_use_certificate_chain_file(impl->context.get(), config.certificate_file.c_str()) != 1 ||
       SSL_CTX_use_PrivateKey_file(impl->context.get(), config.private_key_file.c_str(), SSL_FILETYPE_PEM) != 1 ||
       SSL_CTX_check_private_key(impl->context.get()) != 1)) return nullptr;
  impl->ssl.reset(SSL_new(impl->context.get()));
  if (!impl->ssl || !ConfigurePeerName(impl->ssl.get(), config.peer_name)) return nullptr;
  if (!server && SSL_set_tlsext_host_name(impl->ssl.get(), config.peer_name.c_str()) != 1) return nullptr;
  const auto* bio_method = SocketBioMethod();
  if (!bio_method) return nullptr;
  BIO* bio = BIO_new(bio_method);
  if (!bio) return nullptr;
  BIO_set_data(bio, &impl->socket);
  SSL_set_bio(impl->ssl.get(), bio, bio);
  if (server) SSL_set_accept_state(impl->ssl.get());
  else SSL_set_connect_state(impl->ssl.get());
  while (impl->Check(deadline)) {
    int result = SSL_do_handshake(impl->ssl.get());
    if (result == 1) {
      if (SSL_get_verify_result(impl->ssl.get()) != X509_V_OK) return nullptr;
      if (error) *error = TransportError::kNone;
      return std::unique_ptr<TlsStream>(new TlsStream(std::move(impl)));
    }
    const int ssl_error = SSL_get_error(impl->ssl.get(), result);
    if (!impl->Wait(ssl_error, deadline)) {
      if (error) *error = impl->error == TransportError::kIo
                              ? TransportError::kSecurity : impl->error;
      return nullptr;
    }
  }
  if (error) *error = impl->error;
  return nullptr;
}

bool TlsStream::Write(std::span<const uint8_t> data, Deadline deadline) {
  if (!IsConnected()) { impl_->error = TransportError::kNotConnected; return false; }
  impl_->error = TransportError::kNone;
  while (!data.empty()) {
    if (!impl_->Check(deadline)) return false;
    int n = SSL_write(impl_->ssl.get(), data.data(), static_cast<int>(std::min<size_t>(data.size(), INT_MAX)));
    if (n > 0) data = data.subspan(n);
    else if (!impl_->Wait(SSL_get_error(impl_->ssl.get(), n), deadline)) return false;
  }
  return true;
}

bool TlsStream::Read(std::span<uint8_t> data, Deadline deadline) {
  if (!IsConnected()) { impl_->error = TransportError::kNotConnected; return false; }
  impl_->error = TransportError::kNone;
  while (!data.empty()) {
    if (!impl_->Check(deadline)) return false;
    int n = SSL_read(impl_->ssl.get(), data.data(), static_cast<int>(std::min<size_t>(data.size(), INT_MAX)));
    if (n > 0) data = data.subspan(n);
    else if (!impl_->Wait(SSL_get_error(impl_->ssl.get(), n), deadline)) return false;
  }
  return true;
}

void TlsStream::Close() {
  // No potentially blocking SSL_shutdown during cancellation or destruction.
  CloseSocket(impl_->socket);
  impl_->socket = kInvalidSocket;
}
bool TlsStream::IsConnected() const { return impl_->socket != kInvalidSocket; }
void TlsStream::SetCancellationFlag(const std::atomic<bool>* cancellation) { impl_->cancellation = cancellation; }
TransportError TlsStream::LastError() const { return impl_->error; }

}  // namespace internal

TlsTransportChannel::TlsTransportChannel(std::unique_ptr<internal::TlsStream> stream,
                                         int timeout_ms)
    : stream_(std::move(stream)), timeout_ms_(std::max(1, timeout_ms)) {}
TlsTransportChannel::~TlsTransportChannel() = default;
TransportType TlsTransportChannel::GetType() const { return TransportType::kLanDirectTcp; }
bool TlsTransportChannel::IsSecure() const { return true; }

std::unique_ptr<TlsTransportChannel> TlsTransportChannel::Connect(
    const std::string& host, uint16_t port, const TlsConfig& config,
    int timeout_ms, const std::atomic<bool>* cancellation, TransportError* error) {
  if (config.certificate_file.empty() || config.private_key_file.empty()) {
    if (error) *error = TransportError::kSecurity;
    return nullptr;
  }
  auto stream = internal::TlsStream::Connect(host, port, config, timeout_ms, cancellation, error);
  return stream ? std::unique_ptr<TlsTransportChannel>(new TlsTransportChannel(std::move(stream), timeout_ms)) : nullptr;
}

std::unique_ptr<TlsTransportChannel> TlsTransportChannel::Accept(
    std::unique_ptr<LanTcpConnection> connection, const TlsConfig& config,
    int timeout_ms, const std::atomic<bool>* cancellation, TransportError* error) {
  auto stream = internal::TlsStream::Accept(std::move(connection), config, timeout_ms, cancellation, error);
  return stream ? std::unique_ptr<TlsTransportChannel>(new TlsTransportChannel(std::move(stream), timeout_ms)) : nullptr;
}

bool TlsTransportChannel::SendFrame(const FrameHeader& header, std::span<const uint8_t> payload) {
  error_ = TransportError::kNone;
  if (!ValidateFrameHeader(header) || header.payload_len != payload.size() ||
      header.checksum != ComputeCRC32C(payload)) {
    error_ = TransportError::kProtocol;
    return false;
  }
  const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms_);
  const auto bytes = header.Serialize();
  if (!stream_->Write(bytes, deadline) || !stream_->Write(payload, deadline)) {
    error_ = stream_->LastError();
    Close();
    return false;
  }
  return true;
}

bool TlsTransportChannel::ReadNextFrame(FrameHeader& header, std::vector<uint8_t>& payload) {
  error_ = TransportError::kNone;
  const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms_);
  std::array<uint8_t, FrameHeader::kWireSize> bytes;
  if (!stream_->Read(bytes, deadline)) {
    error_ = stream_->LastError(); Close(); return false;
  }
  auto parsed = FrameHeader::Deserialize(bytes);
  if (!parsed || !ValidateFrameHeader(*parsed)) {
    error_ = TransportError::kProtocol; Close(); return false;
  }
  header = *parsed;
  payload.resize(header.payload_len);
  if (!stream_->Read(payload, deadline)) {
    error_ = stream_->LastError(); Close(); return false;
  }
  if (ComputeCRC32C(payload) != header.checksum) {
    error_ = TransportError::kProtocol; Close(); return false;
  }
  return true;
}

void TlsTransportChannel::Close() { stream_->Close(); }
bool TlsTransportChannel::IsConnected() const { return stream_->IsConnected(); }
void TlsTransportChannel::SetCancellationFlag(const std::atomic<bool>* flag) { stream_->SetCancellationFlag(flag); }
void TlsTransportChannel::SetIoTimeout(int ms) { timeout_ms_ = std::max(1, ms); }
TransportError TlsTransportChannel::LastError() const { return error_; }

}  // namespace cross_transfer
