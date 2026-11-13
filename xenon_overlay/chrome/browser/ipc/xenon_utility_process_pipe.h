// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_UTILITY_PROCESS_PIPE_H_
#define XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_UTILITY_PROCESS_PIPE_H_

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "base/files/file.h"
#include "base/functional/callback.h"
#include "base/memory/ref_counted.h"

namespace xenon::ipc {

// Browser owns the read endpoint; exactly one worker reads at a time. Dropping
// an application control pipe cancels the pending OS read without freeing its
// handle or buffer while the worker still uses them.
class UtilityProcessPipe
    : public base::RefCountedThreadSafe<UtilityProcessPipe> {
 public:
  struct ReadResult {
    std::vector<uint8_t> data;
    bool eof = false;
    std::string error;
  };
  static scoped_refptr<UtilityProcessPipe> Create(base::File* writer,
                                                  std::string* error);
  ReadResult Read(uint32_t maximum_bytes);
  void Cancel();
  // Install before scheduling Read. Runs once on the reader worker after the
  // pipe has no immediately available data and the read must wait.
  void SetReadPendingCallbackForTesting(base::OnceClosure callback);

 private:
  friend class base::RefCountedThreadSafe<UtilityProcessPipe>;
  explicit UtilityProcessPipe(base::File reader);
  ~UtilityProcessPipe();
  base::File reader_;
  std::atomic<bool> cancelled_{false};
  std::atomic<bool> reading_{false};
  base::OnceClosure read_pending_for_testing_;
};

}  // namespace xenon::ipc

#endif  // XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_UTILITY_PROCESS_PIPE_H_
