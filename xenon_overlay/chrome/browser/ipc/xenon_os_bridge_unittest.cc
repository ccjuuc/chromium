// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_os_bridge.h"

#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/strings/string_view_util.h"
#include "build/build_config.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "uv.h"

namespace xenon::ipc {
namespace {

mojom::IpcResultPtr Call(const char* method) {
  return PerformOsCall(base::Value(base::DictValue().Set("method", method)));
}

TEST(XenonOsBridgeTest, UnameFieldsComeFromTheOperatingSystem) {
  uv_utsname_t native = {};
  ASSERT_EQ(0, uv_os_uname(&native));
  for (const auto& [method, expected] :
       {std::pair{"type", native.sysname},
        std::pair{"release", native.release},
        std::pair{"version", native.version},
        std::pair{"machine", native.machine}}) {
    SCOPED_TRACE(method);
    auto result = Call(method);
    ASSERT_TRUE(result->success);
    ASSERT_TRUE(result->value.is_string());
    EXPECT_TRUE(result->value.GetString() == expected);
  }
}

TEST(XenonOsBridgeTest, HostAndDirectoriesComeFromTheOperatingSystem) {
  for (const auto& [method, read] :
       {std::pair{"hostname", &uv_os_gethostname},
        std::pair{"homedir", &uv_os_homedir},
        std::pair{"tmpdir", &uv_os_tmpdir}}) {
    SCOPED_TRACE(method);
    std::vector<char> native(256);
    size_t length = native.size();
    int status = read(native.data(), &length);
    if (status == UV_ENOBUFS) {
      ASSERT_GT(length, native.size());
      ASSERT_LE(length, 16u * 1024u * 1024u);
      native.resize(length);
      status = read(native.data(), &length);
    }
    ASSERT_EQ(0, status);
    ASSERT_LE(length, native.size());
    auto result = Call(method);
    ASSERT_TRUE(result->success);
    ASSERT_TRUE(result->value.is_string());
    // Boolean comparisons intentionally keep machine/user identifiers out of
    // test logs, including assertion failures.
    EXPECT_TRUE(result->value.GetString() ==
                base::as_string_view(base::span(native).first(length)));
  }
}

TEST(XenonOsBridgeTest, UserInfoMatchesNativeIdentityWithoutLoggingIt) {
  uv_passwd_t native = {};
  ASSERT_EQ(0, uv_os_get_passwd(&native));
  base::ScopedClosureRunner cleanup(
      base::BindOnce(&uv_os_free_passwd, &native));
  auto result = Call("userInfo");
  ASSERT_TRUE(result->success);
  ASSERT_TRUE(result->value.is_dict());
  const auto& user = result->value.GetDict();
  const auto* username = user.FindString("username");
  const auto* homedir = user.FindString("homedir");
  ASSERT_TRUE(username);
  ASSERT_TRUE(homedir);
  EXPECT_TRUE(*username == native.username);
  EXPECT_TRUE(*homedir == native.homedir);
  EXPECT_TRUE(user.FindDouble("uid") == static_cast<double>(native.uid));
  EXPECT_TRUE(user.FindDouble("gid") == static_cast<double>(native.gid));
  const auto* shell = user.Find("shell");
  ASSERT_TRUE(shell);
  if (native.shell) {
    ASSERT_TRUE(shell->is_string());
    EXPECT_TRUE(shell->GetString() == native.shell);
  } else {
    EXPECT_TRUE(shell->is_none());
  }
}

TEST(XenonOsBridgeTest, CpuSnapshotHasRealModelsAndNativeMillisecondCounters) {
  uv_cpu_info_t* raw_cpus = nullptr;
  int count = 0;
  ASSERT_EQ(0, uv_cpu_info(&raw_cpus, &count));
  base::ScopedClosureRunner cleanup(
      base::BindOnce(&uv_free_cpu_info, raw_cpus, count));
  auto cpus = UNSAFE_BUFFERS(base::span(raw_cpus, static_cast<size_t>(count)));
  auto result = Call("cpus");
  ASSERT_TRUE(result->success);
  ASSERT_TRUE(result->value.is_list());
  const auto& values = result->value.GetList();
  ASSERT_EQ(cpus.size(), values.size());
  for (size_t i = 0; i < cpus.size(); ++i) {
    ASSERT_TRUE(values[i].is_dict());
    const auto& cpu = values[i].GetDict();
    const auto* model = cpu.FindString("model");
    ASSERT_TRUE(model);
    EXPECT_TRUE(*model == cpus[i].model);
    EXPECT_GE(cpu.FindInt("speed").value_or(-1), 0);
    const auto* times = cpu.FindDict("times");
    ASSERT_TRUE(times);
    for (const auto* key : {"user", "nice", "sys", "idle", "irq"}) {
      auto value = times->FindDouble(key);
      ASSERT_TRUE(value.has_value());
      EXPECT_TRUE(std::isfinite(*value));
      EXPECT_GE(*value, 0);
    }
    // Snapshot B is read after A, so accumulated counters cannot be the old
    // all-zero placeholder on an operating system that reports CPU activity.
    EXPECT_GE(*times->FindDouble("user"),
              static_cast<double>(cpus[i].cpu_times.user));
    EXPECT_GE(*times->FindDouble("idle"),
              static_cast<double>(cpus[i].cpu_times.idle));
  }
}

TEST(XenonOsBridgeTest, MemoryUptimeAndLoadUseNativeUnits) {
  auto total = Call("totalmem");
  ASSERT_TRUE(total->success);
  ASSERT_TRUE(total->value.is_double());
  EXPECT_EQ(static_cast<double>(uv_get_total_memory()),
            total->value.GetDouble());
  auto free = Call("freemem");
  ASSERT_TRUE(free->success);
  ASSERT_TRUE(free->value.is_double());
  EXPECT_GE(free->value.GetDouble(), 0);
  EXPECT_LE(free->value.GetDouble(), total->value.GetDouble());

  double before = 0;
  double after = 0;
  ASSERT_EQ(0, uv_uptime(&before));
  auto uptime = Call("uptime");
  ASSERT_EQ(0, uv_uptime(&after));
  ASSERT_TRUE(uptime->success);
  ASSERT_TRUE(uptime->value.is_double());
  EXPECT_GE(uptime->value.GetDouble(), before);
  EXPECT_LE(uptime->value.GetDouble(), after);

  auto load = Call("loadavg");
  ASSERT_TRUE(load->success);
  ASSERT_TRUE(load->value.is_list());
  ASSERT_EQ(3u, load->value.GetList().size());
  for (const auto& average : load->value.GetList()) {
    ASSERT_TRUE(average.is_double());
    EXPECT_TRUE(std::isfinite(average.GetDouble()));
    EXPECT_GE(average.GetDouble(), 0);
#if BUILDFLAG(IS_WIN)
    EXPECT_EQ(0, average.GetDouble());
#endif
  }
}

TEST(XenonOsBridgeTest, MetadataMatchesTheCompileTarget) {
  auto platform = Call("platform");
  auto arch = Call("arch");
  auto endian = Call("endianness");
  ASSERT_TRUE(platform->success);
  ASSERT_TRUE(arch->success);
  ASSERT_TRUE(endian->success);
#if BUILDFLAG(IS_WIN)
  EXPECT_EQ("win32", platform->value.GetString());
#elif BUILDFLAG(IS_APPLE)
  EXPECT_EQ("darwin", platform->value.GetString());
#elif BUILDFLAG(IS_ANDROID)
  EXPECT_EQ("android", platform->value.GetString());
#elif BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_CHROMEOS)
  EXPECT_EQ("linux", platform->value.GetString());
#endif
#if defined(ARCH_CPU_X86_64)
  EXPECT_EQ("x64", arch->value.GetString());
#elif defined(ARCH_CPU_ARM64)
  EXPECT_EQ("arm64", arch->value.GetString());
#elif defined(ARCH_CPU_X86)
  EXPECT_EQ("ia32", arch->value.GetString());
#endif
#if defined(ARCH_CPU_LITTLE_ENDIAN)
  EXPECT_EQ("LE", endian->value.GetString());
#else
  EXPECT_EQ("BE", endian->value.GetString());
#endif
}

TEST(XenonOsBridgeTest, InvalidAndUnsupportedCallsFailExplicitly) {
  std::vector<base::Value> requests;
  requests.emplace_back();
  requests.emplace_back(42);
  requests.emplace_back(base::ListValue());
  requests.emplace_back(base::DictValue());
  requests.emplace_back(base::DictValue().Set("method", 42));
  for (auto& request : requests) {
    auto result = PerformOsCall(std::move(request));
    EXPECT_FALSE(result->success);
    EXPECT_TRUE(result->error.starts_with("ERR_INVALID_ARG_TYPE:"));
  }
  for (const auto* method :
       {"made-up", "availableParallelism", "getPriority", "setPriority"}) {
    auto result = Call(method);
    EXPECT_FALSE(result->success);
    EXPECT_TRUE(result->error.starts_with("ERR_NOT_SUPPORTED:"));
  }
}

}  // namespace
}  // namespace xenon::ipc
