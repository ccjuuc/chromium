// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_RENDERER_IPC_XENON_PROCESS_SANDBOX_H_
#define XENON_OVERLAY_CHROME_RENDERER_IPC_XENON_PROCESS_SANDBOX_H_

namespace xenon::ipc {

// Reports the OS sandbox, independently of Electron's preload preferences.
// Unlike Chromium's DCHECK helper, --no-sandbox is always reported as false.
bool HasActiveProcessSandbox();

}  // namespace xenon::ipc

#endif  // XENON_OVERLAY_CHROME_RENDERER_IPC_XENON_PROCESS_SANDBOX_H_
