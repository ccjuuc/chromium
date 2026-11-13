// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_app_single_instance_transport.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <optional>
#include <utility>

#include "base/containers/span.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/json/json_reader.h"
#include "base/numerics/byte_conversions.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/threading/platform_thread.h"
#include "base/time/time.h"
#include "build/build_config.h"

#if BUILDFLAG(IS_WIN)
#include <windows.h>

#include <sddl.h>

#include "base/win/access_token.h"
#include "base/win/scoped_handle.h"
#include "base/win/sid.h"
#else
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "base/files/scoped_file.h"
#include "base/posix/eintr_wrapper.h"
#endif

namespace xenon::ipc {
namespace {

constexpr base::TimeDelta kIoTimeout = base::Seconds(2);

bool ValidMessage(std::string_view text, std::string_view lease) {
  auto value = base::JSONReader::ReadDict(text, base::JSON_PARSE_RFC, 64);
  if (!value || value->FindInt("version") != 1 || !value->FindString("lease") ||
      *value->FindString("lease") != lease || !value->FindList("argv") ||
      !value->FindString("cwd") || !value->FindDict("additionalData") ||
      !value->FindString("requestId") ||
      value->FindString("requestId")->empty() ||
      value->FindString("requestId")->size() > 128) {
    return false;
  }
  for (const auto& arg : *value->FindList("argv")) {
    if (!arg.is_string() || arg.GetString().find('\0') != std::string::npos) {
      return false;
    }
  }
  const auto& cwd = *value->FindString("cwd");
  return cwd.find('\0') == std::string::npos &&
         base::FilePath::FromUTF8Unsafe(cwd).IsAbsolute();
}

#if BUILDFLAG(IS_WIN)

using Connection = base::win::ScopedHandle;
using NativeConnection = HANDLE;
using StopHandle = HANDLE;

bool SameUserProcess(DWORD pid) {
  base::win::ScopedHandle process(
      ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
  if (!process.is_valid()) {
    return false;
  }
  auto remote = base::win::AccessToken::FromProcess(process.get());
  auto local = base::win::AccessToken::FromCurrentProcess();
  return remote && local && remote->User() == local->User();
}

bool FinishOverlapped(HANDLE pipe,
                      OVERLAPPED* operation,
                      HANDLE stop,
                      base::TimeTicks deadline,
                      DWORD* bytes) {
  const auto remaining = deadline - base::TimeTicks::Now();
  if (remaining.is_positive()) {
    const std::array<HANDLE, 2> events = {operation->hEvent, stop};
    const DWORD result = ::WaitForMultipleObjects(
        stop ? 2 : 1, events.data(), FALSE,
        static_cast<DWORD>(remaining.InMillisecondsRoundedUp()));
    if (result == WAIT_OBJECT_0) {
      return ::GetOverlappedResult(pipe, operation, bytes, FALSE) != FALSE;
    }
  }
  // The stack OVERLAPPED must outlive cancellation, including the completion
  // written by the kernel. Never abandon an in-flight read or connect.
  ::CancelIoEx(pipe, operation);
  ::GetOverlappedResult(pipe, operation, bytes, TRUE);
  return false;
}

bool Transfer(NativeConnection pipe,
              base::span<uint8_t> data,
              bool write,
              StopHandle stop,
              base::TimeTicks deadline) {
  while (!data.empty()) {
    base::win::ScopedHandle event(::CreateEvent(nullptr, TRUE, FALSE, nullptr));
    if (!event.is_valid()) {
      return false;
    }
    OVERLAPPED operation{};
    operation.hEvent = event.get();
    DWORD bytes = 0;
    const BOOL completed =
        write ? ::WriteFile(pipe, data.data(), static_cast<DWORD>(data.size()),
                            &bytes, &operation)
              : ::ReadFile(pipe, data.data(), static_cast<DWORD>(data.size()),
                           &bytes, &operation);
    if (!completed &&
        (::GetLastError() != ERROR_IO_PENDING ||
         !FinishOverlapped(pipe, &operation, stop, deadline, &bytes))) {
      return false;
    }
    if (bytes == 0 || bytes > data.size()) {
      return false;
    }
    data = data.subspan(bytes);
  }
  return true;
}

base::expected<Connection, std::string> Connect(const std::string& endpoint) {
  if (!endpoint.starts_with("\\\\.\\pipe\\xenon-app-instance-")) {
    return base::unexpected("Invalid local pipe endpoint");
  }
  const auto wide = base::UTF8ToWide(endpoint);
  const auto deadline = base::TimeTicks::Now() + kIoTimeout;
  while (base::TimeTicks::Now() < deadline) {
    Connection pipe(::CreateFileW(
        wide.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_ANONYMOUS,
        nullptr));
    if (pipe.is_valid()) {
      DWORD pid = 0;
      if (!::GetNamedPipeServerProcessId(pipe.get(), &pid) ||
          !SameUserProcess(pid)) {
        return base::unexpected("Local pipe server identity does not match");
      }
      return pipe;
    }
    if (::GetLastError() != ERROR_PIPE_BUSY) {
      return base::unexpected("Unable to connect to the primary application");
    }
    ::WaitNamedPipeW(wide.c_str(), 50);
  }
  return base::unexpected("Timed out connecting to the primary application");
}

#else

using Connection = base::ScopedFD;
using NativeConnection = int;
using StopHandle = int;

bool SameUserSocket(int fd) {
#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_BSD)
  uid_t uid;
  gid_t gid;
  return getpeereid(fd, &uid, &gid) == 0 && uid == geteuid();
#else
  struct ucred credentials{};
  socklen_t length = sizeof(credentials);
  return getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &length) == 0 &&
         credentials.uid == geteuid();
#endif
}

bool SocketAddress(const std::string& name, sockaddr_un* address) {
  if (name.empty() || name.size() >= sizeof(address->sun_path) ||
      name.find('\0') != std::string::npos) {
    return false;
  }
  address->sun_family = AF_UNIX;
  base::span(address->sun_path).first(name.size()).copy_from(base::span(name));
  return true;
}

bool WaitSocket(int fd, short events, int stop, base::TimeTicks deadline) {
  while (base::TimeTicks::Now() < deadline) {
    std::array<pollfd, 2> fds = {{{fd, events, 0}, {stop, POLLIN, 0}}};
    const int timeout = static_cast<int>(
        (deadline - base::TimeTicks::Now()).InMillisecondsRoundedUp());
    const int result =
        HANDLE_EINTR(poll(fds.data(), stop >= 0 ? 2 : 1, std::max(timeout, 0)));
    if (result <= 0 || (stop >= 0 && fds[1].revents)) {
      return false;
    }
    if (fds[0].revents & (events | POLLHUP)) {
      return true;
    }
    if (fds[0].revents & (POLLERR | POLLNVAL)) {
      return false;
    }
  }
  return false;
}

bool Transfer(NativeConnection socket,
              base::span<uint8_t> data,
              bool write,
              StopHandle stop,
              base::TimeTicks deadline) {
  while (!data.empty()) {
    if (!WaitSocket(socket, write ? POLLOUT : POLLIN, stop, deadline)) {
      return false;
    }
#if BUILDFLAG(IS_MAC)
    constexpr int kSendFlags = 0;  // SO_NOSIGPIPE is set when creating sockets.
#else
    constexpr int kSendFlags = MSG_NOSIGNAL;
#endif
    const ssize_t bytes =
        write ? HANDLE_EINTR(send(socket, data.data(), data.size(), kSendFlags))
              : HANDLE_EINTR(recv(socket, data.data(), data.size(), 0));
    if (bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      continue;
    }
    if (bytes <= 0) {
      return false;
    }
    data = data.subspan(static_cast<size_t>(bytes));
  }
  return true;
}

Connection CreateSocket() {
  Connection socket(::socket(AF_UNIX, SOCK_STREAM, 0));
  if (!socket.is_valid() || !base::SetNonBlocking(socket.get())) {
    return {};
  }
  base::SetCloseOnExec(socket.get());
#if BUILDFLAG(IS_MAC)
  int enabled = 1;
  if (setsockopt(socket.get(), SOL_SOCKET, SO_NOSIGPIPE, &enabled,
                 sizeof(enabled)) != 0) {
    return {};
  }
#endif
  return socket;
}

base::expected<Connection, std::string> Connect(const std::string& endpoint) {
  if (!base::FilePath(endpoint).IsAbsolute()) {
    return base::unexpected("Invalid local socket endpoint");
  }
  auto socket = CreateSocket();
  sockaddr_un address{};
  if (!socket.is_valid() || !SocketAddress(endpoint, &address)) {
    return base::unexpected("Unable to create local socket");
  }
  if (HANDLE_EINTR(connect(socket.get(), reinterpret_cast<sockaddr*>(&address),
                           sizeof(address))) != 0) {
    if (errno != EINPROGRESS ||
        !WaitSocket(socket.get(), POLLOUT, -1,
                    base::TimeTicks::Now() + kIoTimeout)) {
      return base::unexpected("Unable to connect to the primary application");
    }
    int error = 0;
    socklen_t length = sizeof(error);
    if (getsockopt(socket.get(), SOL_SOCKET, SO_ERROR, &error, &length) != 0 ||
        error != 0) {
      return base::unexpected("Unable to connect to the primary application");
    }
  }
  if (!SameUserSocket(socket.get())) {
    return base::unexpected("Local socket server identity does not match");
  }
  return socket;
}

#endif

class Listener final : public XenonAppSingleInstanceTransport,
                       public base::PlatformThread::Delegate {
 public:
  Listener(std::string lease, MessageCallback callback)
      : lease_(std::move(lease)), callback_(std::move(callback)) {}

  ~Listener() override {
    stopped_.store(true);
#if BUILDFLAG(IS_WIN)
    if (stop_.is_valid()) {
      ::SetEvent(stop_.get());
    }
#else
    if (wake_write_.is_valid()) {
      const char byte = 1;
      const auto ignored = HANDLE_EINTR(write(wake_write_.get(), &byte, 1));
      (void)ignored;
    }
#endif
    if (!thread_.is_null()) {
      base::PlatformThread::Join(thread_);
    }
  }

  const std::string& endpoint() const override { return endpoint_; }

  bool Start() {
#if BUILDFLAG(IS_WIN)
    endpoint_ = "\\\\.\\pipe\\xenon-app-instance-" + lease_;
    auto identity = UserIdentity();
    if (!identity.has_value()) {
      return false;
    }
    const auto sddl = base::UTF8ToWide("D:P(A;;GA;;;" + *identity + ")");
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
      return false;
    }
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), descriptor,
                                   FALSE};
    server_.Set(::CreateNamedPipeW(
        base::UTF8ToWide(endpoint_).c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
            FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_REJECT_REMOTE_CLIENTS, 1,
        4096, 4096, 2000, &attributes));
    ::LocalFree(descriptor);
    stop_.Set(::CreateEvent(nullptr, TRUE, FALSE, nullptr));
    if (!server_.is_valid() || !stop_.is_valid()) {
      return false;
    }
#else
    if (!socket_directory_.CreateUniqueTempDir() ||
        !base::SetPosixFilePermissions(socket_directory_.GetPath(), 0700)) {
      return false;
    }
    endpoint_ = socket_directory_.GetPath().AppendASCII("notify").value();
    server_ = CreateSocket();
    sockaddr_un address{};
    if (!server_.is_valid() || !SocketAddress(endpoint_, &address) ||
        bind(server_.get(), reinterpret_cast<sockaddr*>(&address),
             sizeof(address)) != 0 ||
        listen(server_.get(), 16) != 0) {
      return false;
    }
    int wake[2];
    if (pipe(wake) != 0) {
      return false;
    }
    wake_read_.reset(wake[0]);
    wake_write_.reset(wake[1]);
    base::SetCloseOnExec(wake_read_.get());
    base::SetCloseOnExec(wake_write_.get());
#endif
    return base::PlatformThread::Create(0, this, &thread_);
  }

  void ThreadMain() override {
    base::PlatformThread::SetName("XenonAppSingleton");
    while (!stopped_.load()) {
#if BUILDFLAG(IS_WIN)
      base::win::ScopedHandle event(
          ::CreateEvent(nullptr, TRUE, FALSE, nullptr));
      if (!event.is_valid()) {
        return;
      }
      OVERLAPPED operation{};
      operation.hEvent = event.get();
      DWORD ignored = 0;
      const BOOL connected = ::ConnectNamedPipe(server_.get(), &operation);
      const DWORD error = connected ? ERROR_SUCCESS : ::GetLastError();
      if (!connected && error != ERROR_PIPE_CONNECTED &&
          (error != ERROR_IO_PENDING ||
           !FinishOverlapped(server_.get(), &operation, stop_.get(),
                             base::TimeTicks::Now() + base::Hours(24),
                             &ignored))) {
        if (stopped_.load()) {
          return;
        }
        ::DisconnectNamedPipe(server_.get());
        continue;
      }
      DWORD pid = 0;
      if (::GetNamedPipeClientProcessId(server_.get(), &pid) &&
          SameUserProcess(pid)) {
        HandleMessage(server_.get(), stop_.get());
      }
      ::DisconnectNamedPipe(server_.get());
#else
      if (!WaitSocket(server_.get(), POLLIN, wake_read_.get(),
                      base::TimeTicks::Now() + base::Hours(24))) {
        if (stopped_.load()) {
          return;
        }
        continue;
      }
      Connection client(HANDLE_EINTR(accept(server_.get(), nullptr, nullptr)));
      if (!client.is_valid()) {
        continue;
      }
      base::SetCloseOnExec(client.get());
      if (!base::SetNonBlocking(client.get()) ||
          !SameUserSocket(client.get())) {
        continue;
      }
#if BUILDFLAG(IS_MAC)
      int enabled = 1;
      if (setsockopt(client.get(), SOL_SOCKET, SO_NOSIGPIPE, &enabled,
                     sizeof(enabled)) != 0) {
        continue;
      }
#endif
      HandleMessage(client.get(), wake_read_.get());
#endif
    }
  }

 private:
  void HandleMessage(NativeConnection connection, StopHandle stop) {
    const auto deadline = base::TimeTicks::Now() + kIoTimeout;
    std::array<uint8_t, 4> size{};
    if (!Transfer(connection, size, false, stop, deadline)) {
      return;
    }
    const uint32_t length = base::U32FromBigEndian(size);
    if (length == 0 || length > kMaxMessageBytes) {
      return;
    }
    std::string text(length, '\0');
    if (!Transfer(connection, base::as_writable_byte_span(text), false, stop,
                  deadline) ||
        !ValidMessage(text, lease_) || stopped_.load()) {
      return;
    }
    callback_.Run(std::move(text));
    std::array<uint8_t, 1> ack{1};
    if (Transfer(connection, ack, true, stop, deadline)) {
      // DisconnectNamedPipe discards unread bytes. Wait for a bounded receipt
      // instead of FlushFileBuffers, which could block forever on a bad client.
      std::array<uint8_t, 1> receipt{};
      Transfer(connection, receipt, false, stop, deadline);
    }
  }

  const std::string lease_;
  const MessageCallback callback_;
  std::string endpoint_;
  Connection server_;
#if BUILDFLAG(IS_WIN)
  base::win::ScopedHandle stop_;
#else
  base::ScopedTempDir socket_directory_;
  base::ScopedFD wake_read_;
  base::ScopedFD wake_write_;
#endif
  std::atomic<bool> stopped_{false};
  base::PlatformThreadHandle thread_;
};

}  // namespace

base::expected<std::unique_ptr<XenonAppSingleInstanceTransport>, std::string>
XenonAppSingleInstanceTransport::Listen(const std::string& lease,
                                        MessageCallback callback) {
  auto listener = std::make_unique<Listener>(lease, std::move(callback));
  if (!listener->Start()) {
    return base::unexpected(
        "Unable to start the application's local IPC server");
  }
  return std::unique_ptr<XenonAppSingleInstanceTransport>(std::move(listener));
}

base::expected<void, std::string> XenonAppSingleInstanceTransport::Notify(
    const std::string& endpoint,
    std::string_view message) {
  if (message.empty() || message.size() > kMaxMessageBytes) {
    return base::unexpected("Single-instance message exceeds the size limit");
  }
  auto connection = Connect(endpoint);
  if (!connection.has_value()) {
    return base::unexpected(std::move(connection.error()));
  }
#if BUILDFLAG(IS_WIN)
  constexpr StopHandle kNoStop = nullptr;
#else
  constexpr StopHandle kNoStop = -1;
#endif
  const auto deadline = base::TimeTicks::Now() + kIoTimeout;
  auto size = base::U32ToBigEndian(static_cast<uint32_t>(message.size()));
  std::string payload(message);
  std::array<uint8_t, 1> ack{};
  if (!Transfer(connection->get(), size, true, kNoStop, deadline) ||
      !Transfer(connection->get(), base::as_writable_byte_span(payload), true,
                kNoStop, deadline) ||
      !Transfer(connection->get(), ack, false, kNoStop, deadline) ||
      ack[0] != 1) {
    return base::unexpected(
        "Primary application did not acknowledge the request");
  }
  // Receipt is best-effort after acknowledgement. Retrying an acknowledged
  // request would deliver second-instance twice if the owner exits here.
  Transfer(connection->get(), ack, true, kNoStop, deadline);
  return {};
}

base::expected<std::string, std::string>
XenonAppSingleInstanceTransport::UserIdentity() {
#if BUILDFLAG(IS_WIN)
  auto token = base::win::AccessToken::FromCurrentProcess();
  if (!token) {
    return base::unexpected("Unable to identify the current user");
  }
  auto sid = token->User().ToSddlString();
  if (!sid) {
    return base::unexpected("Unable to identify the current user");
  }
  return base::ok(base::WideToUTF8(*sid));
#else
  return base::ok(base::NumberToString(geteuid()));
#endif
}

}  // namespace xenon::ipc
