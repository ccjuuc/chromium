// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_PERMISSION_CHECK_REMOTE_H_
#define XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_PERMISSION_CHECK_REMOTE_H_

#include <atomic>
#include <string>
#include <utility>

#include "base/memory/ref_counted.h"
#include "base/memory/scoped_refptr.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "base/types/expected.h"
#include "base/values.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/shared_remote.h"
#include "xenon_overlay/public/mojom/xenon_permission_check.mojom.h"

namespace xenon::ipc {

// True only while this sequence is synchronously waiting for a permission
// handler. Reverse calls that require ordinary asynchronous tasks must reject
// rather than wait for work which cannot run until the handler returns.
bool IsElectronPermissionCheckInProgress();

// The remote is owned by a separate sequence so a deadline can disconnect it
// while the calling sequence waits for a synchronous reply. This transport is
// covered by real Mojo reverse-call, timeout, disconnect and retirement tests.
class ElectronPermissionCheckRemote {
 public:
  ElectronPermissionCheckRemote(
      mojo::PendingRemote<mojom::ElectronPermissionCheck> remote,
      scoped_refptr<base::SequencedTaskRunner> transport_runner,
      base::TimeDelta timeout);
  ElectronPermissionCheckRemote(const ElectronPermissionCheckRemote&) = delete;
  ElectronPermissionCheckRemote& operator=(
      const ElectronPermissionCheckRemote&) = delete;
  ~ElectronPermissionCheckRemote();

  base::expected<bool, std::string> Check(base::Value request);
  void Close();

 private:
  const scoped_refptr<base::SequencedTaskRunner> transport_runner_;
  const base::TimeDelta timeout_;
  const scoped_refptr<base::RefCountedData<std::atomic_bool>> closed_ =
      base::MakeRefCounted<base::RefCountedData<std::atomic_bool>>(
          std::in_place,
          false);
  mojo::SharedRemote<mojom::ElectronPermissionCheck> remote_;
};

}  // namespace xenon::ipc

#endif  // XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_PERMISSION_CHECK_REMOTE_H_
