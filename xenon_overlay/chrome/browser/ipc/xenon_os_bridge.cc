// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_os_bridge.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/string_view_util.h"
#include "uv.h"
#include "xenon_overlay/common/ipc/xenon_runtime_platform.h"

namespace xenon::ipc {
namespace {

std::optional<int> PrefixLength(base::span<const uint8_t> mask) {
  int prefix = 0;
  bool seen_zero = false;
  for (uint8_t byte : mask) {
    for (int bit = 7; bit >= 0; --bit) {
      if (byte & (1 << bit)) {
        if (seen_zero) {
          return std::nullopt;
        }
        ++prefix;
      } else {
        seen_zero = true;
      }
    }
  }
  return prefix;
}

mojom::IpcResultPtr Failure(int status) {
  auto result = mojom::IpcResult::New();
  result->success = false;
  result->error = std::string(uv_err_name(status)) + ": " +
                  uv_strerror(status) + ", uv_interface_addresses";
  return result;
}

mojom::IpcResultPtr OsFailure(std::string error) {
  auto result = mojom::IpcResult::New();
  result->success = false;
  result->error = std::move(error);
  return result;
}

mojom::IpcResultPtr OsFailure(int status, std::string_view operation) {
  return OsFailure(std::string(uv_err_name(status)) + ": " +
                   uv_strerror(status) + ", " + std::string(operation));
}

mojom::IpcResultPtr OsValue(base::Value value) {
  auto result = mojom::IpcResult::New();
  result->success = true;
  result->value = std::move(value);
  return result;
}

mojom::IpcResultPtr ReadOsString(int (*read)(char*, size_t*),
                               std::string_view operation) {
  std::vector<char> buffer(256);
  for (;;) {
    size_t length = buffer.size();
    int status = read(buffer.data(), &length);
    if (status == UV_ENOBUFS) {
      // libuv reports the required size, including the terminating NUL. Bound
      // unexpected results instead of retrying forever or allocating blindly.
      if (length <= buffer.size() || length > 16 * 1024 * 1024) {
        return OsFailure(status, operation);
      }
      buffer.resize(length);
      continue;
    }
    if (status != 0) {
      return OsFailure(status, operation);
    }
    if (length > buffer.size()) {
      return OsFailure(UV_ENOBUFS, operation);
    }
    return OsValue(base::Value(
        base::as_string_view(base::span(buffer).first(length))));
  }
}

mojom::IpcResultPtr ReadCpuInfo() {
  uv_cpu_info_t* raw_cpus = nullptr;
  int count = 0;
  int status = uv_cpu_info(&raw_cpus, &count);
  if (status != 0) {
    return OsFailure(status, "uv_cpu_info");
  }
  base::ScopedClosureRunner cleanup(
      base::BindOnce(&uv_free_cpu_info, raw_cpus, count));
  // libuv owns exactly |count| entries until uv_free_cpu_info.
  auto cpus = UNSAFE_BUFFERS(base::span(raw_cpus, static_cast<size_t>(count)));
  base::ListValue result;
  for (const auto& cpu : cpus) {
    base::DictValue times;
    times.Set("user", static_cast<double>(cpu.cpu_times.user));
    times.Set("nice", static_cast<double>(cpu.cpu_times.nice));
    times.Set("sys", static_cast<double>(cpu.cpu_times.sys));
    times.Set("idle", static_cast<double>(cpu.cpu_times.idle));
    times.Set("irq", static_cast<double>(cpu.cpu_times.irq));
    result.Append(base::DictValue()
                      .Set("model", cpu.model)
                      .Set("speed", cpu.speed)
                      .Set("times", std::move(times)));
  }
  return OsValue(base::Value(std::move(result)));
}

mojom::IpcResultPtr ReadUserInfo() {
  uv_passwd_t user = {};
  int status = uv_os_get_passwd(&user);
  if (status != 0) {
    return OsFailure(status, "uv_os_get_passwd");
  }
  base::ScopedClosureRunner cleanup(
      base::BindOnce(&uv_os_free_passwd, &user));
  base::DictValue value;
  value.Set("username", user.username);
  value.Set("uid", static_cast<double>(user.uid));
  value.Set("gid", static_cast<double>(user.gid));
  value.Set("shell", user.shell ? base::Value(user.shell) : base::Value());
  value.Set("homedir", user.homedir);
  return OsValue(base::Value(std::move(value)));
}

}  // namespace

mojom::IpcResultPtr PerformOsCall(base::Value request) {
  const auto* arguments = request.GetIfDict();
  const auto* method = arguments ? arguments->FindString("method") : nullptr;
  if (!method) {
    return OsFailure("ERR_INVALID_ARG_TYPE: os request.method must be a string");
  }
  if (*method == "networkInterfaces") {
    return GetNetworkInterfaces();
  }
  if (*method == "type" || *method == "release" || *method == "version" ||
      *method == "machine") {
    uv_utsname_t name = {};
    int status = uv_os_uname(&name);
    if (status != 0) {
      return OsFailure(status, "uv_os_uname");
    }
    if (*method == "type") {
      return OsValue(base::Value(name.sysname));
    }
    if (*method == "release") {
      return OsValue(base::Value(name.release));
    }
    if (*method == "version") {
      return OsValue(base::Value(name.version));
    }
    return OsValue(base::Value(name.machine));
  }
  if (*method == "hostname") {
    return ReadOsString(&uv_os_gethostname, "uv_os_gethostname");
  }
  if (*method == "homedir") {
    return ReadOsString(&uv_os_homedir, "uv_os_homedir");
  }
  if (*method == "tmpdir") {
    return ReadOsString(&uv_os_tmpdir, "uv_os_tmpdir");
  }
  if (*method == "userInfo") {
    return ReadUserInfo();
  }
  if (*method == "cpus") {
    return ReadCpuInfo();
  }
  if (*method == "totalmem") {
    return OsValue(base::Value(static_cast<double>(uv_get_total_memory())));
  }
  if (*method == "freemem") {
    return OsValue(base::Value(static_cast<double>(uv_get_free_memory())));
  }
  if (*method == "uptime") {
    double seconds = 0;
    int status = uv_uptime(&seconds);
    return status == 0 ? OsValue(base::Value(seconds))
                       : OsFailure(status, "uv_uptime");
  }
  if (*method == "loadavg") {
    // libuv returns the documented zeros on Windows, which has no load-average
    // facility, and real 1/5/15-minute averages on supported Unix platforms.
    double averages[3] = {};
    uv_loadavg(averages);
    base::ListValue result;
    for (double average : averages) {
      result.Append(average);
    }
    return OsValue(base::Value(std::move(result)));
  }
  if (*method == "endianness") {
    return OsValue(base::Value(EndiannessName()));
  }
  if (*method == "platform" || *method == "arch") {
    const char* value =
        *method == "platform" ? PlatformName() : ArchitectureName();
    return *value ? OsValue(base::Value(value))
                  : OsFailure("ERR_NOT_SUPPORTED: unknown Node target metadata");
  }
  if (*method == "availableParallelism") {
    // Bundled libuv 1.43 predates uv_available_parallelism. CPU enumeration is
    // not equivalent to the CPUs available under a process's affinity mask.
    return OsFailure("ERR_NOT_SUPPORTED: os.availableParallelism requires "
                     "process-aware native support");
  }
  if (*method == "getPriority" || *method == "setPriority") {
    // pid=0 in libuv means this browser process, not the JS caller's process.
    // Reject until IPC supplies a trusted caller identity rather than silently
    // reading or changing the wrong process's priority.
    return OsFailure("ERR_NOT_SUPPORTED: os." + *method +
                     " requires a trusted caller process identity");
  }
  return OsFailure("ERR_NOT_SUPPORTED: unsupported os method");
}

mojom::IpcResultPtr GetNetworkInterfaces() {
  uv_interface_address_t* raw_addresses = nullptr;
  int count = 0;
  int status = uv_interface_addresses(&raw_addresses, &count);
  if (status != 0) {
    return Failure(status);
  }
  base::ScopedClosureRunner cleanup(
      base::BindOnce(&uv_free_interface_addresses, raw_addresses, count));
  // libuv owns exactly |count| entries until uv_free_interface_addresses.
  auto addresses =
      UNSAFE_BUFFERS(base::span(raw_addresses, static_cast<size_t>(count)));
  base::DictValue interfaces;
  for (const auto& address : addresses) {
    const bool ipv6 = address.address.address4.sin_family == AF_INET6;
    if (!ipv6 && address.address.address4.sin_family != AF_INET) {
      continue;
    }
    char ip[INET6_ADDRSTRLEN] = {};
    char netmask[INET6_ADDRSTRLEN] = {};
    status = ipv6 ? uv_ip6_name(&address.address.address6, ip, sizeof(ip))
                  : uv_ip4_name(&address.address.address4, ip, sizeof(ip));
    if (status != 0) {
      return Failure(status);
    }
    status =
        ipv6 ? uv_ip6_name(&address.netmask.netmask6, netmask, sizeof(netmask))
             : uv_ip4_name(&address.netmask.netmask4, netmask, sizeof(netmask));
    if (status != 0) {
      return Failure(status);
    }
    const auto prefix = ipv6 ? PrefixLength(base::as_byte_span(
                                   address.netmask.netmask6.sin6_addr.s6_addr))
                             : PrefixLength(base::byte_span_from_ref(
                                   address.netmask.netmask4.sin_addr.s_addr));
    std::string mac;
    for (unsigned char byte : address.phys_addr) {
      if (!mac.empty()) {
        mac += ':';
      }
      mac +=
          base::ToLowerASCII(base::HexEncode(base::byte_span_from_ref(byte)));
    }
    base::DictValue entry;
    entry.Set("address", ip);
    entry.Set("netmask", netmask);
    entry.Set("family", ipv6 ? "IPv6" : "IPv4");
    entry.Set("mac", mac);
    entry.Set("internal", address.is_internal != 0);
    entry.Set("cidr", prefix ? base::Value(std::string(ip) + "/" +
                                           base::NumberToString(*prefix))
                             : base::Value());
    if (ipv6) {
      entry.Set("scopeid",
                static_cast<double>(address.address.address6.sin6_scope_id));
    }
    base::ListValue* entries = interfaces.FindList(address.name);
    if (!entries) {
      entries = &interfaces.Set(address.name, base::ListValue())->GetList();
    }
    entries->Append(std::move(entry));
  }
  auto result = mojom::IpcResult::New();
  result->success = true;
  result->value = base::Value(std::move(interfaces));
  return result;
}

}  // namespace xenon::ipc
