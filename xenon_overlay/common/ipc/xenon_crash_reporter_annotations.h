// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_COMMON_IPC_XENON_CRASH_REPORTER_ANNOTATIONS_H_
#define XENON_OVERLAY_COMMON_IPC_XENON_CRASH_REPORTER_ANNOTATIONS_H_

#include <stdint.h>

#include "base/values.h"
#include "xenon_overlay/public/mojom/xenon_ipc.mojom.h"

namespace xenon::ipc {

// Writes annotations in the calling process, never by Browser IPC. A main
// container owns one instance in its Utility process; each renderer binding
// owns one in its renderer process. Shutdown clears only that owner's keys.
// Crashpad annotation nodes themselves must remain alive for process lifetime.
class ElectronCrashReporterAnnotations {
 public:
  explicit ElectronCrashReporterAnnotations(bool allow_start = false);
  ~ElectronCrashReporterAnnotations();
  ElectronCrashReporterAnnotations(const ElectronCrashReporterAnnotations&) =
      delete;
  ElectronCrashReporterAnnotations& operator=(
      const ElectronCrashReporterAnnotations&) = delete;

  mojom::IpcResultPtr Call(base::Value request);
  void Clear();

 private:
  const uint64_t owner_id_;
  const bool allow_start_;
  bool started_ = false;
};

}  // namespace xenon::ipc

#endif  // XENON_OVERLAY_COMMON_IPC_XENON_CRASH_REPORTER_ANNOTATIONS_H_
