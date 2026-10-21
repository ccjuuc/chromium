// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/updater/xenon_update_bundle.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

#include "base/files/file.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/json/json_reader.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/values.h"
#include "base/version.h"
#include "build/build_config.h"
#include "crypto/hash.h"
#include "xenon_overlay/chrome/browser/updater/xenon_update_patcher.h"

#if BUILDFLAG(IS_POSIX)
#include <sys/stat.h>
#endif

namespace xenon::updater {
namespace {

constexpr size_t kMaxManifestSize = 32 * 1024 * 1024;

struct Entry {
  std::string action;
  std::string target;
  std::string base;
  std::string payload;
  std::string link_target;
  int mode = 0;
  int64_t size = 0;
  std::array<uint8_t, crypto::hash::kSha256Size> sha256{};
};

using Entries = std::map<std::string, Entry>;

bool Fail(std::string* error, std::string message) {
  if (error) {
    *error = std::move(message);
  }
  return false;
}

std::vector<std::string> Components(std::string_view path) {
  return base::SplitString(path, "/", base::KEEP_WHITESPACE,
                           base::SPLIT_WANT_ALL);
}

bool IsRelativePath(std::string_view path, bool allow_dot_components = false) {
  if (path.empty() || path.front() == '/' || path.back() == '/' ||
      path.find_first_of("\\:") != std::string_view::npos ||
      path.find('\0') != std::string_view::npos || !base::IsStringUTF8(path)) {
    return false;
  }
  for (const auto& component : Components(path)) {
    if (component.empty() ||
        (!allow_dot_components && (component == "." || component == ".."))) {
      return false;
    }
#if BUILDFLAG(IS_WIN)
    // Avoid Win32 aliases (trailing dots/spaces, streams and device names) in
    // the cross-platform reconstruction tests as well.
    if (component != "." && component != ".." &&
        (component.back() == '.' || component.back() == ' ')) {
      return false;
    }
    std::string stem =
        base::ToUpperASCII(component.substr(0, component.find('.')));
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" ||
        (stem.size() == 4 &&
         (stem.starts_with("COM") || stem.starts_with("LPT")) &&
         stem[3] >= '1' && stem[3] <= '9')) {
      return false;
    }
#endif
  }
  return true;
}

std::string Parent(std::string_view path) {
  const size_t slash = path.rfind('/');
  return slash == std::string_view::npos ? std::string()
                                         : std::string(path.substr(0, slash));
}

base::FilePath Append(const base::FilePath& root, std::string_view relative) {
  return root.Append(base::FilePath::FromUTF8Unsafe(relative));
}

bool IsRegularFile(const base::FilePath& path) {
#if BUILDFLAG(IS_POSIX)
  base::stat_wrapper_t info;
  return base::File::Lstat(path, &info) == 0 && S_ISREG(info.st_mode);
#else
  base::File::Info info;
  return !base::IsLink(path) && base::GetFileInfo(path, &info) &&
         !info.is_directory;
#endif
}

// Reject every symlink component, including links which point back inside the
// tree. The generator records real versioned paths rather than Current aliases.
bool ResolveRegularFile(const base::FilePath& root,
                        const std::string& relative,
                        base::FilePath* result) {
  if (!IsRelativePath(relative)) {
    return false;
  }
  base::FilePath path = root;
  const auto components = Components(relative);
  for (size_t i = 0; i < components.size(); ++i) {
    path = Append(path, components[i]);
    if (base::IsLink(path) ||
        (i + 1 < components.size() && !base::DirectoryExists(path))) {
      return false;
    }
  }
  if (!IsRegularFile(path)) {
    return false;
  }
  *result = path;
  return true;
}

std::optional<int> ReadMode(const base::DictValue& value,
                            std::string_view key) {
  std::optional<int> mode = value.FindInt(key);
  return mode && *mode >= 0 && *mode <= 0777 ? mode : std::nullopt;
}

bool SetMode(const base::FilePath& path, int mode) {
#if BUILDFLAG(IS_POSIX)
  return base::SetPosixFilePermissions(path, mode);
#else
  return true;
#endif
}

bool ParseEntry(const base::DictValue& dict, Entry* entry) {
  const std::string* action = dict.FindString("action");
  const std::string* target = dict.FindString("target");
  if (!action || !target || !IsRelativePath(*target)) {
    return false;
  }
  entry->action = *action;
  entry->target = *target;
  if (*action == "symlink") {
    const std::string* link_target = dict.FindString("link_target");
    if (!link_target || !IsRelativePath(*link_target, true)) {
      return false;
    }
    entry->link_target = *link_target;
    return true;
  }
  const auto mode = ReadMode(dict, "mode");
  if (!mode) {
    return false;
  }
  entry->mode = *mode;
  if (*action == "directory") {
    return true;
  }
  if (*action != "copy" && *action != "add" && *action != "patch") {
    return false;
  }
  const std::string* sha256 = dict.FindString("sha256");
  // JSON integers beyond int32 are represented as doubles. Limit sizes to the
  // exactly representable range and reject fractional/negative values.
  const auto size = dict.FindDouble("size");
  if (!sha256 || !base::HexStringToSpan(*sha256, entry->sha256) || !size ||
      !std::isfinite(*size) || *size < 0 || *size > 9007199254740991.0 ||
      std::floor(*size) != *size) {
    return false;
  }
  entry->size = static_cast<int64_t>(*size);
  if (*action == "copy" || *action == "patch") {
    const std::string* base = dict.FindString("base");
    if (!base || !IsRelativePath(*base)) {
      return false;
    }
    entry->base = *base;
  }
  if (*action == "add" || *action == "patch") {
    const std::string* payload =
        dict.FindString(*action == "add" ? "source" : "patch");
    if (!payload || !IsRelativePath(*payload)) {
      return false;
    }
    entry->payload = *payload;
  }
  return true;
}

// Resolve symlinks using the declared tree, expanding links before processing
// '..'. A lexical containment check alone misses alias/.. escapes.
bool LinkStaysInTree(const Entry& link, const Entries& entries) {
  std::string parent = Parent(link.target);
  std::vector<std::string> resolved =
      parent.empty() ? std::vector<std::string>() : Components(parent);
  std::deque<std::string> pending;
  for (const auto& part : Components(link.link_target)) {
    pending.push_back(part);
  }
  size_t followed_links = 0;
  while (!pending.empty()) {
    std::string part = std::move(pending.front());
    pending.pop_front();
    if (part == ".") {
      continue;
    }
    if (part == "..") {
      if (resolved.empty()) {
        return false;
      }
      resolved.pop_back();
      continue;
    }
    resolved.push_back(part);
    auto found = entries.find(base::JoinString(resolved, "/"));
    if (found == entries.end()) {
      return false;
    }
    if (found->second.action == "symlink") {
      if (++followed_links > 40) {
        return false;
      }
      resolved.pop_back();
      auto target_parts = Components(found->second.link_target);
      for (auto it = target_parts.rbegin(); it != target_parts.rend(); ++it) {
        pending.push_front(*it);
      }
    } else if (!pending.empty() && found->second.action != "directory") {
      return false;
    }
  }
  return true;
}

bool VerifyFile(const base::FilePath& path, const Entry& entry) {
  base::File file(path, base::File::FLAG_OPEN | base::File::FLAG_READ |
                            base::File::FLAG_NO_FOLLOW);
  std::array<uint8_t, crypto::hash::kSha256Size> digest;
  return file.IsValid() && file.GetLength() == entry.size &&
         crypto::hash::HashFile(crypto::hash::kSha256, &file, digest) &&
         digest == entry.sha256;
}

bool Overlap(const base::FilePath& a, const base::FilePath& b) {
  return a == b || a.IsParent(b) || b.IsParent(a);
}

}  // namespace

bool ReconstructMacUpdateBundle(const base::FilePath& current_app,
                                const base::FilePath& unpacked_patch,
                                const base::FilePath& output_app,
                                const std::string& expected_base_version,
                                const std::string& expected_target_version,
                                std::string* error) {
  if (error) {
    error->clear();
  }
  if (!current_app.IsAbsolute() || !unpacked_patch.IsAbsolute() ||
      !output_app.IsAbsolute() || current_app.ReferencesParent() ||
      unpacked_patch.ReferencesParent() || output_app.ReferencesParent() ||
      base::IsLink(current_app) || base::IsLink(unpacked_patch) ||
      base::IsLink(output_app) || base::PathExists(output_app) ||
      !base::DirectoryExists(current_app) ||
      !base::DirectoryExists(unpacked_patch) ||
      !base::DirectoryExists(output_app.DirName())) {
    return Fail(error, "Invalid bundle input or output directory");
  }
  const base::FilePath current = base::MakeAbsoluteFilePath(current_app);
  const base::FilePath patch = base::MakeAbsoluteFilePath(unpacked_patch);
  const base::FilePath output_parent =
      base::MakeAbsoluteFilePath(output_app.DirName());
  if (current.empty() || patch.empty() || output_parent.empty()) {
    return Fail(error, "Cannot resolve bundle directories");
  }
  const base::FilePath output = output_parent.Append(output_app.BaseName());
  if (Overlap(output, current) || Overlap(output, patch)) {
    return Fail(error, "Output bundle overlaps an input directory");
  }

  base::FilePath manifest_path;
  std::string json;
  if (!ResolveRegularFile(patch, "manifest.json", &manifest_path) ||
      !base::ReadFileToStringWithMaxSize(manifest_path, &json,
                                         kMaxManifestSize)) {
    return Fail(error, "Cannot read update bundle manifest");
  }
  auto manifest = base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
  if (!manifest || manifest->FindInt("schema_version") != 2 ||
      !manifest->FindString("platform") ||
      *manifest->FindString("platform") != "mac" ||
      !base::Version(expected_base_version).IsValid() ||
      !base::Version(expected_target_version).IsValid() ||
      !manifest->FindString("base_version") ||
      *manifest->FindString("base_version") != expected_base_version ||
      !manifest->FindString("target_version") ||
      *manifest->FindString("target_version") != expected_target_version) {
    return Fail(error, "Invalid mac update manifest or version mismatch");
  }
  const auto root_mode = ReadMode(*manifest, "root_mode");
  const auto* actions = manifest->FindList("actions");
  if (!root_mode || !actions || actions->empty()) {
    return Fail(error, "Manifest is missing root mode or actions");
  }

  Entries entries;
  std::set<std::string> folded_targets;
  for (const auto& action : *actions) {
    Entry entry;
    if (!action.is_dict() || !ParseEntry(action.GetDict(), &entry)) {
      return Fail(error, "Invalid action in mac update manifest");
    }
    if (!folded_targets.insert(base::ToLowerASCII(entry.target)).second) {
      return Fail(error, "Duplicate manifest target: " + entry.target);
    }
    std::string target = entry.target;
    entries.emplace(std::move(target), std::move(entry));
  }
  for (const auto& [target, entry] : entries) {
    std::string parent = Parent(target);
    if (!parent.empty()) {
      auto it = entries.find(parent);
      if (it == entries.end() || it->second.action != "directory") {
        return Fail(error,
                    "Target parent is not a declared directory: " + target);
      }
    }
    if (entry.action == "symlink" && !LinkStaysInTree(entry, entries)) {
      return Fail(error, "Unsafe or unresolved symbolic link: " + target);
    }
  }

  if (!base::CreateDirectory(output)) {
    return Fail(error, "Cannot create output bundle");
  }
  std::vector<base::FilePath> directories{output};
  base::ScopedClosureRunner cleanup(base::BindOnce(
      [](const base::FilePath& output,
         const std::vector<base::FilePath>* directories) {
        // Restore write/search permission before removing a partially built
        // tree. These paths were created here and never include symlinks.
        for (const auto& dir : *directories) {
          if (!base::IsLink(dir)) {
            SetMode(dir, 0700);
          }
        }
        base::DeletePathRecursively(output);
      },
      output, base::Unretained(&directories)));
  if (!SetMode(output, 0700)) {
    return Fail(error, "Cannot protect output staging directory");
  }
  // Map order creates parents before their children. Permissions are applied
  // only after all contents exist, so read-only directories are supported.
  for (const auto& [target, entry] : entries) {
    if (entry.action == "directory") {
      base::FilePath destination = Append(output, target);
      if (base::PathExists(destination) || base::IsLink(destination) ||
          !base::CreateDirectory(destination)) {
        return Fail(error, "Cannot create bundle directory: " + target);
      }
      directories.push_back(destination);
    }
  }
  for (const auto& [target, entry] : entries) {
    if (entry.action == "directory" || entry.action == "symlink") {
      continue;
    }
    base::FilePath destination = Append(output, target);
    base::FilePath source;
    base::FilePath payload;
    if ((!entry.base.empty() &&
         !ResolveRegularFile(current, entry.base, &source)) ||
        (!entry.payload.empty() &&
         !ResolveRegularFile(patch, entry.payload, &payload))) {
      return Fail(error, "Missing or unsafe source for: " + target);
    }
    if (base::PathExists(destination) || base::IsLink(destination)) {
      return Fail(error, "Output target already exists: " + target);
    }
    if (entry.action == "patch") {
      std::string patch_error;
      if (!XenonUpdatePatcher::ApplyPatch(source, payload, destination,
                                          &patch_error)) {
        return Fail(error, "Cannot patch " + target + ": " + patch_error);
      }
    } else if (!base::CopyFile(entry.action == "copy" ? source : payload,
                               destination)) {
      return Fail(error, "Cannot copy bundle file: " + target);
    }
    if (!VerifyFile(destination, entry)) {
      return Fail(error, "Bundle file size or SHA-256 mismatch: " + target);
    }
    if (!SetMode(destination, entry.mode)) {
      return Fail(error, "Cannot restore bundle file permissions: " + target);
    }
  }
  for (const auto& [target, entry] : entries) {
    if (entry.action != "symlink") {
      continue;
    }
#if BUILDFLAG(IS_POSIX)
    if (!base::CreateSymbolicLink(
            base::FilePath::FromUTF8Unsafe(entry.link_target),
            Append(output, target))) {
      return Fail(error, "Cannot create bundle symbolic link: " + target);
    }
#else
    return Fail(error, "Symbolic link reconstruction requires POSIX");
#endif
  }
  for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
    if (it->second.action == "directory" &&
        !SetMode(Append(output, it->first), it->second.mode)) {
      return Fail(error, "Cannot restore directory permissions: " + it->first);
    }
  }
  if (!SetMode(output, *root_mode)) {
    return Fail(error, "Cannot restore application permissions");
  }
  cleanup.Release().Reset();
  return true;
}

}  // namespace xenon::updater
