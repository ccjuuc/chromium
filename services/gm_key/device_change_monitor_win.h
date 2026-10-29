// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SERVICES_GM_KEY_DEVICE_CHANGE_MONITOR_WIN_H_
#define SERVICES_GM_KEY_DEVICE_CHANGE_MONITOR_WIN_H_

#include <windows.h>

#include <cfgmgr32.h>

#include <atomic>
#include <cstdint>
#include <memory>

#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"

namespace base {
class SingleThreadTaskRunner;
}

namespace gm_key {

// PnP notifications retain each removal/arrival even when the next SKF device
// enumeration returns the same names. The OS callback never enters the SDK or
// waits for its worker; it advances the generation before posting to Mojo.
class DeviceChangeMonitor {
 public:
  DeviceChangeMonitor(std::shared_ptr<std::atomic<uint64_t>> generation,
                      base::RepeatingClosure changed);
  ~DeviceChangeMonitor();
  DeviceChangeMonitor(const DeviceChangeMonitor&) = delete;
  DeviceChangeMonitor& operator=(const DeviceChangeMonitor&) = delete;

  bool Start();

 private:
  static DWORD CALLBACK OnNotification(HCMNOTIFICATION notification,
                                       void* context,
                                       CM_NOTIFY_ACTION action,
                                       PCM_NOTIFY_EVENT_DATA data,
                                       DWORD data_size);

  const std::shared_ptr<std::atomic<uint64_t>> generation_;
  const scoped_refptr<base::SingleThreadTaskRunner> task_runner_;
  const base::RepeatingClosure changed_;
  HCMNOTIFICATION notification_ = nullptr;
};

}  // namespace gm_key

#endif  // SERVICES_GM_KEY_DEVICE_CHANGE_MONITOR_WIN_H_
