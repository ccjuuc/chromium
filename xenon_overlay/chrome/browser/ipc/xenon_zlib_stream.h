// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
#ifndef XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ZLIB_STREAM_H_
#define XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ZLIB_STREAM_H_

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>

#include "base/memory/ref_counted.h"
#include "base/values.h"
#include "xenon_overlay/public/mojom/xenon_ipc.mojom.h"

namespace xenon::ipc {

// Plain-data codec state. Only one Process call may run at a time. Cancellation
// never waits for the worker; its last reference releases the codec afterwards.
class ZlibStream final : public base::RefCountedThreadSafe<ZlibStream> {
 public:
  static constexpr size_t kMaximumChunk = 64 * 1024;
  static scoped_refptr<ZlibStream> Create(const std::string& operation,
                                          const base::DictValue& options,
                                          std::string* error);
  bool TryBegin();
  void Cancel();
  mojom::IpcResultPtr Process(base::Value::BlobStorage input,
                              int flush,
                              int output_size);

 private:
  friend class base::RefCountedThreadSafe<ZlibStream>;
  struct State;
  explicit ZlibStream(std::unique_ptr<State> state);
  ~ZlibStream();
  std::unique_ptr<State> state_;
  std::atomic<bool> busy_{false};
  std::atomic<bool> cancelled_{false};
};

// One-shot convenience operations retain their existing bounded-output policy.
mojom::IpcResultPtr PerformStreamingZlibCall(std::string operation,
                                             base::Value::BlobStorage input,
                                             base::DictValue options);

}  // namespace xenon::ipc
#endif  // XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ZLIB_STREAM_H_
