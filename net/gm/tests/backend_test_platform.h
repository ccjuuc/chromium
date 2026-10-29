// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_GM_TESTS_BACKEND_TEST_PLATFORM_H_
#define NET_GM_TESTS_BACKEND_TEST_PLATFORM_H_

// OS primitives for the independent backend tests. This header must not expose
// either crypto library: coexistence_test links only Chromium's BoringSSL.
#if defined(_WIN32)
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <dlfcn.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <filesystem>
#include <type_traits>

namespace gm_test {

// Independent test executables call the backend's dynamic C ABI. Retain
// Windows CFG and narrowly exempt this call on platforms without cross-DSO CFI.
template <typename Result, typename... Args>
#if defined(__clang__) && !defined(_WIN32)
[[clang::no_sanitize("cfi-icall"), clang::noinline]]
#endif
Result CallModuleFunction(Result (*function)(Args...),
                          std::type_identity_t<Args>... args) {
  return function(args...);
}

#if defined(_WIN32)
using Socket = SOCKET;
using SocketLength = int;
using Module = HMODULE;
inline constexpr Socket kInvalidSocket = INVALID_SOCKET;
#else
using Socket = int;
using SocketLength = socklen_t;
using Module = void*;
inline constexpr Socket kInvalidSocket = -1;
#endif

inline bool InitializeSockets() {
#if defined(_WIN32)
  WSADATA data;
  return WSAStartup(MAKEWORD(2, 2), &data) == 0;
#else
  // Tests intentionally close peers mid-handshake; retain the network error
  // instead of allowing a process-wide SIGPIPE to hide the test assertion.
  return signal(SIGPIPE, SIG_IGN) != SIG_ERR;
#endif
}

inline void CleanupSockets() {
#if defined(_WIN32)
  WSACleanup();
#endif
}

inline void CloseSocket(Socket socket) {
#if defined(_WIN32)
  closesocket(socket);
#else
  close(socket);
#endif
}

inline void SetSocketTimeout(Socket socket, unsigned milliseconds) {
#if defined(_WIN32)
  DWORD timeout = milliseconds;
#else
  timeval timeout = {static_cast<time_t>(milliseconds / 1000),
                     static_cast<suseconds_t>((milliseconds % 1000) * 1000)};
#endif
  setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
             reinterpret_cast<const char*>(&timeout), sizeof(timeout));
  setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO,
             reinterpret_cast<const char*>(&timeout), sizeof(timeout));
}

inline bool SetExclusiveListener(Socket socket) {
#if defined(_WIN32)
  BOOL exclusive = TRUE;
  return setsockopt(socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                    reinterpret_cast<const char*>(&exclusive),
                    sizeof(exclusive)) == 0;
#else
  // No SO_REUSEADDR/SO_REUSEPORT: normal POSIX bind ownership is exclusive.
  return socket != kInvalidSocket;
#endif
}

inline int WaitReadable(Socket socket, fd_set* sockets, timeval* wait) {
#if defined(_WIN32)
  return select(0, sockets, nullptr, nullptr, wait);
#else
  return select(socket + 1, sockets, nullptr, nullptr, wait);
#endif
}

inline Module LoadModule(const std::filesystem::path& path) {
  if (!path.is_absolute()) {
    return nullptr;
  }
#if defined(_WIN32)
  return LoadLibraryExW(path.c_str(), nullptr,
                       LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                           LOAD_LIBRARY_SEARCH_SYSTEM32);
#else
  return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

inline void* FindFunction(Module module, const char* name) {
#if defined(_WIN32)
  return reinterpret_cast<void*>(GetProcAddress(module, name));
#else
  return dlsym(module, name);
#endif
}

inline void CloseModule(Module module) {
#if defined(_WIN32)
  FreeLibrary(module);
#else
  dlclose(module);
#endif
}

}  // namespace gm_test

#endif  // NET_GM_TESTS_BACKEND_TEST_PLATFORM_H_
