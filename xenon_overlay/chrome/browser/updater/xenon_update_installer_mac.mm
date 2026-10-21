// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/updater/xenon_update_installer.h"

#import <Foundation/Foundation.h>
#import <Security/Security.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <deque>
#include <utility>
#include <vector>

#include "base/apple/foundation_util.h"
#include "base/apple/scoped_cftyperef.h"
#include "base/command_line.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/files/scoped_file.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/logging.h"
#include "base/process/launch.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/sys_string_conversions.h"
#include "base/version.h"
#include "third_party/zlib/contrib/minizip/unzip.h"
#include "third_party/zlib/google/zip.h"
#include "third_party/zlib/google/zip_reader.h"
#include "xenon_overlay/chrome/browser/updater/xenon_mac_install_script.h"

namespace xenon::updater {
namespace {

bool Fail(std::string* error, const std::string& message) {
  if (error) {
    *error = message;
  }
  LOG(ERROR) << "[XenonUpdateInstaller] " << message;
  return false;
}

bool RunCommand(const base::CommandLine& command,
                const char* operation,
                std::string* error) {
  std::string output;
  int exit_code = -1;
  if (!base::GetAppOutputWithExitCode(command, &output, &exit_code) ||
      exit_code != 0) {
    return Fail(error, std::string(operation) + " failed (exit " +
                           base::NumberToString(exit_code) + ")");
  }
  return true;
}

bool IsBasename(const base::FilePath& name) {
  return !name.empty() && !name.IsAbsolute() && !name.ReferencesParent() &&
         name.BaseName() == name && name.value() != ".";
}

struct ZipPermissions {
  base::FilePath path;
  mode_t mode;
};

bool ReadZipPermissions(const base::FilePath& archive,
                        std::vector<ZipPermissions>* permissions,
                        std::string* error) {
  zip::ZipReader reader;
  if (!reader.Open(archive)) {
    return Fail(error, "Could not read the full update ZIP");
  }
  // ZipReader supplies the same normalized paths that Unzip uses, but does
  // not expose the originating OS or file type. Read those from the matching
  // central-directory entry before trusting its POSIX permission bits.
  unzFile metadata = unzOpen64(archive.value().c_str());
  if (!metadata) {
    return Fail(error, "Could not read the update ZIP permission metadata");
  }
  base::ScopedClosureRunner close_metadata(
      base::BindOnce([](unzFile file) { unzClose(file); }, metadata));
  int metadata_status = unzGoToFirstFile(metadata);
  while (const zip::ZipReader::Entry* entry = reader.Next()) {
    unz_file_info64 info = {};
    if (entry->is_unsafe || metadata_status != UNZ_OK ||
        unzGetCurrentFileInfo64(metadata, &info, nullptr, 0, nullptr, 0,
                                nullptr, 0) != UNZ_OK) {
      return Fail(error, "Invalid full update ZIP metadata");
    }
    // ZIP host IDs 3 (Unix) and 19 (macOS) define the upper external
    // attributes as st_mode. DOS attributes must never be treated as chmod.
    const unsigned host = static_cast<unsigned>((info.version >> 8) & 0xff);
    const mode_t mode = static_cast<mode_t>(info.external_fa >> 16);
    if ((host != 3 && host != 19) ||
        (!S_ISREG(mode) && !S_ISDIR(mode) && !S_ISLNK(mode)) ||
        S_ISDIR(mode) != entry->is_directory ||
        S_ISLNK(mode) != entry->is_symbolic_link || (mode & 07000)) {
      return Fail(error,
                  "The full update ZIP must preserve Unix file types and "
                  "permissions without special permission bits");
    }
    permissions->push_back({entry->path, mode});
    metadata_status = unzGoToNextFile(metadata);
  }
  return (reader.ok() && metadata_status == UNZ_END_OF_LIST_OF_FILE) ||
         Fail(error, "Could not read all update ZIP permission metadata");
}

bool RestoreZipPermissions(const base::FilePath& staging,
                           std::vector<ZipPermissions> permissions,
                           std::string* error) {
  // Unzip restores executable bits only. Apply the complete permissions after
  // extraction, with files before directories and descendants before parents,
  // so read-only directories cannot block the remainder of extraction.
  std::ranges::sort(permissions, [](const auto& a, const auto& b) {
    if (S_ISDIR(a.mode) != S_ISDIR(b.mode)) {
      return !S_ISDIR(a.mode);
    }
    return a.path.value().size() > b.path.value().size();
  });
  base::ScopedFD root(open(staging.value().c_str(),
                           O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  if (!root.is_valid()) {
    return Fail(error, "Could not open the update staging directory");
  }
  for (const auto& entry : permissions) {
    if (S_ISLNK(entry.mode)) {
      continue;
    }
    // Open every component relative to a trusted descriptor. Never chmod a
    // symlink or follow one through an intermediate component.
    base::ScopedFD descriptor(dup(root.get()));
    const auto components = entry.path.GetComponents();
    for (size_t index = 0; index < components.size(); ++index) {
      int flags = O_RDONLY | O_NOFOLLOW | O_CLOEXEC;
      if (index + 1 != components.size() || S_ISDIR(entry.mode)) {
        flags |= O_DIRECTORY;
      }
      descriptor.reset(
          openat(descriptor.get(), components[index].c_str(), flags));
      if (!descriptor.is_valid()) {
        return Fail(error, "Could not safely open an extracted ZIP entry");
      }
    }
    struct stat info;
    if (components.empty() || fstat(descriptor.get(), &info) != 0 ||
        (info.st_mode & S_IFMT) != (entry.mode & S_IFMT) ||
        fchmod(descriptor.get(), entry.mode & 0777) != 0) {
      return Fail(error, "Could not restore the update ZIP permissions");
    }
  }
  return true;
}

void MakeDirectoriesWritable(int directory) {
  base::ScopedFD duplicate(dup(directory));
  DIR* entries = duplicate.is_valid() ? fdopendir(duplicate.get()) : nullptr;
  if (!entries) {
    return;
  }
  static_cast<void>(duplicate.release());
  while (const dirent* entry = readdir(entries)) {
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
      continue;
    }
    struct stat info;
    if (fstatat(directory, entry->d_name, &info, AT_SYMLINK_NOFOLLOW) != 0 ||
        !S_ISDIR(info.st_mode)) {
      continue;
    }
    // A failed package may already contain 0500/0000 directories. Restore
    // owner access before opening them, without following symbolic links.
    if (fchmodat(directory, entry->d_name, (info.st_mode & 0777) | 0700,
                 AT_SYMLINK_NOFOLLOW) != 0) {
      continue;
    }
    base::ScopedFD child(
        openat(directory, entry->d_name,
               O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (child.is_valid()) {
      MakeDirectoriesWritable(child.get());
    }
  }
  closedir(entries);
}

void PrepareFailedStagingForRemoval(const base::FilePath& staging) {
  base::ScopedFD directory(
      open(staging.value().c_str(),
           O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  if (directory.is_valid()) {
    MakeDirectoriesWritable(directory.get());
  }
}

base::FilePath FindPackagedBundle(const base::FilePath& directory,
                                  const base::FilePath& target_name,
                                  std::string* error) {
  const base::FilePath exact = directory.Append(target_name);
  if (base::PathExists(exact) || base::IsLink(exact)) {
    if (base::DirectoryExists(exact) && !base::IsLink(exact)) {
      return exact;
    }
    Fail(error, "The packaged application must be a real directory");
    return {};
  }
  // Renaming an installed app does not change its code identity. If the
  // original product name is still used in the package, accept it only when
  // there is a single unambiguous top-level app; signature checks follow.
  base::FileEnumerator applications(
      directory, false,
      base::FileEnumerator::DIRECTORIES | base::FileEnumerator::SHOW_SYM_LINKS,
      base::FilePath::StringType(),
      base::FileEnumerator::FolderSearchPolicy::ALL,
      base::FileEnumerator::ErrorPolicy::STOP_ENUMERATION);
  base::FilePath candidate;
  for (base::FilePath path = applications.Next(); !path.empty();
       path = applications.Next()) {
    if (path.MatchesExtension(".app") &&
        S_ISDIR(applications.GetInfo().stat().st_mode)) {
      if (!candidate.empty()) {
        Fail(error, "The update package contains multiple application bundles");
        return {};
      }
      candidate = path;
    }
  }
  if (applications.GetError() != base::File::FILE_OK || candidate.empty()) {
    Fail(error, "The update package does not contain an application bundle");
    return {};
  }
  return candidate;
}

// Keep the lock outside the sealed bundle. Do not unlink it on release: doing
// so could let two processes lock different inodes for the same installation.
class InstallationLock {
 public:
  bool Acquire(const base::FilePath& target, std::string* error) {
    const base::FilePath path = target.DirName().Append(
        "." + target.BaseName().value() + ".xenon-update.lock");
    constexpr int flags = O_RDWR | O_CLOEXEC | O_NOFOLLOW;
    fd_.reset(open(path.value().c_str(), flags | O_CREAT | O_EXCL, 0666));
    if (fd_.is_valid()) {
      // Users with permission to update the same shared installation must use
      // the same inode. Only this newly created empty lock is made writable.
      if (fchmod(fd_.get(), 0666) != 0) {
        return Fail(error,
                    "Could not set permissions on the installation lock");
      }
    } else if (errno == EEXIST) {
      fd_.reset(open(path.value().c_str(), flags));
    }
    struct stat info;
    if (!fd_.is_valid() || fstat(fd_.get(), &info) != 0 ||
        !S_ISREG(info.st_mode) || info.st_nlink != 1) {
      return Fail(error,
                  "Cannot safely open the installation lock; write permission "
                  "to the application parent directory is required");
    }
    if (flock(fd_.get(), LOCK_EX | LOCK_NB) != 0) {
      return Fail(error, "Another process is updating this application, or the "
                         "installation lock is unavailable");
    }
    struct stat path_info;
    if (lstat(path.value().c_str(), &path_info) != 0 ||
        info.st_dev != path_info.st_dev || info.st_ino != path_info.st_ino) {
      return Fail(error, "The installation lock changed while acquiring it");
    }
    return true;
  }

 private:
  base::ScopedFD fd_;
};

// An interrupted rsync can leave a relative link whose destination has not
// arrived yet. Resolve existing links without requiring the final target to
// exist, while keeping every step inside the application.
bool IsInternalLink(const base::FilePath& root, const base::FilePath& link) {
  base::FilePath relative;
  if (!root.AppendRelativePath(link, &relative)) {
    return false;
  }
  const auto components = relative.GetComponents();
  std::deque<std::string> pending(components.begin(), components.end());
  base::FilePath resolved = root;
  size_t links_followed = 0;
  while (!pending.empty()) {
    std::string component = std::move(pending.front());
    pending.pop_front();
    if (component == ".") {
      continue;
    }
    if (component == "..") {
      if (resolved == root) {
        return false;
      }
      resolved = resolved.DirName();
      continue;
    }
    const base::FilePath next = resolved.Append(component);
    struct stat info;
    const int status = lstat(next.value().c_str(), &info);
    if (status != 0 && errno != ENOENT && errno != ENOTDIR) {
      return false;
    }
    if (status == 0 && S_ISLNK(info.st_mode)) {
      base::FilePath target;
      if (++links_followed > 40 || !base::ReadSymbolicLink(next, &target) ||
          target.empty() || target.IsAbsolute()) {
        return false;
      }
      const auto target_components = target.GetComponents();
      pending.insert(pending.begin(), target_components.begin(),
                     target_components.end());
    } else {
      resolved = next;
    }
  }
  return resolved != root && root.IsParent(resolved);
}

// Enumeration never follows directory symlinks. Only an authenticated repair
// attempt can tolerate internal links left dangling by an interrupted copy.
bool ValidateBundleTree(const base::FilePath& bundle,
                        std::string* error,
                        bool allow_internal_dangling_links = false) {
  const base::FilePath root = base::MakeAbsoluteFilePath(bundle);
  if (root.empty() || base::IsLink(bundle) || !base::DirectoryExists(root)) {
    return Fail(error, "The application bundle must be a real directory");
  }
  base::FileEnumerator files(
      root, true,
      base::FileEnumerator::FILES | base::FileEnumerator::DIRECTORIES |
          base::FileEnumerator::SHOW_SYM_LINKS,
      base::FilePath::StringType(),
      base::FileEnumerator::FolderSearchPolicy::ALL,
      base::FileEnumerator::ErrorPolicy::STOP_ENUMERATION);
  for (base::FilePath path = files.Next(); !path.empty(); path = files.Next()) {
    const mode_t mode = files.GetInfo().stat().st_mode;
    if (S_ISLNK(mode)) {
      const base::FilePath destination = base::MakeAbsoluteFilePath(path);
      if ((destination.empty() &&
           !(allow_internal_dangling_links && IsInternalLink(root, path))) ||
          (!destination.empty() && !root.IsParent(destination))) {
        return Fail(error,
                    "Bundle contains an external or broken symbolic link");
      }
    } else if (!S_ISREG(mode) && !S_ISDIR(mode)) {
      return Fail(error, "Bundle contains a special file");
    }
  }
  return files.GetError() == base::File::FILE_OK ||
         Fail(error, "Could not enumerate the complete application bundle");
}

std::string PlistString(const base::FilePath& path, NSString* key) {
  NSDictionary* plist = [NSDictionary
      dictionaryWithContentsOfURL:base::apple::FilePathToNSURL(path)
                            error:nil];
  NSString* value = base::apple::ObjCCast<NSString>(plist[key]);
  return value ? base::SysNSStringToUTF8(value) : std::string();
}

base::FilePath InfoPlist(const base::FilePath& bundle) {
  return bundle.AppendASCII("Contents/Info.plist");
}

struct SignedBundle {
  base::apple::ScopedCFTypeRef<SecStaticCodeRef> code;
  std::string identifier;
  std::string team;
};

bool ReadSignedBundle(const base::FilePath& bundle,
                      SignedBundle* result,
                      std::string* error) {
  base::apple::ScopedCFTypeRef<SecRequirementRef> developer_id;
  // Developer ID Application, not ad-hoc, self-signed or development signing.
  // This is the Developer ID requirement from Apple's TN3127 and base/mac.
  const CFStringRef developer_id_requirement =
      CFSTR("anchor apple generic and certificate "
            "1[field.1.2.840.113635.100.6.2.6] exists and certificate "
            "leaf[field.1.2.840.113635.100.6.1.13] exists");
  if (SecStaticCodeCreateWithPath(
          base::apple::FilePathToCFURL(bundle).get(), kSecCSDefaultFlags,
          result->code.InitializeInto()) != errSecSuccess ||
      SecRequirementCreateWithString(
          developer_id_requirement, kSecCSDefaultFlags,
          developer_id.InitializeInto()) != errSecSuccess ||
      SecStaticCodeCheckValidity(result->code.get(),
                                 kSecCSCheckAllArchitectures |
                                     kSecCSCheckNestedCode |
                                     kSecCSStrictValidate,
                                 developer_id.get()) != errSecSuccess) {
    return Fail(error,
                "Application does not have a valid Developer ID signature: " +
                    bundle.value());
  }
  base::apple::ScopedCFTypeRef<CFDictionaryRef> info;
  if (SecCodeCopySigningInformation(result->code.get(),
                                    kSecCSSigningInformation,
                                    info.InitializeInto()) != errSecSuccess) {
    return Fail(error, "Could not read the application signing identity");
  }
  CFNumberRef flags = base::apple::GetValueFromDictionary<CFNumberRef>(
      info.get(), kSecCodeInfoFlags);
  CFStringRef identifier = base::apple::GetValueFromDictionary<CFStringRef>(
      info.get(), kSecCodeInfoIdentifier);
  CFStringRef team = base::apple::GetValueFromDictionary<CFStringRef>(
      info.get(), kSecCodeInfoTeamIdentifier);
  long long flags_value = 0;
  if (!flags || !identifier || !team ||
      !CFNumberGetValue(flags, kCFNumberLongLongType, &flags_value) ||
      (flags_value & kSecCodeSignatureAdhoc)) {
    return Fail(error,
                "An authenticated signing identity and Team ID are required");
  }
  result->identifier = base::SysCFStringRefToUTF8(identifier);
  result->team = base::SysCFStringRefToUTF8(team);
  if (result->identifier.empty() || result->team.empty() ||
      result->identifier !=
          PlistString(InfoPlist(bundle), @"CFBundleIdentifier")) {
    return Fail(error, "Bundle identifier does not match its signing identity");
  }
  return true;
}

// The native installer mutates directories in place. Reject an unwritable
// installation before running it; never attempt automatic elevation.
bool CheckWritableDirectories(const base::FilePath& bundle,
                              const base::FilePath& versions,
                              const std::string& target_version,
                              std::string* error) {
  const base::FilePath versions_dir = bundle.Append(versions);
  std::vector<base::FilePath> pending{bundle};
  while (!pending.empty()) {
    base::FilePath path = std::move(pending.back());
    pending.pop_back();
    if (access(path.value().c_str(), W_OK | X_OK) != 0) {
      return Fail(error,
                  "Installation requires write permission to " + path.value());
    }
    base::FileEnumerator dirs(
        path, false,
        base::FileEnumerator::DIRECTORIES |
            base::FileEnumerator::SHOW_SYM_LINKS,
        base::FilePath::StringType(),
        base::FileEnumerator::FolderSearchPolicy::ALL,
        base::FileEnumerator::ErrorPolicy::STOP_ENUMERATION);
    for (base::FilePath child = dirs.Next(); !child.empty();
         child = dirs.Next()) {
      if (base::IsLink(child) || (path == versions_dir &&
                                  child.BaseName().value() != target_version)) {
        continue;
      }
      pending.push_back(std::move(child));
    }
    if (dirs.GetError() != base::File::FILE_OK) {
      return Fail(error, "Could not check installation directory permissions");
    }
  }
  return true;
}

class MountedDmg {
 public:
  ~MountedDmg() {
    if (attached_ && !Detach(nullptr)) {
      // Never recursively remove a path that still contains a mounted volume.
      LOG(ERROR) << "Leaving mounted update image at " << directory_.Take();
    }
  }

  bool Attach(const base::FilePath& image, std::string* error) {
    if (!directory_.CreateUniqueTempDir()) {
      return Fail(error,
                  "Could not create a private disk image mount directory");
    }
    mount_path_ = directory_.GetPath().AppendASCII("volume");
    if (!base::CreateDirectory(mount_path_)) {
      return Fail(error, "Could not create the disk image mount point");
    }
    base::CommandLine command(base::FilePath("/usr/bin/hdiutil"));
    command.AppendArg("attach");
    command.AppendArgPath(image);
    command.AppendArg("-readonly");
    command.AppendArg("-nobrowse");
    command.AppendArg("-noautoopen");
    command.AppendArg("-mountpoint");
    command.AppendArgPath(mount_path_);
    const bool success =
        RunCommand(command, "Read-only disk image mount", error);
    struct stat mount_info;
    struct stat parent_info;
    attached_ =
        success ||
        (stat(mount_path_.value().c_str(), &mount_info) == 0 &&
         stat(directory_.GetPath().value().c_str(), &parent_info) == 0 &&
         mount_info.st_dev != parent_info.st_dev);
    return success;
  }

  bool Detach(std::string* error) {
    if (!attached_) {
      return true;
    }
    base::CommandLine command(base::FilePath("/usr/bin/hdiutil"));
    command.AppendArg("detach");
    command.AppendArgPath(mount_path_);
    std::string output;
    if (!base::GetAppOutput(command, &output)) {
      command.AppendArg("-force");
      if (!RunCommand(command, "Disk image detach", error)) {
        return false;
      }
    }
    attached_ = false;
    return true;
  }

  const base::FilePath& path() const { return mount_path_; }

 private:
  base::ScopedTempDir directory_;
  base::FilePath mount_path_;
  bool attached_ = false;
};

}  // namespace

struct MacUpdateInstallContext::State {
  base::FilePath target;
  dev_t device = 0;
  ino_t inode = 0;
  std::string identifier;
  std::string team;
  std::string original_version;
  std::string last_attempt_version;
  std::string product;
  base::apple::ScopedCFTypeRef<SecRequirementRef> requirement;
};

MacUpdateInstallContext::MacUpdateInstallContext() = default;
MacUpdateInstallContext::~MacUpdateInstallContext() = default;

std::string XenonUpdateInstaller::ReadBundleShortVersion(
    const base::FilePath& bundle_path) {
  if (bundle_path.empty()) {
    return {};
  }
  @autoreleasepool {
    const base::FilePath plist_path = InfoPlist(bundle_path);
    std::string version =
        PlistString(plist_path, @"CFBundleShortVersionString");
    if (version.empty()) {
      version = PlistString(plist_path, @"CFBundleVersion");
    }
    return version;
  }
}

base::FilePath XenonUpdateInstaller::PrepareMacFullUpdate(
    const base::FilePath& downloaded_file,
    const base::FilePath& staging_root,
    const base::FilePath& target_app_name,
    std::string* error_detail) {
  if (error_detail) {
    error_detail->clear();
  }
  if (!IsBasename(target_app_name) || target_app_name.Extension() != ".app" ||
      !staging_root.IsAbsolute() || !base::DirectoryExists(staging_root)) {
    Fail(error_detail, "Invalid application name or staging directory");
    return {};
  }
  base::ScopedTempDir staging;
  if (!staging.CreateUniqueTempDirUnderPath(staging_root, "mac_full_") ||
      !base::SetPosixFilePermissions(staging.GetPath(), 0700)) {
    Fail(error_detail, "Could not create a private update staging directory");
    return {};
  }
  // ScopedTempDir alone cannot remove a copied or restored read-only tree.
  // Run this before its destructor on every failure, including DMG failures.
  base::ScopedClosureRunner prepare_cleanup(
      base::BindOnce(&PrepareFailedStagingForRemoval, staging.GetPath()));
  const base::FilePath bundle = staging.GetPath().Append(target_app_name);
  bool is_zip = downloaded_file.MatchesExtension(".zip");
  bool is_dmg = downloaded_file.MatchesExtension(".dmg");
  if (!is_zip && !is_dmg) {
    // Download endpoints may have no extension; the manager saves those as
    // .bin. Identify the payload instead of making the URL dictate its type.
    zip::ZipReader reader;
    is_zip = reader.Open(downloaded_file);
    if (!is_zip) {
      base::CommandLine command(base::FilePath("/usr/bin/hdiutil"));
      command.AppendArg("imageinfo");
      command.AppendArgPath(downloaded_file);
      std::string output;
      int exit_code = -1;
      is_dmg = base::GetAppOutputWithExitCode(command, &output, &exit_code) &&
               exit_code == 0;
    }
  }
  if (is_zip) {
    std::vector<ZipPermissions> permissions;
    if (!ReadZipPermissions(downloaded_file, &permissions, error_detail)) {
      return {};
    }
    if (!zip::Unzip(downloaded_file, staging.GetPath(), {},
                    zip::UnzipSymlinkOption::PRESERVE)) {
      Fail(error_detail, "Could not safely extract the full update ZIP");
      return {};
    }
    if (!RestoreZipPermissions(staging.GetPath(), std::move(permissions),
                               error_detail)) {
      return {};
    }
    const base::FilePath source =
        FindPackagedBundle(staging.GetPath(), target_app_name, error_detail);
    if (source.empty() || (source != bundle && !base::Move(source, bundle))) {
      if (!source.empty()) {
        Fail(error_detail, "Could not preserve the installed application name");
      }
      return {};
    }
  } else if (is_dmg) {
    MountedDmg image;
    if (!image.Attach(downloaded_file, error_detail)) {
      return {};
    }
    const base::FilePath source =
        FindPackagedBundle(image.path(), target_app_name, error_detail);
    if (source.empty() || !ValidateBundleTree(source, error_detail)) {
      return {};
    }
    base::CommandLine command(base::FilePath("/usr/bin/ditto"));
    command.AppendArgPath(source);
    command.AppendArgPath(bundle);
    if (!RunCommand(command, "Disk image application copy", error_detail) ||
        !image.Detach(error_detail)) {
      return {};
    }
  } else {
    Fail(error_detail, "Unsupported full update format; expected ZIP or DMG");
    return {};
  }
  if (!ValidateBundleTree(bundle, error_detail)) {
    return {};
  }
  // Its bundle ID, signing identity and version are checked before installing.
  prepare_cleanup.ReplaceClosure(base::NullCallback());
  staging.Take();
  return bundle;
}

bool XenonUpdateInstaller::InstallMacUpdate(
    const base::FilePath& staged_bundle,
    const base::FilePath& target_install_dir,
    const std::string& target_version,
    std::string* error_detail,
    MacUpdateInstallContext* retry_context) {
  @autoreleasepool {
    if (error_detail) {
      error_detail->clear();
    }
    const base::Version version(target_version);
    if (!version.IsValid() ||
        !base::ContainsOnlyChars(target_version, "0123456789.")) {
      return Fail(error_detail, "Invalid target application version");
    }
    const base::FilePath staged = base::MakeAbsoluteFilePath(staged_bundle);
    const base::FilePath target =
        base::MakeAbsoluteFilePath(target_install_dir);
    if (staged.empty() || target.empty() || staged == target ||
        staged.IsParent(target) || target.IsParent(staged) ||
        staged.BaseName() != target.BaseName() ||
        target.Extension() != ".app") {
      return Fail(error_detail,
                  "Invalid or overlapping application bundle paths");
    }
    InstallationLock installation_lock;
    if (!installation_lock.Acquire(target, error_detail)) {
      return false;
    }
    MacUpdateInstallContext local_context;
    MacUpdateInstallContext& context =
        retry_context ? *retry_context : local_context;
    struct stat target_info;
    if (lstat(target.value().c_str(), &target_info) != 0 ||
        !S_ISDIR(target_info.st_mode)) {
      return Fail(error_detail, "Could not identify the installed application");
    }
    if (context.state_ && (context.state_->target != target ||
                           context.state_->device != target_info.st_dev ||
                           context.state_->inode != target_info.st_ino)) {
      return Fail(error_detail, "Installation changed since the failed update");
    }
    // Another browser process may have completed a newer update between our
    // attempts. The in-place installer retains the directory inode, so the
    // retained identity alone is not enough to prevent downgrading it.
    const base::Version disk_version(
        PlistString(InfoPlist(target), @"CFBundleShortVersionString"));
    if (disk_version.IsValid() && disk_version > version) {
      return Fail(error_detail,
                  "A newer application version is already installed");
    }
    if (!ValidateBundleTree(staged_bundle, error_detail) ||
        !ValidateBundleTree(target_install_dir, error_detail,
                            /*allow_internal_dangling_links=*/
                            context.state_ != nullptr)) {
      return false;
    }

    SignedBundle update;
    if (!ReadSignedBundle(staged, &update, error_detail)) {
      return false;
    }
    // Capture trust before the first mutation. A failed outer-app rsync can
    // break its resource seal or already update Info.plist; neither must block
    // retrying the same authenticated update in this browser session.
    std::unique_ptr<MacUpdateInstallContext::State> initial_state;
    if (!context.state_) {
      SignedBundle current;
      if (!ReadSignedBundle(target, &current, error_detail)) {
        return false;
      }
      initial_state = std::make_unique<MacUpdateInstallContext::State>();
      initial_state->target = target;
      initial_state->device = target_info.st_dev;
      initial_state->inode = target_info.st_ino;
      initial_state->identifier = current.identifier;
      initial_state->team = current.team;
      initial_state->original_version =
          PlistString(InfoPlist(target), @"CFBundleShortVersionString");
      initial_state->product =
          PlistString(InfoPlist(target), @"CFBundleDisplayName");
      if (SecCodeCopyDesignatedRequirement(
              current.code.get(), kSecCSDefaultFlags,
              initial_state->requirement.InitializeInto()) != errSecSuccess) {
        return Fail(error_detail,
                    "Could not retain the installed signing identity");
      }
    }
    MacUpdateInstallContext::State* trusted =
        context.state_ ? context.state_.get() : initial_state.get();
    if (trusted->identifier != update.identifier ||
        trusted->team != update.team ||
        SecStaticCodeCheckValidity(
            update.code.get(),
            kSecCSCheckAllArchitectures | kSecCSCheckNestedCode |
                kSecCSStrictValidate,
            trusted->requirement.get()) != errSecSuccess) {
      return Fail(
          error_detail,
          "Update signing identity differs from the installed application");
    }

    const std::string& current_version = trusted->original_version;
    const std::string& product = trusted->product;
    const base::FilePath executable = base::FilePath::FromUTF8Unsafe(
        PlistString(InfoPlist(staged), @"CFBundleExecutable"));
    if (!base::Version(current_version).IsValid() ||
        version.CompareTo(base::Version(current_version)) <= 0 ||
        (!trusted->last_attempt_version.empty() &&
         version < base::Version(trusted->last_attempt_version)) ||
        PlistString(InfoPlist(staged), @"CFBundleShortVersionString") !=
            target_version ||
        !IsBasename(base::FilePath::FromUTF8Unsafe(product)) ||
        PlistString(InfoPlist(target), @"CFBundleDisplayName") != product ||
        PlistString(InfoPlist(staged), @"CFBundleDisplayName") != product ||
        !IsBasename(executable) ||
        access(staged.AppendASCII("Contents/MacOS")
                   .Append(executable)
                   .value()
                   .c_str(),
               X_OK) != 0) {
      return Fail(error_detail, "Update version, product or executable does "
                                "not match the installed application");
    }
    const base::FilePath versions = base::FilePath("Contents/Frameworks")
                                        .Append(base::FilePath::FromUTF8Unsafe(
                                            product + " Framework.framework"))
                                        .AppendASCII("Versions");
    const base::FilePath staged_version =
        staged.Append(versions).AppendASCII(target_version);
    if (!base::DirectoryExists(
            target.Append(versions).AppendASCII(current_version)) ||
        !base::DirectoryExists(staged_version) ||
        base::IsLink(staged_version) ||
        PlistString(staged_version.AppendASCII("Resources/Info.plist"),
                    @"CFBundleShortVersionString") != target_version) {
      return Fail(
          error_detail,
          "Update does not contain the expected versioned Chromium framework");
    }
    if (!CheckWritableDirectories(target, versions, target_version,
                                  error_detail)) {
      return false;
    }

    base::ScopedTempDir script_dir;
    if (!script_dir.CreateUniqueTempDir()) {
      return Fail(error_detail,
                  "Could not create the private installer directory");
    }
    const base::FilePath script_path =
        script_dir.GetPath().AppendASCII("install.sh");
    std::string script(kMacInstallScript);
    base::ReplaceSubstringsAfterOffset(
        &script, 0, "\nUPDATE_VERSION=\n",
        "\nUPDATE_VERSION=" + target_version + "\n");
    if (!base::WriteFile(script_path, script) ||
        !base::SetPosixFilePermissions(script_path, 0600)) {
      return Fail(error_detail, "Could not write the trusted installer script");
    }
    // Paths are argv, never interpolated into shell source. The script is
    // compiled from Chromium source, not taken from the downloaded package.
    // Its native quarantine cleanup follows the signature checks above.
    base::CommandLine command(base::FilePath("/bin/bash"));
    command.AppendArg("-p");
    command.AppendArgPath(script_path);
    command.AppendArgPath(staged.DirName());
    command.AppendArgPath(target);
    command.AppendArg(current_version);
    trusted->last_attempt_version = target_version;
    if (initial_state) {
      context.state_ = std::move(initial_state);
    }
    if (!RunCommand(command, "Chromium versioned application installation",
                    error_detail)) {
      return false;
    }
    SignedBundle installed;
    if (!ReadSignedBundle(target, &installed, error_detail) ||
        installed.identifier != update.identifier ||
        installed.team != update.team ||
        PlistString(InfoPlist(target), @"CFBundleShortVersionString") !=
            target_version) {
      return Fail(
          error_detail,
          "Installation completed but final bundle verification failed; "
          "the application requires repair");
    }
    context.state_.reset();
    return true;
  }
}

}  // namespace xenon::updater
