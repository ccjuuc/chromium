// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_POWER_SAVE_BLOCKER_H_
#define XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_POWER_SAVE_BLOCKER_H_

#include <map>
#include <string>

#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/types/expected.h"
#include "base/values.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "services/device/public/mojom/wake_lock.mojom.h"

namespace xenon::ipc {

// Browser UI sequence only. Owners are authenticated container identities that
// include their generation; they must never be taken from JavaScript arguments.
// A successful start means a system wake-lock request has been submitted, as in
// Electron. OS power policy can still override the request.
class ElectronPowerSaveBlocker {
 public:
  using WakeLockBinder = base::RepeatingCallback<void(
      device::mojom::WakeLockType,
      mojo::PendingReceiver<device::mojom::WakeLock>)>;

  // Production binds through content::GetDeviceService().BindWakeLockProvider;
  // injection keeps broker tests independent of the Browser process lifetime.
  explicit ElectronPowerSaveBlocker(WakeLockBinder binder);
  ~ElectronPowerSaveBlocker();
  ElectronPowerSaveBlocker(const ElectronPowerSaveBlocker&) = delete;
  ElectronPowerSaveBlocker& operator=(const ElectronPowerSaveBlocker&) = delete;

  base::expected<base::Value, std::string> Call(const std::string& owner,
                                                const base::Value& request);
  void ReleaseOwner(const std::string& owner);

 private:
  struct OwnerState {
    OwnerState();
    ~OwnerState();
    std::map<int, device::mojom::WakeLockType> requests;
    mojo::Remote<device::mojom::WakeLock> wake_lock;
    device::mojom::WakeLockType effective_type =
        device::mojom::WakeLockType::kPreventAppSuspension;
  };

  void Update(const std::string& owner, OwnerState& state);

  WakeLockBinder binder_;
  std::map<std::string, OwnerState> owners_;
  int next_id_ = 0;
  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<ElectronPowerSaveBlocker> weak_factory_{this};
};

}  // namespace xenon::ipc

#endif  // XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_POWER_SAVE_BLOCKER_H_
