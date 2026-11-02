#ifndef CROSS_TRANSFER_INCLUDE_CROSS_TRANSFER_TLS_TRANSPORT_CHANNEL_H_
#define CROSS_TRANSFER_INCLUDE_CROSS_TRANSFER_TLS_TRANSPORT_CHANNEL_H_

#include "cross_transfer/transport_channel.h"

#include <chrono>
#include <memory>
#include <string>

namespace cross_transfer {

// Trust is supplied by the pairing/application layer. No insecure verification
// mode is supported. Direct peers must both present certificates; peer_name
// identifies the expected peer certificate SAN, independently of its IP address.
struct TlsConfig {
  std::string ca_file;
  std::string certificate_file;
  std::string private_key_file;
  std::string peer_name;
};

namespace internal {

// Blocking, bounded I/O for a dedicated worker sequence. Only the atomic
// cancellation flag may be changed concurrently; Close/destruction run on the
// owner sequence. One deadline covers all TLS records of an operation.
class TlsStream {
 public:
  using Deadline = std::chrono::steady_clock::time_point;
  static std::unique_ptr<TlsStream> Connect(
      const std::string& host, uint16_t port, const TlsConfig& config,
      int timeout_ms, const std::atomic<bool>* cancellation,
      TransportError* error);
  static std::unique_ptr<TlsStream> Accept(
      std::unique_ptr<LanTcpConnection> connection, const TlsConfig& config,
      int timeout_ms, const std::atomic<bool>* cancellation,
      TransportError* error);
  ~TlsStream();

  bool Write(std::span<const uint8_t> data, Deadline deadline);
  bool Read(std::span<uint8_t> data, Deadline deadline);
  void Close();
  bool IsConnected() const;
  void SetCancellationFlag(const std::atomic<bool>* cancellation);
  TransportError LastError() const;

 private:
  struct Impl;
  explicit TlsStream(std::unique_ptr<Impl> impl);
  static std::unique_ptr<TlsStream> Establish(
      std::unique_ptr<LanTcpConnection> connection, const TlsConfig& config,
      bool server, Deadline deadline, const std::atomic<bool>* cancellation,
      TransportError* error);
  std::unique_ptr<Impl> impl_;
};

}  // namespace internal

class TlsTransportChannel final : public ITransportChannel {
 public:
  static std::unique_ptr<TlsTransportChannel> Connect(
      const std::string& host, uint16_t port, const TlsConfig& config,
      int timeout_ms = 30000,
      const std::atomic<bool>* cancellation = nullptr,
      TransportError* error = nullptr);
  static std::unique_ptr<TlsTransportChannel> Accept(
      std::unique_ptr<LanTcpConnection> connection, const TlsConfig& config,
      int timeout_ms = 30000,
      const std::atomic<bool>* cancellation = nullptr,
      TransportError* error = nullptr);
  ~TlsTransportChannel() override;

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
  TlsTransportChannel(std::unique_ptr<internal::TlsStream> stream, int timeout_ms);
  std::unique_ptr<internal::TlsStream> stream_;
  int timeout_ms_;
  TransportError error_{TransportError::kNone};
};

}  // namespace cross_transfer

#endif  // CROSS_TRANSFER_INCLUDE_CROSS_TRANSFER_TLS_TRANSPORT_CHANNEL_H_
