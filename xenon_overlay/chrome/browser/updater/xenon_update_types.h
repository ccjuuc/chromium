// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_UPDATER_XENON_UPDATE_TYPES_H_
#define XENON_OVERLAY_CHROME_BROWSER_UPDATER_XENON_UPDATE_TYPES_H_

#include <stdint.h>

#include <optional>
#include <string>

#include "base/values.h"
#include "url/gurl.h"

namespace xenon::updater {

enum class UpdateState {
  kIdle,
  kCheckingForUpdate,
  kUpdateAvailable,
  kUpdateNotAvailable,
  kDownloading,
  kPatching,
  kUpdateDownloaded,
  kError,
};

const char* UpdateStateToString(UpdateState state);

struct UpdatePackageInfo {
  GURL url;
  int64_t size = 0;
  std::string sha256;

  bool IsValid() const;
  base::DictValue ToValue() const;
  static std::optional<UpdatePackageInfo> FromValue(const base::DictValue& dict);
};

struct UpdateManifest {
  std::string target_version;
  bool is_diff = false;
  std::optional<UpdatePackageInfo> diff_package;
  std::optional<UpdatePackageInfo> full_package;
  std::vector<std::string> deletions;
  std::string changelog;
  bool force_update = false;

  bool IsValid() const;
  base::DictValue ToValue() const;
  static std::optional<UpdateManifest> FromJson(const std::string& json_str);
  static std::optional<UpdateManifest> FromValue(const base::DictValue& dict);
};

struct DownloadProgress {
  int64_t bytes_downloaded = 0;
  int64_t total_bytes = 0;
  int percent = 0;

  base::DictValue ToValue() const;
};

}  // namespace xenon::updater

#endif  // XENON_OVERLAY_CHROME_BROWSER_UPDATER_XENON_UPDATE_TYPES_H_
