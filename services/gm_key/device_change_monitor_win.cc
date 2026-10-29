// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/gm_key/device_change_monitor_win.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/task/single_thread_task_runner.h"

namespace gm_key {

DeviceChangeMonitor::DeviceChangeMonitor(
    std::shared_ptr<std::atomic<uint64_t>> generation,
    base::RepeatingClosure changed)
    : generation_(std::move(generation)),
      task_runner_(base::SingleThreadTaskRunner::GetCurrentDefault()),
      changed_(std::move(changed)) {}

DeviceChangeMonitor::~DeviceChangeMonitor() {
  if (notification_) {
    // Called on the service sequence, never inside the OS callback. Windows
    // waits for an in-flight callback before this object's storage is freed.
    CM_Unregister_Notification(notification_);
  }
}

bool DeviceChangeMonitor::Start() {
  // GUID_DEVINTERFACE_USB_DEVICE, including vendor, HID and CCID USBKeys. SKF
  // names have no standard mapping to a PnP interface, so conservatively revoke
  // this configured Key service on any physical USB device removal/arrival.
  constexpr GUID kUsbDevice = {
      0xa5dcbf10,
      0x6530,
      0x11d2,
      {0x90, 0x1f, 0x00, 0xc0, 0x4f, 0xb9, 0x51, 0xed}};
  CM_NOTIFY_FILTER filter = {};
  filter.cbSize = sizeof(filter);
  filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
  filter.u.DeviceInterface.ClassGuid = kUsbDevice;
  return CM_Register_Notification(&filter, this, &OnNotification,
                                  &notification_) == CR_SUCCESS;
}

DWORD CALLBACK DeviceChangeMonitor::OnNotification(HCMNOTIFICATION,
                                                   void* context,
                                                   CM_NOTIFY_ACTION action,
                                                   PCM_NOTIFY_EVENT_DATA,
                                                   DWORD) {
  if (action == CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL ||
      action == CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL) {
    auto* monitor = static_cast<DeviceChangeMonitor*>(context);
    monitor->generation_->fetch_add(1);
    monitor->task_runner_->PostTask(FROM_HERE,
                                    base::BindOnce(monitor->changed_));
  }
  return ERROR_SUCCESS;
}

}  // namespace gm_key
