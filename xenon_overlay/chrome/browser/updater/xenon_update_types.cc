// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/updater/xenon_update_types.h"

#include <algorithm>

#include "base/json/json_reader.h"

namespace xenon::updater {

const char* UpdateStateToString(UpdateState state) {
  switch (state) {
    case UpdateState::kIdle:
      return "idle";
    case UpdateState::kCheckingForUpdate:
      return "checking-for-update";
    case UpdateState::kUpdateAvailable:
      return "update-available";
    case UpdateState::kUpdateNotAvailable:
      return "update-not-available";
    case UpdateState::kDownloading:
      return "downloading";
    case UpdateState::kPatching:
      return "patching";
    case UpdateState::kUpdateDownloaded:
      return "update-downloaded";
    case UpdateState::kError:
      return "error";
  }
}

bool UpdatePackageInfo::IsValid() const {
  return url.is_valid() && size > 0 && !sha256.empty();
}

base::DictValue UpdatePackageInfo::ToValue() const {
  base::DictValue dict;
  dict.Set("url", url.spec());
  dict.Set("size", static_cast<double>(size));
  dict.Set("sha256", sha256);
  return dict;
}

std::optional<UpdatePackageInfo> UpdatePackageInfo::FromValue(
    const base::DictValue& dict) {
  const std::string* url_str = dict.FindString("url");
  if (!url_str) {
    return std::nullopt;
  }
  GURL parsed_url(*url_str);
  if (!parsed_url.is_valid()) {
    return std::nullopt;
  }

  int64_t package_size = 0;
  if (std::optional<double> size_double = dict.FindDouble("size")) {
    package_size = static_cast<int64_t>(*size_double);
  } else if (std::optional<int> size_int = dict.FindInt("size")) {
    package_size = *size_int;
  }

  const std::string* sha256_str = dict.FindString("sha256");
  std::string sha256 = sha256_str ? *sha256_str : "";

  UpdatePackageInfo info;
  info.url = std::move(parsed_url);
  info.size = package_size;
  info.sha256 = std::move(sha256);

  if (!info.IsValid()) {
    return std::nullopt;
  }
  return info;
}

bool UpdateManifest::IsValid() const {
  if (target_version.empty()) {
    return false;
  }
  if (is_diff) {
    return diff_package.has_value() && diff_package->IsValid();
  }
  return full_package.has_value() && full_package->IsValid();
}

base::DictValue UpdateManifest::ToValue() const {
  base::DictValue dict;
  dict.Set("target_version", target_version);
  dict.Set("is_diff", is_diff);
  if (diff_package) {
    dict.Set("diff_package", diff_package->ToValue());
  }
  if (full_package) {
    dict.Set("full_package", full_package->ToValue());
  }
  if (!deletions.empty()) {
    base::ListValue del_list;
    for (const auto& del : deletions) {
      del_list.Append(del);
    }
    dict.Set("deletions", std::move(del_list));
  }
  dict.Set("changelog", changelog);
  dict.Set("force_update", force_update);
  return dict;
}

std::optional<UpdateManifest> UpdateManifest::FromJson(
    const std::string& json_str) {
  auto parsed = base::JSONReader::Read(json_str, base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict()) {
    return std::nullopt;
  }
  return FromValue(parsed->GetDict());
}

std::optional<UpdateManifest> UpdateManifest::FromValue(
    const base::DictValue& dict) {
  // Support either direct schema or wrapped under "data"
  const base::DictValue* root = &dict;
  if (const base::DictValue* data_dict = dict.FindDict("data")) {
    root = data_dict;
  }

  const std::string* version = root->FindString("target_version");
  if (!version) {
    version = root->FindString("version");
  }
  if (!version || version->empty()) {
    return std::nullopt;
  }

  UpdateManifest manifest;
  manifest.target_version = *version;
  manifest.is_diff = root->FindBool("is_diff").value_or(false);

  if (const base::DictValue* diff_dict = root->FindDict("diff_package")) {
    manifest.diff_package = UpdatePackageInfo::FromValue(*diff_dict);
  }
  if (const base::DictValue* full_dict = root->FindDict("full_package")) {
    manifest.full_package = UpdatePackageInfo::FromValue(*full_dict);
  } else if (!manifest.is_diff) {
    // If is_diff is false and url is given at top-level
    manifest.full_package = UpdatePackageInfo::FromValue(*root);
  }

  if (const base::ListValue* del_list = root->FindList("deletions")) {
    for (const auto& item : *del_list) {
      if (item.is_string()) {
        manifest.deletions.push_back(item.GetString());
      }
    }
  }

  if (const std::string* notes = root->FindString("changelog")) {
    manifest.changelog = *notes;
  } else if (const std::string* release_notes = root->FindString("releaseNotes")) {
    manifest.changelog = *release_notes;
  }

  manifest.force_update = root->FindBool("force_update").value_or(false);

  if (!manifest.IsValid()) {
    return std::nullopt;
  }
  return manifest;
}

base::DictValue DownloadProgress::ToValue() const {
  base::DictValue dict;
  dict.Set("bytes_downloaded", static_cast<double>(bytes_downloaded));
  dict.Set("total_bytes", static_cast<double>(total_bytes));
  dict.Set("percent", percent);
  return dict;
}

}  // namespace xenon::updater
