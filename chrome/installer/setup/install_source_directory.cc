// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/installer/setup/install_source_directory.h"

#include <windows.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/command_line.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/types/expected_macros.h"
#include "base/version.h"
#include "chrome/common/chrome_constants.h"
#include "chrome/installer/setup/setup_constants.h"

namespace installer {
namespace {

constexpr char kDeletionsFile[] = ".xenon-deletions.json";

bool IsSafeComponent(std::wstring_view component) {
  return !component.empty() && component != L"." && component != L".." &&
         component.back() != L'.' && component.back() != L' ' &&
         component.find_first_of(L":*?\"<>|") == std::wstring_view::npos &&
         component.find(L'\0') == std::wstring_view::npos &&
         !base::IsReservedNameOnWindows(std::wstring(component));
}

bool IsSafeRelativePath(const base::FilePath& path) {
  if (path.empty() || path.IsAbsolute() || path.ReferencesParent()) {
    return false;
  }
  for (auto component : base::SplitStringPiece(
           path.value(), L"\\/", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL)) {
    if (!IsSafeComponent(component)) {
      return false;
    }
  }
  return true;
}

bool IsPrivateMetadata(const base::FilePath& relative) {
  for (const auto& component : relative.GetComponents()) {
    if (base::StartsWith(component, L".xenon-",
                         base::CompareCase::INSENSITIVE_ASCII)) {
      return true;
    }
  }
  return false;
}

bool HasPlainAncestors(base::FilePath path) {
  for (;;) {
    const DWORD attributes = ::GetFileAttributesW(path.value().c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
      return false;
    }
    const auto parent = path.DirName();
    if (parent == path) {
      return true;
    }
    path = parent;
  }
}

std::optional<base::FilePath> PlainDirectory(const base::FilePath& path) {
  const auto normalized =
      path.NormalizePathSeparators().StripTrailingSeparators();
  const auto& value = normalized.value();
  // Avoid UNC/device paths, alternate streams and Win32 name normalization.
  if (value.size() <= 3 || !base::IsAsciiAlpha(value[0]) || value[1] != L':' ||
      value[2] != L'\\' ||
      !IsSafeRelativePath(base::FilePath(value.substr(3))) ||
      !HasPlainAncestors(normalized)) {
    return std::nullopt;
  }
  base::FilePath resolved;
  // Resolve 8.3 aliases before comparing directory identities/ancestry.
  if (!base::NormalizeFilePath(normalized, &resolved) ||
      !HasPlainAncestors(resolved)) {
    return std::nullopt;
  }
  return resolved;
}

bool SamePath(const base::FilePath& left, const base::FilePath& right) {
  return base::FilePath::CompareEqualIgnoreCase(left.value(), right.value());
}

bool IsParentIgnoringCase(const base::FilePath& parent,
                          const base::FilePath& child) {
  const auto parent_components = parent.GetComponents();
  const auto child_components = child.GetComponents();
  if (parent_components.size() >= child_components.size()) {
    return false;
  }
  for (size_t index = 0; index < parent_components.size(); ++index) {
    if (!base::FilePath::CompareEqualIgnoreCase(parent_components[index],
                                                child_components[index])) {
      return false;
    }
  }
  return true;
}

bool Overlaps(const base::FilePath& left, const base::FilePath& right) {
  return SamePath(left, right) || IsParentIgnoringCase(left, right) ||
         IsParentIgnoringCase(right, left);
}

bool HasNoTargetReparsePoints(const base::FilePath& target,
                              const base::FilePath& relative) {
  base::FilePath current = target;
  for (const auto& component : relative.GetComponents()) {
    current = current.Append(component);
    const DWORD attributes = ::GetFileAttributesW(current.value().c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
      const DWORD error = ::GetLastError();
      return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    }
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
      return false;
    }
  }
  return true;
}

bool IsPlainFile(const base::FilePath& path) {
  const DWORD attributes = ::GetFileAttributesW(path.value().c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         !(attributes &
           (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
}

bool IsNonemptyPlainFile(const base::FilePath& path) {
  return IsPlainFile(path) && base::GetFileSize(path).value_or(0) > 0;
}

struct SourceEntry {
  base::FilePath relative;
  bool directory;
};

base::expected<std::vector<SourceEntry>, InstallStatus> EnumerateSource(
    const base::FilePath& source) {
  std::vector<SourceEntry> result;
  size_t version_count = 0;
  base::FileEnumerator entries(
      source, true,
      base::FileEnumerator::FILES | base::FileEnumerator::DIRECTORIES,
      base::FilePath::StringType(),
      base::FileEnumerator::FolderSearchPolicy::ALL,
      base::FileEnumerator::ErrorPolicy::STOP_ENUMERATION);
  for (auto path = entries.Next(); !path.empty(); path = entries.Next()) {
    base::FilePath relative;
    const DWORD attributes = ::GetFileAttributesW(path.value().c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        !source.AppendRelativePath(path, &relative) ||
        !IsSafeRelativePath(relative) || !HasPlainAncestors(path.DirName())) {
      return base::unexpected(INVALID_ARCHIVE);
    }
    const bool is_directory = attributes & FILE_ATTRIBUTE_DIRECTORY;
    if (SamePath(relative, base::FilePath::FromASCII(kDeletionsFile))) {
      if (is_directory) {
        return base::unexpected(INVALID_ARCHIVE);
      }
      continue;
    }
    if (IsPrivateMetadata(relative)) {
      return base::unexpected(INVALID_ARCHIVE);
    }
    if (relative.DirName() == base::FilePath(FILE_PATH_LITERAL(".")) &&
        is_directory && base::Version(relative.MaybeAsASCII()).IsValid()) {
      ++version_count;
      if (relative.MaybeAsASCII() != chrome::kChromeVersion) {
        return base::unexpected(INVALID_ARCHIVE);
      }
    }
    result.push_back({relative, is_directory});
  }
  if (entries.GetError() != base::File::FILE_OK || version_count != 1 ||
      !IsNonemptyPlainFile(source.Append(kChromeExe)) ||
      !IsNonemptyPlainFile(
          source.AppendASCII(chrome::kChromeVersion).Append(kChromeDll))) {
    return base::unexpected(INVALID_ARCHIVE);
  }
  return result;
}

base::expected<std::vector<base::FilePath>, InstallStatus> ReadDeletions(
    const base::FilePath& source,
    const base::FilePath& target,
    const std::vector<SourceEntry>& entries) {
  std::vector<base::FilePath> result;
  const auto metadata = source.AppendASCII(kDeletionsFile);
  const DWORD attributes = ::GetFileAttributesW(metadata.value().c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    if (::GetLastError() == ERROR_FILE_NOT_FOUND) {
      return result;
    }
    return base::unexpected(INVALID_ARCHIVE);
  }
  std::string json;
  if (!IsPlainFile(metadata) ||
      !base::ReadFileToStringWithMaxSize(metadata, &json, 1024 * 1024)) {
    return base::unexpected(INVALID_ARCHIVE);
  }
  auto deletions = base::JSONReader::ReadList(json, base::JSON_PARSE_RFC);
  if (!deletions) {
    return base::unexpected(INVALID_ARCHIVE);
  }
  for (const auto& value : *deletions) {
    if (!value.is_string() || !base::IsStringUTF8(value.GetString()) ||
        value.GetString().find('\0') != std::string::npos) {
      return base::unexpected(INVALID_ARCHIVE);
    }
    const auto relative = base::FilePath::FromUTF8Unsafe(value.GetString())
                              .NormalizePathSeparators();
    if (!IsSafeInstallSourceDeletion(target, relative)) {
      return base::unexpected(INVALID_ARCHIVE);
    }
    for (const auto& entry : entries) {
      // Deleting a missing child of a payload directory is permitted; deleting
      // any payload item, its ancestor, or a child of a payload file is not.
      if (SamePath(relative, entry.relative) ||
          (!entry.directory && Overlaps(relative, entry.relative)) ||
          IsParentIgnoringCase(relative, entry.relative)) {
        return base::unexpected(INVALID_ARCHIVE);
      }
    }
    for (const auto& previous : result) {
      if (Overlaps(relative, previous)) {
        return base::unexpected(INVALID_ARCHIVE);
      }
    }
    result.push_back(relative);
  }
  return result;
}

}  // namespace

base::expected<void, InstallStatus> ValidateInstallSourceDirectoryCommandLine(
    const base::CommandLine& command_line,
    const base::FilePath& target_path) {
  if (!command_line.HasSwitch(switches::kInstallSourceDir) ||
      !command_line.HasSwitch(switches::kInstallDirectory) ||
      command_line.HasSwitch(switches::kInstallArchive) ||
      command_line.HasSwitch(switches::kUncompressedArchive) ||
      command_line.HasSwitch(switches::kUninstall)) {
    return base::unexpected(UNSUPPORTED_OPTION);
  }
  const auto source = PlainDirectory(
      command_line.GetSwitchValuePath(switches::kInstallSourceDir));
  const auto requested = PlainDirectory(
      command_line.GetSwitchValuePath(switches::kInstallDirectory));
  const auto target = PlainDirectory(target_path);
  if (!source || !requested || !target || !SamePath(*requested, *target) ||
      Overlaps(*source, *target)) {
    return base::unexpected(UNSUPPORTED_OPTION);
  }
  return base::ok();
}

base::expected<std::vector<base::FilePath>, InstallStatus>
PrepareInstallSourceDirectory(const base::CommandLine& command_line,
                              const base::FilePath& target_path,
                              const base::FilePath& unpack_path) {
  RETURN_IF_ERROR(
      ValidateInstallSourceDirectoryCommandLine(command_line, target_path));
  const auto source = PlainDirectory(
      command_line.GetSwitchValuePath(switches::kInstallSourceDir));
  const auto target = PlainDirectory(target_path);
  const auto unpack = PlainDirectory(unpack_path);
  if (!source || !target || !unpack || Overlaps(*source, *unpack) ||
      Overlaps(*target, *unpack)) {
    return base::unexpected(UNSUPPORTED_OPTION);
  }
  ASSIGN_OR_RETURN(auto entries, EnumerateSource(*source));
  for (const auto& entry : entries) {
    if (!HasNoTargetReparsePoints(*target, entry.relative)) {
      return base::unexpected(INVALID_ARCHIVE);
    }
  }
  ASSIGN_OR_RETURN(auto deletions, ReadDeletions(*source, *target, entries));
  const auto destination = unpack->Append(kInstallSourceChromeDir);
  if (base::PathExists(destination) || !base::CreateDirectory(destination)) {
    return base::unexpected(UNPACKING_FAILED);
  }
  for (const auto& entry : entries) {
    const auto input = source->Append(entry.relative);
    const auto output = destination.Append(entry.relative);
    if (!HasPlainAncestors(input.DirName()) ||
        !HasPlainAncestors(output.DirName()) ||
        (entry.directory
             ? !PlainDirectory(input) || !base::CreateDirectory(output)
             : !IsPlainFile(input) || !base::CopyFile(input, output))) {
      return base::unexpected(UNPACKING_FAILED);
    }
  }
  return deletions;
}

bool IsSafeInstallSourceDeletion(const base::FilePath& target_path,
                                 const base::FilePath& relative_path) {
  if (!IsSafeRelativePath(relative_path) || IsPrivateMetadata(relative_path) ||
      base::Version(
          base::FilePath(relative_path.GetComponents().front()).MaybeAsASCII())
          .IsValid() ||
      !PlainDirectory(target_path)) {
    return false;
  }
  base::FilePath current = target_path;
  const auto components = relative_path.GetComponents();
  for (size_t index = 0; index < components.size(); ++index) {
    current = current.Append(components[index]);
    const DWORD attributes = ::GetFileAttributesW(current.value().c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
      const DWORD error = ::GetLastError();
      return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) !=
            (index + 1 < components.size())) {
      return false;
    }
    base::FilePath canonical;
    if (!base::NormalizeFilePath(current, &canonical) ||
        !SamePath(canonical, current)) {
      // A short-name alias must not bypass version/metadata exclusions or
      // alias a path present in the new payload.
      return false;
    }
  }
  return true;
}

}  // namespace installer
