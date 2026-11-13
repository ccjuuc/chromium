// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_APP_PATHS_H_
#define XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_APP_PATHS_H_

#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/types/expected.h"

namespace xenon::ipc {

// An application's path overrides, owned by its main-process container. Never
// overrides the host's process-wide PathService. Call on one sequence where
// blocking filesystem operations are permitted.
class XenonAppPaths {
 public:
  struct Error {
    std::string code;
    std::string message;
  };
  using StoragePathCallback = base::RepeatingCallback<
      base::expected<base::FilePath, Error>(
          std::string, std::optional<base::FilePath>)>;

  XenonAppPaths(base::FilePath app_path,
                base::FilePath executable_path,
                std::string app_name,
                base::FilePath legacy_user_data_path = {},
                base::FilePath host_crash_dumps_path = {},
                StoragePathCallback storage_path = {});
  ~XenonAppPaths();

  XenonAppPaths(const XenonAppPaths&) = delete;
  XenonAppPaths& operator=(const XenonAppPaths&) = delete;

  base::expected<base::FilePath, Error> Get(std::string_view name);
  base::expected<void, Error> Set(std::string_view name,
                                  const base::FilePath& path);
  base::expected<void, Error> SetLogsPath(
      std::optional<base::FilePath> path = std::nullopt);

 private:
  base::expected<base::FilePath, Error> GetDefault(std::string_view name);
  base::expected<base::FilePath, Error> DefaultLogsPath();
  base::expected<base::FilePath, Error> AppendApplicationName(
      const base::FilePath& parent);

  const base::FilePath app_path_;
  const base::FilePath executable_path_;
  const std::string app_name_;
  const base::FilePath legacy_user_data_path_;
  // Supplied only by the host that owns Crashpad. PathService's generic crash
  // directory need not be the live Crashpad database, especially on Windows.
  const base::FilePath host_crash_dumps_path_;
  const StoragePathCallback storage_path_;
  std::map<std::string, base::FilePath, std::less<>> overrides_;
};

}  // namespace xenon::ipc

#endif  // XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_APP_PATHS_H_
