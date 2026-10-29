// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "services/gm_key/compiler_compat.h"
#include "services/gm_key/skf_driver.h"

// Standalone, explicit diagnostic process. Never accepts a PIN on the command
// line, never signs, and prints only counts (no device names or certificates).
int wmain(int argc, wchar_t** argv) {
  if (argc != 2) {
    std::fprintf(stderr,
                 "usage: gm_skf_probe.exe <absolute signed AMD64 SKF DLL>\n");
    return 2;
  }
  const auto check = [](gm_key::Status status) {
    if (!status.ok()) {
      std::fprintf(stderr, "SKF error=%u vendor=0x%08x system=0x%08x\n",
                   static_cast<unsigned>(status.error), status.skf_error,
                   status.system_error);
      return false;
    }
    return true;
  };
  std::unique_ptr<gm_key::SkfDriver> driver;
  // SAFETY: The CRT provides argc NUL-terminated arguments; argc == 2 above.
  const std::wstring path = GM_KEY_UNSAFE_BUFFERS(argv[1]);
  if (!check(gm_key::SkfDriver::Load(path, &driver))) {
    return 1;
  }
  std::vector<std::string> devices;
  if (!check(driver->EnumerateDevices(&devices))) {
    return 1;
  }
  size_t app_count = 0;
  size_t container_count = 0;
  for (const auto& device : devices) {
    std::vector<std::string> apps;
    if (!check(driver->EnumerateApplications(device, &apps))) {
      return 1;
    }
    app_count += apps.size();
    for (const auto& app : apps) {
      std::vector<std::string> containers;
      if (!check(driver->EnumerateContainers(device, app, &containers))) {
        return 1;
      }
      container_count += containers.size();
    }
  }
  std::printf("present_devices=%zu applications=%zu containers=%zu\n",
              devices.size(), app_count, container_count);
  return 0;
}
