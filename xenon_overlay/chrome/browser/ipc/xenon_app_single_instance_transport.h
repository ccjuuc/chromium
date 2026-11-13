// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_APP_SINGLE_INSTANCE_TRANSPORT_H_
#define XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_APP_SINGLE_INSTANCE_TRANSPORT_H_

#include <memory>
#include <string>
#include <string_view>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/types/expected.h"

namespace xenon::ipc {

// Internal, bounded local transport. The caller must hold the application's
// actual file lock before starting a listener; the transport is not the lock.
class XenonAppSingleInstanceTransport {
 public:
  static constexpr size_t kMaxMessageBytes = 1024 * 1024;
  using MessageCallback = base::RepeatingCallback<void(std::string)>;

  virtual ~XenonAppSingleInstanceTransport() = default;
  virtual const std::string& endpoint() const = 0;

  static base::expected<std::unique_ptr<XenonAppSingleInstanceTransport>,
                        std::string>
  Listen(const std::string& lease, MessageCallback callback);
  static base::expected<void, std::string> Notify(const std::string& endpoint,
                                                  std::string_view message);
  static base::expected<std::string, std::string> UserIdentity();
};

}  // namespace xenon::ipc

#endif  // XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_APP_SINGLE_INSTANCE_TRANSPORT_H_
