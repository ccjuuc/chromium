#ifndef CROSS_TRANSFER_INCLUDE_CROSS_TRANSFER_TRANSPORT_CHANNEL_H_
#define CROSS_TRANSFER_INCLUDE_CROSS_TRANSFER_TRANSPORT_CHANNEL_H_

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "cross_transfer/borrowed_ptr.h"
#include "cross_transfer/network_nic_helper.h"
#include "cross_transfer/protocol.h"

namespace cross_transfer {
#if defined(_WIN32)
using NativeSocket = std::uintptr_t;
#else
using NativeSocket = int;
#endif
inline constexpr NativeSocket kInvalidSocket = static_cast<NativeSocket>(-1);

enum class TransportType { kLanDirectTcp, kWebRtcData, kCloudRelay443 };
enum class TransportError {
  kNone,
  kNotConnected,
  kCancelled,
  kTimeout,
  kIo,
  kProtocol,
  kResourceLimit,
  kSecurity,
};

// Synchronous worker-thread interface: callers serialize
// SendFrame/ReadNextFrame and never invoke them on a UI or networking
// event-loop thread. Each operation has a finite deadline; cancellation must
// interrupt waits. A supplied flag must outlive the channel or be detached with
// SetCancellationFlag(nullptr). Implementations validate framing before
// allocating a payload. IsSecure means traffic is encrypted AND the intended
// peer/service has been authenticated.
class ITransportChannel {
 public:
  virtual ~ITransportChannel();
  virtual bool SendFrame(const FrameHeader&, std::span<const uint8_t>) = 0;
  virtual bool ReadNextFrame(FrameHeader&, std::vector<uint8_t>&) = 0;
  virtual void Close() = 0;
  virtual bool IsConnected() const = 0;
  virtual TransportType GetType() const = 0;
  virtual bool IsSecure() const;
  // Defaults keep test channels compatible. Production adapters implement
  // these.
  virtual void SetCancellationFlag(const std::atomic<bool>*);
  virtual void SetIoTimeout(int);
  virtual TransportError LastError() const;
};

class LanTcpConnection : public ITransportChannel {
 public:
  explicit LanTcpConnection(NativeSocket socket_fd,
                            const std::string& peer_ip,
                            uint16_t peer_port);
  ~LanTcpConnection() override;
  bool SendFrame(const FrameHeader&, std::span<const uint8_t>) override;
  bool ReadNextFrame(FrameHeader&, std::vector<uint8_t>&) override;
  // Close may interrupt I/O from another thread. Join the worker before
  // destroy.
  void Close() override;
  bool IsConnected() const override;
  TransportType GetType() const override;
  void SetCancellationFlag(const std::atomic<bool>* flag) override;
  void SetIoTimeout(int timeout_ms) override;
  TransportError LastError() const override;
  const std::string& peer_ip() const { return peer_ip_; }
  uint16_t peer_port() const { return peer_port_; }
  // Transfers the nonblocking socket to a TLS adapter. Destructor won't close
  // it.
  NativeSocket ReleaseSocket();

 private:
  using Deadline = std::chrono::steady_clock::time_point;
  bool WaitForIoLocked(bool writing, Deadline deadline);
  bool ReadExactLocked(std::span<uint8_t> data, Deadline deadline);
  bool WriteExactLocked(std::span<const uint8_t> data, Deadline deadline);
  bool FailLocked(TransportError error);
  void CloseLocked();
  bool CancelledLocked() const;

  mutable std::mutex io_mutex_;
  NativeSocket socket_fd_{kInvalidSocket};
  std::string peer_ip_;
  uint16_t peer_port_{0};
  int io_timeout_ms_{30000};
  BorrowedPtr<const std::atomic<bool>> cancellation_{nullptr};
  std::atomic<bool> close_requested_{false};
  std::atomic<bool> is_connected_{false};
  std::atomic<TransportError> last_error_{TransportError::kNone};
};

class LanTcpServer {
 public:
  LanTcpServer();
  ~LanTcpServer();
  // Empty address tries dual-stack IPv6 then IPv4. Explicit numeric addresses
  // support IPv4 and scoped IPv6.
  bool Start(uint16_t port, const std::string& bind_address = {});
  std::unique_ptr<LanTcpConnection> Accept(int timeout_ms = 30000);
  void SetCancellationFlag(const std::atomic<bool>* flag);
  uint16_t listening_port() const { return port_.load(); }
  TransportError LastError() const { return last_error_.load(); }
  // May interrupt Accept from another thread. Join that thread before destroy.
  void Stop();

 private:
  mutable std::mutex mutex_;
  NativeSocket listen_fd_{kInvalidSocket};
  std::atomic<uint16_t> port_{0};
  std::atomic<bool> stop_requested_{true};
  BorrowedPtr<const std::atomic<bool>> cancellation_{nullptr};
  std::atomic<TransportError> last_error_{TransportError::kNone};
};

class LanTcpClient {
 public:
  static std::unique_ptr<LanTcpConnection> Connect(
      const std::string& host,
      uint16_t port,
      int timeout_ms = 3000,
      const std::atomic<bool>* cancellation = nullptr,
      TransportError* error = nullptr);
  // Races IPv4, IPv6 and hostname candidates until success or a shared
  // deadline. Resolver workers own their results and are concurrency-bounded;
  // cancellation does not wait for the platform's blocking resolver to finish.
  static std::unique_ptr<LanTcpConnection> RaceConnect(
      const std::vector<CandidateEndpoint>& candidates,
      int timeout_ms = 1000,
      const std::atomic<bool>* cancellation = nullptr,
      TransportError* error = nullptr);
};
}  // namespace cross_transfer
#endif  // CROSS_TRANSFER_INCLUDE_CROSS_TRANSFER_TRANSPORT_CHANNEL_H_
