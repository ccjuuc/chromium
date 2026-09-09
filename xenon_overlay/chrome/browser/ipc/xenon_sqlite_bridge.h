// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_SQLITE_BRIDGE_H_
#define XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_SQLITE_BRIDGE_H_

#include <map>
#include <memory>

#include "base/sequence_checker.h"
#include "base/values.h"
#include "xenon_overlay/public/mojom/xenon_ipc.mojom.h"

namespace xenon::ipc {

// Owned by one trusted Electron document, on a blocking sequenced task runner.
// Handles cannot be used by other documents. Destruction finalizes statements
// and closes connections (rolling back any unfinished transactions).
class XenonSqliteBridge {
 public:
  XenonSqliteBridge();
  ~XenonSqliteBridge();
  XenonSqliteBridge(const XenonSqliteBridge&) = delete;
  XenonSqliteBridge& operator=(const XenonSqliteBridge&) = delete;

  mojom::IpcResultPtr Call(base::Value arguments);

 private:
  struct Database;
  std::map<int, std::unique_ptr<Database>> databases_;
  int next_database_id_ = 1;
  int next_statement_id_ = 1;
  SEQUENCE_CHECKER(sequence_checker_);
};

}  // namespace xenon::ipc

#endif  // XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_SQLITE_BRIDGE_H_
