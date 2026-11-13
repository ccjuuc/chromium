// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_electron_power_save_blocker.h"

#include <limits>
#include <utility>

#include "base/functional/bind.h"
#include "build/build_config.h"
#include "build/config/linux/dbus/buildflags.h"

namespace xenon::ipc {
ElectronPowerSaveBlocker::OwnerState::OwnerState() = default;
ElectronPowerSaveBlocker::OwnerState::~OwnerState() = default;

ElectronPowerSaveBlocker::ElectronPowerSaveBlocker(WakeLockBinder binder)
    : binder_(std::move(binder)) {}

ElectronPowerSaveBlocker::~ElectronPowerSaveBlocker() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // Closing each Mojo endpoint releases its native wake lock even during exit.
}

base::expected<base::Value, std::string> ElectronPowerSaveBlocker::Call(
    const std::string& owner,
    const base::Value& request) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (owner.empty()) {
    return base::unexpected(
        "ERR_ACCESS_DENIED: missing powerSaveBlocker owner");
  }
  const auto* args = request.GetIfDict();
  const auto* method = args ? args->FindString("method") : nullptr;
  if (!method) {
    return base::unexpected(
        "ERR_INVALID_ARG_TYPE: missing powerSaveBlocker method");
  }
  if (*method == "start") {
#if !BUILDFLAG(IS_WIN) && !BUILDFLAG(IS_MAC) && !BUILDFLAG(IS_CHROMEOS) && \
    !(BUILDFLAG(IS_LINUX) && BUILDFLAG(USE_DBUS))
    return base::unexpected(
        "ERR_NOT_SUPPORTED: powerSaveBlocker requires a desktop platform");
#else
    const auto* type = args->FindString("type");
    if (!type || (*type != "prevent-app-suspension" &&
                  *type != "prevent-display-sleep")) {
      return base::unexpected(
          "ERR_INVALID_ARG_VALUE: invalid powerSaveBlocker type");
    }
    if (!binder_ || next_id_ == std::numeric_limits<int>::max()) {
      return base::unexpected(
          "ERR_RESOURCE_UNAVAILABLE: cannot create powerSaveBlocker");
    }
    const int id = next_id_++;
    auto& state = owners_[owner];
    state.requests.emplace(
        id, *type == "prevent-display-sleep"
                ? device::mojom::WakeLockType::kPreventDisplaySleep
                : device::mojom::WakeLockType::kPreventAppSuspension);
    Update(owner, state);
    return base::Value(id);
#endif
  }
  if (*method != "stop" && *method != "isStarted") {
    return base::unexpected(
        "ERR_NOT_SUPPORTED: unknown powerSaveBlocker method");
  }
  const auto id = args->FindInt("id");
  if (!id) {
    return base::unexpected(
        "ERR_INVALID_ARG_TYPE: powerSaveBlocker id must be an integer");
  }
  const auto it = owners_.find(owner);
  if (it == owners_.end()) {
    return base::Value(false);
  }
  auto& state = it->second;
  if (*method == "isStarted") {
    return base::Value(state.requests.contains(*id));
  }
  const bool removed = state.requests.erase(*id) != 0;
  if (state.requests.empty()) {
    ReleaseOwner(owner);
  } else if (removed) {
    Update(owner, state);
  }
  return base::Value(removed);
}

void ElectronPowerSaveBlocker::ReleaseOwner(const std::string& owner) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  const auto it = owners_.find(owner);
  if (it != owners_.end()) {
    if (it->second.wake_lock) {
      it->second.wake_lock->CancelWakeLock();
    }
    owners_.erase(it);
  }
}

void ElectronPowerSaveBlocker::Update(const std::string& owner,
                                      OwnerState& state) {
  auto type = device::mojom::WakeLockType::kPreventAppSuspension;
  for (const auto& [id, requested_type] : state.requests) {
    if (requested_type == device::mojom::WakeLockType::kPreventDisplaySleep) {
      type = requested_type;
      break;
    }
  }
  if (state.wake_lock && state.effective_type == type) {
    return;
  }
  // WakeLockProvider stores context-free endpoints under their creation type.
  // Changing that type leaves its disconnect bookkeeping in the wrong bucket.
  // Submit the replacement before cancelling the old request, and let each
  // endpoint retain the type under which the provider owns it.
  auto previous = std::move(state.wake_lock);
  binder_.Run(type, state.wake_lock.BindNewPipeAndPassReceiver());
  state.wake_lock.set_disconnect_handler(
      base::BindOnce(&ElectronPowerSaveBlocker::ReleaseOwner,
                     weak_factory_.GetWeakPtr(), owner));
  state.effective_type = type;
  state.wake_lock->RequestWakeLock();
  if (previous) {
    previous->CancelWakeLock();
  }
}

}  // namespace xenon::ipc
