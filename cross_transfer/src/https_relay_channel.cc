#include "cross_transfer/https_relay_channel.h"

#include "cross_transfer/tls_transport_channel.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cctype>
#include <map>
#include <memory>
#include <string_view>
#include <system_error>
#include <thread>

#include <openssl/rand.h>

namespace cross_transfer {
namespace {

using Clock = std::chrono::steady_clock;
constexpr size_t kMaxHttpHeaders = 8192;
constexpr size_t kHeaderSize = FrameHeader::kWireSize;

bool IsHex(std::string_view value, size_t length) {
  return value.size() == length && std::all_of(value.begin(), value.end(), [](unsigned char ch) {
    return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
  });
}

bool IsRoom(std::string_view value) {
  return !value.empty() && value.size() <= 64 &&
         std::all_of(value.begin(), value.end(), [](unsigned char ch) {
           return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                  (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
         });
}

bool ValidHost(std::string_view host) {
  return !host.empty() && host.size() <= 253 &&
         std::none_of(host.begin(), host.end(), [](unsigned char ch) {
           return ch <= 32 || ch >= 127 || ch == '/' || ch == '\\' || ch == '?' || ch == '#';
         });
}

std::string RandomLease() {
  std::array<uint8_t, 16> bytes;
  if (RAND_bytes(bytes.data(), bytes.size()) != 1) return {};
  constexpr std::string_view hex = "0123456789abcdef";
  std::string result;
  for (uint8_t byte : bytes) { result += hex[byte >> 4]; result += hex[byte & 15]; }
  return result;
}

std::string Lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return text;
}

std::string_view Trim(std::string_view value) {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
  return value;
}

}  // namespace

struct HttpsRelayChannel::Impl {
  HttpsRelayOptions options;
  std::string lease;
  BorrowedPtr<const std::atomic<bool>> cancellation{nullptr};
  TransportError error{TransportError::kNone};
  bool connected{false};
  bool leased{false};
  uint64_t received_sequence{0};

  bool Check(Clock::time_point deadline) {
    if (cancellation && cancellation->load()) { error = TransportError::kCancelled; return false; }
    if (Clock::now() >= deadline) { error = TransportError::kTimeout; return false; }
    return true;
  }

  // One TLS connection per bounded request avoids hidden persistent-connection
  // ownership and makes cancellation and reconnect behavior deterministic.
  bool RequestImpl(std::string_view method, std::string_view operation,
               std::span<const uint8_t> body, Clock::time_point deadline,
               int* status, std::vector<uint8_t>* response, uint64_t* sequence) {
    if (!Check(deadline)) return false;
    int remaining = static_cast<int>(std::max<int64_t>(1,
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count()));
    TlsConfig tls;
    tls.ca_file = options.ca_file;
    tls.peer_name = options.host;
    auto stream = internal::TlsStream::Connect(options.host, options.port, tls,
                                               remaining, cancellation, &error);
    if (!stream) return false;
    const std::string role = options.role == RelayRole::kSender ? "sender" : "receiver";
    std::string authority = options.host.find(':') == std::string::npos
                                ? options.host : "[" + options.host + "]";
    std::string request = std::string(method) + " /v1/rooms/" + options.room_id + "/" +
        options.connection_id + "/" + role + "/" + std::string(operation) + " HTTP/1.1\r\n" +
        "Host: " + authority + ":" + std::to_string(options.port) + "\r\n" +
        "Authorization: Bearer " + options.bearer_token + "\r\n" +
        "X-Relay-Lease: " + lease + "\r\n" +
        "X-Relay-Ack: " + std::to_string(received_sequence) + "\r\n" +
        "Content-Type: application/octet-stream\r\n" +
        "Content-Length: " + std::to_string(body.size()) + "\r\n" +
        "Connection: close\r\n\r\n";
    if (!stream->Write(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(request.data()), request.size()), deadline) ||
        !stream->Write(body, deadline)) { error = stream->LastError(); return false; }
    std::string headers;
    headers.reserve(512);
    while (!headers.ends_with("\r\n\r\n")) {
      uint8_t byte;
      if (!stream->Read(std::span<uint8_t>(&byte, 1), deadline)) { error = stream->LastError(); return false; }
      headers.push_back(static_cast<char>(byte));
      if (headers.size() > kMaxHttpHeaders) { error = TransportError::kResourceLimit; return false; }
    }
    size_t line_end = headers.find("\r\n");
    if (line_end == std::string::npos ||
        !(headers.starts_with("HTTP/1.1 ") || headers.starts_with("HTTP/1.0 ")) ||
        line_end < 12 || headers[12] != ' ') { error = TransportError::kProtocol; return false; }
    const auto status_text = std::string_view(headers).substr(9, 3);
    auto code = std::from_chars(std::to_address(status_text.begin()), std::to_address(status_text.end()), *status);
    if (code.ec != std::errc() || code.ptr != std::to_address(status_text.end())) { error = TransportError::kProtocol; return false; }
    std::map<std::string, std::string> fields;
    size_t offset = line_end + 2;
    while (offset < headers.size() - 2) {
      size_t end = headers.find("\r\n", offset);
      size_t colon = headers.find(':', offset);
      if (end == std::string::npos || colon == std::string::npos || colon >= end || colon == offset) {
        error = TransportError::kProtocol; return false;
      }
      std::string name = Lower(headers.substr(offset, colon - offset));
      if (fields.contains(name)) { error = TransportError::kProtocol; return false; }
      fields.emplace(std::move(name), Trim(std::string_view(headers).substr(colon + 1, end - colon - 1)));
      offset = end + 2;
    }
    auto length = fields.find("content-length");
    if (length == fields.end() || fields.contains("transfer-encoding")) { error = TransportError::kProtocol; return false; }
    size_t body_size = 0;
    const std::string_view length_text = length->second;
    auto number = std::from_chars(std::to_address(length_text.begin()), std::to_address(length_text.end()), body_size);
    if (number.ec != std::errc() || number.ptr != std::to_address(length_text.end())) {
      error = TransportError::kProtocol; return false;
    }
    if (body_size > kMaxFramePayload + kHeaderSize) { error = TransportError::kResourceLimit; return false; }
    response->resize(body_size);
    if (!stream->Read(*response, deadline)) { error = stream->LastError(); return false; }
    if (sequence && *status == 200) {
      const auto item = fields.find("x-relay-sequence");
      if (item == fields.end()) { error = TransportError::kProtocol; return false; }
      const std::string_view sequence_text = item->second;
      const auto parsed = std::from_chars(std::to_address(sequence_text.begin()), std::to_address(sequence_text.end()), *sequence);
      if (parsed.ec != std::errc() || parsed.ptr != std::to_address(sequence_text.end()) ||
          *sequence == 0 || *sequence <= received_sequence) { error = TransportError::kProtocol; return false; }
    }
    return true;
  }

  bool Request(std::string_view method, std::string_view operation,
               std::span<const uint8_t> body, Clock::time_point deadline,
               int* status, std::vector<uint8_t>* response, uint64_t* sequence = nullptr) {
    bool ok = RequestImpl(method, operation, body, deadline, status, response, sequence);
    if (!ok) connected = false;
    return ok;
  }

  void MapStatus(int status) {
    if (status == 401 || status == 403) error = TransportError::kSecurity;
    else if (status == 408 || status == 504) error = TransportError::kTimeout;
    else if (status == 413 || status == 429 || status == 503) error = TransportError::kResourceLimit;
    else if (status == 409 || status == 410) error = TransportError::kNotConnected;
    else error = TransportError::kProtocol;
  }
};

HttpsRelayChannel::HttpsRelayChannel(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

std::unique_ptr<HttpsRelayChannel> HttpsRelayChannel::Connect(
    HttpsRelayOptions options, const std::atomic<bool>* cancellation, TransportError* error) {
  if (error) *error = TransportError::kSecurity;
  if (!ValidHost(options.host) || options.port == 0 || !IsRoom(options.room_id) ||
      !IsHex(options.connection_id, 32) || !IsHex(options.bearer_token, 64) || options.timeout_ms <= 0) return nullptr;
  auto impl = std::make_unique<Impl>();
  impl->options = std::move(options);
  impl->cancellation = cancellation;
  impl->lease = RandomLease();
  if (impl->lease.empty()) return nullptr;
  int status = 0;
  std::vector<uint8_t> response;
  if (!impl->Request("POST", "open", {}, Clock::now() + std::chrono::milliseconds(impl->options.timeout_ms),
                     &status, &response)) {
    if (error) *error = impl->error;
    return nullptr;
  }
  if (status != 200 || !response.empty()) {
    impl->MapStatus(status);
    if (error) *error = impl->error;
    return nullptr;
  }
  impl->connected = true;
  impl->leased = true;
  if (error) *error = TransportError::kNone;
  return std::unique_ptr<HttpsRelayChannel>(new HttpsRelayChannel(std::move(impl)));
}

HttpsRelayChannel::~HttpsRelayChannel() { Close(); }
TransportType HttpsRelayChannel::GetType() const { return TransportType::kCloudRelay443; }
bool HttpsRelayChannel::IsSecure() const { return true; }

bool HttpsRelayChannel::SendFrame(const FrameHeader& header, std::span<const uint8_t> payload) {
  if (!impl_->connected) { impl_->error = TransportError::kNotConnected; return false; }
  impl_->error = TransportError::kNone;
  if (!ValidateFrameHeader(header) || header.payload_len != payload.size() ||
      header.checksum != ComputeCRC32C(payload)) { impl_->error = TransportError::kProtocol; return false; }
  auto bytes = header.Serialize();
  std::vector<uint8_t> body(bytes.begin(), bytes.end());
  body.insert(body.end(), payload.begin(), payload.end());
  const auto deadline = Clock::now() + std::chrono::milliseconds(impl_->options.timeout_ms);
  while (impl_->Check(deadline)) {
    int status = 0;
    std::vector<uint8_t> response;
    if (!impl_->Request("POST", "frames", body, deadline, &status, &response)) return false;
    if (status == 200 && response.empty()) return true;
    if (status != 429) { impl_->MapStatus(status); impl_->connected = false; return false; }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  impl_->connected = false;
  return false;
}

bool HttpsRelayChannel::ReadNextFrame(FrameHeader& header, std::vector<uint8_t>& payload) {
  if (!impl_->connected) { impl_->error = TransportError::kNotConnected; return false; }
  impl_->error = TransportError::kNone;
  const auto deadline = Clock::now() + std::chrono::milliseconds(impl_->options.timeout_ms);
  while (impl_->Check(deadline)) {
    int status = 0;
    uint64_t sequence = 0;
    std::vector<uint8_t> response;
    if (!impl_->Request("GET", "frames", {}, deadline, &status, &response, &sequence)) return false;
    if (status == 204 && response.empty()) continue;
    if (status != 200) { impl_->MapStatus(status); impl_->connected = false; return false; }
    if (response.size() < kHeaderSize) { impl_->error = TransportError::kProtocol; impl_->connected = false; return false; }
    auto parsed = FrameHeader::Deserialize(std::span<const uint8_t, kHeaderSize>(response.data(), kHeaderSize));
    if (!parsed || !ValidateFrameHeader(*parsed) || parsed->payload_len != response.size() - kHeaderSize) {
      impl_->error = TransportError::kProtocol; impl_->connected = false; return false;
    }
    header = *parsed;
    payload.assign(response.begin() + kHeaderSize, response.end());
    if (ComputeCRC32C(payload) != header.checksum) { impl_->error = TransportError::kProtocol; impl_->connected = false; return false; }
    // Acknowledge only after the complete response passed protocol validation.
    // A timed-out GET can never consume the next frame invisibly.
    impl_->received_sequence = sequence;
    return true;
  }
  impl_->connected = false;
  return false;
}

void HttpsRelayChannel::Close() {
  if (!impl_->leased) return;
  impl_->leased = false;
  impl_->connected = false;
  // Best effort lease release is itself bounded. Cancelled operations never
  // start more network work; the service TTL then reclaims the abandoned room.
  if (impl_->cancellation && impl_->cancellation->load()) return;
  TransportError saved = impl_->error;
  int status = 0;
  std::vector<uint8_t> response;
  impl_->Request("DELETE", "open", {}, Clock::now() + std::chrono::milliseconds(250), &status, &response);
  impl_->error = saved;
}
bool HttpsRelayChannel::IsConnected() const { return impl_->connected; }
void HttpsRelayChannel::SetCancellationFlag(const std::atomic<bool>* flag) { impl_->cancellation = flag; }
void HttpsRelayChannel::SetIoTimeout(int ms) { impl_->options.timeout_ms = std::max(1, ms); }
TransportError HttpsRelayChannel::LastError() const { return impl_->error; }

}  // namespace cross_transfer
