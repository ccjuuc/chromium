#ifndef CROSS_TRANSFER_INCLUDE_CROSS_TRANSFER_HTTPS_RELAY_CHANNEL_H_
#define CROSS_TRANSFER_INCLUDE_CROSS_TRANSFER_HTTPS_RELAY_CHANNEL_H_

#include "cross_transfer/transport_channel.h"

#include <memory>
#include <string>

namespace cross_transfer {

enum class RelayRole { kSender, kReceiver };

// The pairing/control plane provisions one room and distinct role credentials.
// connection_id is a shared random 128-bit hex value for this connection attempt,
// not the persistent file task ID. Both peers actively connect to this HTTPS
// service. The service terminates TLS and is a trusted party (not E2EE).
struct HttpsRelayOptions {
  std::string host;
  uint16_t port{443};
  std::string room_id;
  std::string connection_id;
  RelayRole role{RelayRole::kSender};
  std::string bearer_token;  // Exactly 64 hex characters; never logged.
  std::string ca_file;       // Empty uses the TLS library's default trust store.
  int timeout_ms{30000};
  std::string ca_pem;  // Optional embedded roots, mutually exclusive with ca_file.
};

// Own on one dedicated worker sequence. Send/receive are bounded operations,
// not concurrent; the cancellation flag may be set from another thread. HTTP
// mailboxes are bounded and provide application-level backpressure. Automatic
// retries are restricted to explicit 204/429 responses, never ambiguous writes.
class HttpsRelayChannel final : public ITransportChannel {
 public:
  static std::unique_ptr<HttpsRelayChannel> Connect(
      HttpsRelayOptions options,
      const std::atomic<bool>* cancellation = nullptr,
      TransportError* error = nullptr);
  ~HttpsRelayChannel() override;

  bool SendFrame(const FrameHeader&, std::span<const uint8_t>) override;
  bool ReadNextFrame(FrameHeader&, std::vector<uint8_t>&) override;
  void Close() override;
  bool IsConnected() const override;
  TransportType GetType() const override;
  bool IsSecure() const override;
  void SetCancellationFlag(const std::atomic<bool>*) override;
  void SetIoTimeout(int timeout_ms) override;
  TransportError LastError() const override;

 private:
  struct Impl;
  explicit HttpsRelayChannel(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace cross_transfer

#endif  // CROSS_TRANSFER_INCLUDE_CROSS_TRANSFER_HTTPS_RELAY_CHANNEL_H_
