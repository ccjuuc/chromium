#include "cross_transfer/transport_channel.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cstring>
#include <thread>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#endif

namespace cross_transfer {
namespace {
using Clock = std::chrono::steady_clock;
using Deadline = Clock::time_point;
constexpr int kPollIntervalMs = 25;
constexpr size_t kMaxAttempts = 128;
constexpr size_t kMaxCandidates = 64;
constexpr unsigned kMaxDnsWorkers = 8;
std::atomic<unsigned> g_dns_workers{0};
#if defined(_WIN32)
using PollFd = WSAPOLLFD;
using AddressLength = int;
#else
using PollFd = pollfd;
using AddressLength = socklen_t;
#endif

bool InitializeSockets() {
#if defined(_WIN32)
  // Process-lifetime reference: resolver workers may outlive a connect attempt.
  static const bool initialized = [] {
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }();
  return initialized;
#else
  return true;
#endif
}
void CloseSocket(NativeSocket fd) {
  if (fd == kInvalidSocket) {
    return;
  }
#if defined(_WIN32)
  closesocket(fd);
#else
  close(fd);
#endif
}
int SocketError() {
#if defined(_WIN32)
  return WSAGetLastError();
#else
  return errno;
#endif
}
bool Interrupted(int error) {
#if defined(_WIN32)
  return error == WSAEINTR;
#else
  return error == EINTR;
#endif
}
bool WouldBlock(int error) {
#if defined(_WIN32)
  return error == WSAEWOULDBLOCK;
#else
  return error == EWOULDBLOCK || error == EAGAIN;
#endif
}
bool ConnectPending(int error) {
#if defined(_WIN32)
  return WouldBlock(error) || error == WSAEINPROGRESS || error == WSAEALREADY ||
         Interrupted(error);
#else
  return WouldBlock(error) || error == EINPROGRESS || error == EALREADY ||
         Interrupted(error);
#endif
}
bool MakeNonBlocking(NativeSocket fd) {
#if defined(_WIN32)
  u_long mode = 1;
  return ioctlsocket(fd, FIONBIO, &mode) == 0;
#else
  int flags = fcntl(fd, F_GETFL, 0);
  return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}
int Poll(PollFd* fds, size_t count, int timeout_ms) {
#if defined(_WIN32)
  return WSAPoll(fds, static_cast<ULONG>(count), timeout_ms);
#else
  return poll(fds, static_cast<nfds_t>(count), timeout_ms);
#endif
}
int RemainingMs(Deadline deadline) {
  auto remaining = deadline - Clock::now();
  if (remaining <= Clock::duration::zero()) {
    return 0;
  }
  auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count();
  return static_cast<int>(std::min<int64_t>(INT_MAX, std::max<int64_t>(1, ms)));
}
Deadline MakeDeadline(int timeout_ms) {
  return Clock::now() + std::chrono::milliseconds(std::max(0, timeout_ms));
}
bool Cancelled(const std::atomic<bool>* flag) {
  return flag && flag->load();
}

struct Address {
  sockaddr_storage storage{};
  AddressLength length{0};
  std::string ip;
  uint16_t port{0};
};
std::vector<Address> Resolve(const std::string& host,
                             uint16_t port,
                             bool numeric_only) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  hints.ai_flags = AI_NUMERICSERV | (numeric_only ? AI_NUMERICHOST : 0);
  addrinfo* results = nullptr;
  if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints,
                  &results) != 0) {
    return {};
  }
  std::vector<Address> addresses;
  for (auto* item = results; item && addresses.size() < kMaxAttempts;
       item = item->ai_next) {
    if ((item->ai_family != AF_INET && item->ai_family != AF_INET6) ||
        item->ai_addrlen > sizeof(sockaddr_storage)) {
      continue;
    }
    Address address;
    address.length = static_cast<AddressLength>(item->ai_addrlen);
    const auto source = std::span(
        reinterpret_cast<const std::byte*>(item->ai_addr), item->ai_addrlen);
    auto destination = std::as_writable_bytes(std::span(&address.storage, 1));
    std::copy(source.begin(), source.end(), destination.begin());
    char name[NI_MAXHOST]{};
    if (getnameinfo(item->ai_addr, address.length, name, sizeof(name), nullptr,
                    0, NI_NUMERICHOST) != 0) {
      continue;
    }
    address.ip = name;
    address.port = port;
    addresses.push_back(std::move(address));
  }
  freeaddrinfo(results);
  return addresses;
}
struct Resolution {
  std::mutex mutex;
  bool done{false};
  std::vector<Address> addresses;
};
std::shared_ptr<Resolution> StartResolution(std::string host, uint16_t port) {
  unsigned workers = g_dns_workers.load();
  do {
    if (workers >= kMaxDnsWorkers) {
      return nullptr;
    }
  } while (!g_dns_workers.compare_exchange_weak(workers, workers + 1));
  auto state = std::make_shared<Resolution>();
  std::thread([state, host = std::move(host), port] {
    auto addresses = Resolve(host, port, false);
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      state->addresses = std::move(addresses);
      state->done = true;
    }
    --g_dns_workers;
  }).detach();
  return state;
}
}  // namespace

ITransportChannel::~ITransportChannel() = default;
bool ITransportChannel::IsSecure() const { return false; }
void ITransportChannel::SetCancellationFlag(const std::atomic<bool>*) {}
void ITransportChannel::SetIoTimeout(int) {}
TransportError ITransportChannel::LastError() const { return TransportError::kNone; }

LanTcpConnection::LanTcpConnection(NativeSocket socket_fd,
                                   const std::string& peer_ip,
                                   uint16_t peer_port)
    : socket_fd_(socket_fd), peer_ip_(peer_ip), peer_port_(peer_port) {
  if (socket_fd_ == kInvalidSocket || !InitializeSockets() ||
      !MakeNonBlocking(socket_fd_)) {
    CloseLocked();
    last_error_ = TransportError::kIo;
    return;
  }
  int flag = 1;
  setsockopt(socket_fd_, IPPROTO_TCP, TCP_NODELAY,
             reinterpret_cast<const char*>(&flag), sizeof(flag));
#if defined(__APPLE__)
  if (setsockopt(socket_fd_, SOL_SOCKET, SO_NOSIGPIPE, &flag, sizeof(flag)) !=
      0) {
    CloseLocked();
    last_error_ = TransportError::kIo;
    return;
  }
#endif
  is_connected_ = true;
}
LanTcpConnection::~LanTcpConnection() {
  Close();
}
TransportType LanTcpConnection::GetType() const {
  return TransportType::kLanDirectTcp;
}
TransportError LanTcpConnection::LastError() const { return last_error_.load(); }

void LanTcpConnection::SetCancellationFlag(const std::atomic<bool>* flag) {
  std::lock_guard<std::mutex> lock(io_mutex_);
  cancellation_ = flag;
}
void LanTcpConnection::SetIoTimeout(int timeout_ms) {
  std::lock_guard<std::mutex> lock(io_mutex_);
  io_timeout_ms_ = std::max(1, timeout_ms);
}
bool LanTcpConnection::CancelledLocked() const {
  return close_requested_.load() || Cancelled(cancellation_);
}
void LanTcpConnection::CloseLocked() {
  CloseSocket(std::exchange(socket_fd_, kInvalidSocket));
  is_connected_ = false;
}
bool LanTcpConnection::FailLocked(TransportError error) {
  last_error_ = error;
  CloseLocked();
  return false;
}
bool LanTcpConnection::WaitForIoLocked(bool writing, Deadline deadline) {
  for (;;) {
    if (CancelledLocked()) {
      return FailLocked(TransportError::kCancelled);
    }
    const int remaining = RemainingMs(deadline);
    if (remaining == 0) {
      return FailLocked(TransportError::kTimeout);
    }
    PollFd pfd{};
    pfd.fd = socket_fd_;
    pfd.events = writing ? POLLOUT : POLLIN;
    int result = Poll(&pfd, 1, std::min(remaining, kPollIntervalMs));
    if (result < 0) {
      if (Interrupted(SocketError())) {
        continue;
      }
      return FailLocked(TransportError::kIo);
    }
    if (result > 0) {
      if (pfd.revents & POLLNVAL) {
        return FailLocked(TransportError::kIo);
      }
      if (pfd.revents & (pfd.events | POLLHUP | POLLERR)) {
        return true;
      }
    }
  }
}
bool LanTcpConnection::ReadExactLocked(std::span<uint8_t> data,
                                       Deadline deadline) {
  size_t offset = 0;
  while (offset < data.size()) {
    if (!WaitForIoLocked(false, deadline)) {
      return false;
    }
    int count = static_cast<int>(std::min<size_t>(INT_MAX, data.size() - offset));
    auto read =
        recv(socket_fd_, reinterpret_cast<char*>(data.subspan(offset).data()), count, 0);
    if (read > 0) {
      offset += static_cast<size_t>(read);
    } else if (read == 0) {
      return FailLocked(TransportError::kNotConnected);
    } else {
      int error = SocketError();
      if (!Interrupted(error) && !WouldBlock(error)) {
        return FailLocked(TransportError::kIo);
      }
    }
  }
  return true;
}
bool LanTcpConnection::WriteExactLocked(std::span<const uint8_t> data,
                                        Deadline deadline) {
  size_t offset = 0;
  while (offset < data.size()) {
    if (!WaitForIoLocked(true, deadline)) {
      return false;
    }
    int count = static_cast<int>(std::min<size_t>(INT_MAX, data.size() - offset));
    int flags = 0;
#if defined(MSG_NOSIGNAL)
    flags = MSG_NOSIGNAL;
#endif
    auto written = send(
        socket_fd_, reinterpret_cast<const char*>(data.subspan(offset).data()), count, flags);
    if (written > 0) {
      offset += static_cast<size_t>(written);
    } else if (written == 0) {
      return FailLocked(TransportError::kNotConnected);
    } else {
      int error = SocketError();
      if (!Interrupted(error) && !WouldBlock(error)) {
        return FailLocked(TransportError::kIo);
      }
    }
  }
  return true;
}
bool LanTcpConnection::SendFrame(const FrameHeader& header,
                                 std::span<const uint8_t> payload) {
  std::lock_guard<std::mutex> lock(io_mutex_);
  if (!is_connected_) {
    last_error_ = TransportError::kNotConnected;
    return false;
  }
  if (CancelledLocked()) {
    return FailLocked(TransportError::kCancelled);
  }
  if (!ValidateFrameHeader(header) || header.payload_len != payload.size() ||
      header.checksum != ComputeCRC32C(payload)) {
    return FailLocked(TransportError::kProtocol);
  }
  auto bytes = header.Serialize();
  Deadline deadline = MakeDeadline(io_timeout_ms_);
  if (!WriteExactLocked(bytes, deadline) ||
      !WriteExactLocked(payload, deadline)) {
    return false;
  }
  last_error_ = TransportError::kNone;
  return true;
}
bool LanTcpConnection::ReadNextFrame(FrameHeader& out_header,
                                     std::vector<uint8_t>& out_payload) {
  std::lock_guard<std::mutex> lock(io_mutex_);
  out_payload.clear();
  if (!is_connected_) {
    last_error_ = TransportError::kNotConnected;
    return false;
  }
  if (CancelledLocked()) {
    return FailLocked(TransportError::kCancelled);
  }
  Deadline deadline = MakeDeadline(io_timeout_ms_);
  std::array<uint8_t, FrameHeader::kWireSize> bytes{};
  if (!ReadExactLocked(bytes, deadline)) {
    return false;
  }
  auto header = FrameHeader::Deserialize(bytes);
  if (!header || !ValidateFrameHeader(*header)) {
    return FailLocked(TransportError::kProtocol);
  }
  out_header = *header;
  out_payload.resize(header->payload_len);
  if (!ReadExactLocked(out_payload, deadline)) {
    out_payload.clear();
    return false;
  }
  if (ComputeCRC32C(out_payload) != header->checksum) {
    out_payload.clear();
    return FailLocked(TransportError::kProtocol);
  }
  last_error_ = TransportError::kNone;
  return true;
}
void LanTcpConnection::Close() {
  close_requested_ = true;
  std::lock_guard<std::mutex> lock(io_mutex_);
  CloseLocked();
}
bool LanTcpConnection::IsConnected() const {
  return is_connected_.load();
}
NativeSocket LanTcpConnection::ReleaseSocket() {
  std::lock_guard<std::mutex> lock(io_mutex_);
  close_requested_ = true;
  is_connected_ = false;
  return std::exchange(socket_fd_, kInvalidSocket);
}

LanTcpServer::LanTcpServer() = default;
LanTcpServer::~LanTcpServer() {
  Stop();
}
bool LanTcpServer::Start(uint16_t port, const std::string& bind_address) {
  Stop();
  std::lock_guard<std::mutex> lock(mutex_);
  last_error_ = TransportError::kIo;
  if (!InitializeSockets()) {
    return false;
  }
  std::vector<Address> addresses;
  if (bind_address.empty()) {
    addresses = Resolve("::", port, true);
    auto ipv4 = Resolve("0.0.0.0", port, true);
    addresses.insert(addresses.end(), ipv4.begin(), ipv4.end());
  } else {
    addresses = Resolve(bind_address, port, true);
  }
  for (const auto& address : addresses) {
    NativeSocket fd =
        socket(address.storage.ss_family, SOCK_STREAM, IPPROTO_TCP);
    if (fd == kInvalidSocket) {
      continue;
    }
    int one = 1;
#if defined(_WIN32)
    setsockopt(fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
               reinterpret_cast<const char*>(&one), sizeof(one));
#else
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#endif
    if (address.storage.ss_family == AF_INET6 && bind_address.empty()) {
      int zero = 0;
      if (setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY,
                     reinterpret_cast<const char*>(&zero), sizeof(zero)) != 0) {
        CloseSocket(fd);
        continue;
      }
    }
    if (!MakeNonBlocking(fd) ||
        bind(fd, reinterpret_cast<const sockaddr*>(&address.storage),
             address.length) != 0 ||
        listen(fd, 16) != 0) {
      CloseSocket(fd);
      continue;
    }
    sockaddr_storage actual{};
    AddressLength size = sizeof(actual);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&actual), &size) != 0) {
      CloseSocket(fd);
      continue;
    }
    port_ = actual.ss_family == AF_INET6
                ? ntohs(reinterpret_cast<sockaddr_in6*>(&actual)->sin6_port)
                : ntohs(reinterpret_cast<sockaddr_in*>(&actual)->sin_port);
    listen_fd_ = fd;
    stop_requested_ = false;
    last_error_ = TransportError::kNone;
    return true;
  }
  return false;
}
void LanTcpServer::SetCancellationFlag(const std::atomic<bool>* flag) {
  std::lock_guard<std::mutex> lock(mutex_);
  cancellation_ = flag;
}
std::unique_ptr<LanTcpConnection> LanTcpServer::Accept(int timeout_ms) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (listen_fd_ == kInvalidSocket) {
    last_error_ = TransportError::kNotConnected;
    return nullptr;
  }
  Deadline deadline = MakeDeadline(timeout_ms < 0 ? 30000 : timeout_ms);
  for (;;) {
    if (stop_requested_.load() || Cancelled(cancellation_)) {
      last_error_ = TransportError::kCancelled;
      return nullptr;
    }
    int remaining = RemainingMs(deadline);
    if (remaining == 0) {
      last_error_ = TransportError::kTimeout;
      return nullptr;
    }
    PollFd pfd{};
    pfd.fd = listen_fd_;
    pfd.events = POLLIN;
    int ready = Poll(&pfd, 1, std::min(remaining, kPollIntervalMs));
    if (ready == 0) {
      continue;
    }
    if (ready < 0) {
      if (Interrupted(SocketError())) {
        continue;
      }
      last_error_ = TransportError::kIo;
      return nullptr;
    }
    if (!(pfd.revents & POLLIN)) {
      last_error_ = TransportError::kIo;
      return nullptr;
    }
    sockaddr_storage peer{};
    AddressLength length = sizeof(peer);
    NativeSocket fd =
        accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &length);
    if (fd == kInvalidSocket) {
      int error = SocketError();
      if (WouldBlock(error) || Interrupted(error)) {
        continue;
      }
      last_error_ = TransportError::kIo;
      return nullptr;
    }
    char ip[NI_MAXHOST]{};
    if (getnameinfo(reinterpret_cast<sockaddr*>(&peer), length, ip, sizeof(ip),
                    nullptr, 0, NI_NUMERICHOST) != 0) {
      CloseSocket(fd);
      last_error_ = TransportError::kIo;
      return nullptr;
    }
    uint16_t port =
        peer.ss_family == AF_INET6
            ? ntohs(reinterpret_cast<sockaddr_in6*>(&peer)->sin6_port)
            : ntohs(reinterpret_cast<sockaddr_in*>(&peer)->sin_port);
    auto connection = std::make_unique<LanTcpConnection>(fd, ip, port);
    if (!connection->IsConnected()) {
      last_error_ = connection->LastError();
      return nullptr;
    }
    connection->SetCancellationFlag(cancellation_);
    last_error_ = TransportError::kNone;
    return connection;
  }
}
void LanTcpServer::Stop() {
  stop_requested_ = true;
  std::lock_guard<std::mutex> lock(mutex_);
  CloseSocket(std::exchange(listen_fd_, kInvalidSocket));
  port_ = 0;
}

std::unique_ptr<LanTcpConnection> LanTcpClient::Connect(
    const std::string& host,
    uint16_t port,
    int timeout_ms,
    const std::atomic<bool>* cancellation,
    TransportError* error) {
  CandidateEndpoint endpoint;
  endpoint.ip = host;
  endpoint.port = port;
  return RaceConnect({endpoint}, timeout_ms, cancellation, error);
}
std::unique_ptr<LanTcpConnection> LanTcpClient::RaceConnect(
    const std::vector<CandidateEndpoint>& candidates,
    int timeout_ms,
    const std::atomic<bool>* cancellation,
    TransportError* error) {
  auto fail = [&](TransportError value) -> std::unique_ptr<LanTcpConnection> {
    if (error) {
      *error = value;
    }
    return nullptr;
  };
  if (Cancelled(cancellation)) {
    return fail(TransportError::kCancelled);
  }
  if (!InitializeSockets()) {
    return fail(TransportError::kIo);
  }
  if (candidates.empty()) {
    return fail(TransportError::kNotConnected);
  }
  if (candidates.size() > kMaxCandidates) {
    return fail(TransportError::kResourceLimit);
  }
  if (timeout_ms <= 0) {
    return fail(TransportError::kTimeout);
  }
  Deadline deadline = MakeDeadline(timeout_ms);
  struct Attempt {
    NativeSocket fd;
    Address address;
  };
  std::vector<Attempt> attempts;
  std::vector<std::shared_ptr<Resolution>> resolutions;
  std::vector<Address> ready_addresses;
  bool resolution_limited = false;
  auto ordered = candidates;
  std::stable_sort(ordered.begin(), ordered.end(),
                   [](const auto& a, const auto& b) {
                     return a.priority_score > b.priority_score;
                   });
  for (const auto& candidate : ordered) {
    if (candidate.ip.empty() || candidate.port == 0) {
      continue;
    }
    auto resolved = Resolve(candidate.ip, candidate.port, true);
    if (!resolved.empty()) {
      ready_addresses.insert(ready_addresses.end(), resolved.begin(),
                             resolved.end());
    } else {
      auto state = StartResolution(candidate.ip, candidate.port);
      if (state) {
        resolutions.push_back(std::move(state));
      } else {
        resolution_limited = true;
      }
    }
  }
  auto close_attempts = [&] {
    for (auto& attempt : attempts) {
      CloseSocket(std::exchange(attempt.fd, kInvalidSocket));
    }
  };
  auto success = [&](NativeSocket fd, const Address& address) {
    if (Cancelled(cancellation) || RemainingMs(deadline) == 0) {
      CloseSocket(fd);
      close_attempts();
      return fail(Cancelled(cancellation) ? TransportError::kCancelled
                                          : TransportError::kTimeout);
    }
    auto connection =
        std::make_unique<LanTcpConnection>(fd, address.ip, address.port);
    close_attempts();
    connection->SetCancellationFlag(cancellation);
    if (!connection->IsConnected()) {
      if (error) {
        *error = connection->LastError();
      }
      connection.reset();
    } else if (error) {
      *error = TransportError::kNone;
    }
    return connection;
  };
  for (;;) {
    if (Cancelled(cancellation)) {
      close_attempts();
      return fail(TransportError::kCancelled);
    }
    if (RemainingMs(deadline) == 0) {
      close_attempts();
      return fail(TransportError::kTimeout);
    }
    for (auto& state : resolutions) {
      if (!state) {
        continue;
      }
      bool done;
      {
        std::lock_guard<std::mutex> lock(state->mutex);
        done = state->done;
        if (done) {
          ready_addresses.insert(ready_addresses.end(),
                                 state->addresses.begin(),
                                 state->addresses.end());
        }
      }
      if (done) {
        state.reset();
      }
    }
    for (auto& address : ready_addresses) {
      if (attempts.size() >= kMaxAttempts) {
        break;
      }
      NativeSocket fd =
          socket(address.storage.ss_family, SOCK_STREAM, IPPROTO_TCP);
      if (fd == kInvalidSocket) {
        continue;
      }
      if (!MakeNonBlocking(fd)) {
        CloseSocket(fd);
        continue;
      }
      int result =
          connect(fd, reinterpret_cast<const sockaddr*>(&address.storage),
                  address.length);
      if (result == 0) {
        return success(fd, address);
      }
      if (!ConnectPending(SocketError())) {
        CloseSocket(fd);
        continue;
      }
      attempts.push_back({fd, std::move(address)});
    }
    ready_addresses.clear();
    std::vector<PollFd> poll_fds;
    std::vector<size_t> indexes;
    for (size_t i = 0; i < attempts.size(); ++i) {
      if (attempts[i].fd == kInvalidSocket) {
        continue;
      }
      PollFd pfd{};
      pfd.fd = attempts[i].fd;
      pfd.events = POLLOUT;
      poll_fds.push_back(pfd);
      indexes.push_back(i);
    }
    bool resolving = std::any_of(resolutions.begin(), resolutions.end(),
                                 [](const auto& state) { return bool(state); });
    if (poll_fds.empty() && !resolving) {
      close_attempts();
      return fail(resolution_limited ? TransportError::kResourceLimit
                                     : TransportError::kIo);
    }
    int wait_ms = std::min(RemainingMs(deadline), kPollIntervalMs);
    if (poll_fds.empty()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
      continue;
    }
    int ready = Poll(poll_fds.data(), poll_fds.size(), wait_ms);
    if (ready < 0) {
      if (Interrupted(SocketError())) {
        continue;
      }
      close_attempts();
      return fail(TransportError::kIo);
    }
    if (ready == 0) {
      continue;
    }
    for (size_t i = 0; i < poll_fds.size(); ++i) {
      if (!(poll_fds[i].revents & (POLLOUT | POLLERR | POLLHUP | POLLNVAL))) {
        continue;
      }
      auto& attempt = attempts[indexes[i]];
      int socket_error = 0;
      AddressLength length = sizeof(socket_error);
      bool connected =
          !(poll_fds[i].revents & POLLNVAL) &&
          getsockopt(attempt.fd, SOL_SOCKET, SO_ERROR,
                     reinterpret_cast<char*>(&socket_error), &length) == 0 &&
          socket_error == 0 && (poll_fds[i].revents & POLLOUT);
      if (connected) {
        return success(std::exchange(attempt.fd, kInvalidSocket),
                       attempt.address);
      }
      CloseSocket(std::exchange(attempt.fd, kInvalidSocket));
    }
  }
}
}  // namespace cross_transfer
