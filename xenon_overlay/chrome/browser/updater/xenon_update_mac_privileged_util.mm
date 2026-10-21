// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/updater/xenon_update_mac_privileged_util.h"

#import <Security/Security.h>
#include <copyfile.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <libproc.h>
#include <limits.h>
#include <stdio.h>
#include <sys/acl.h>
#include <sys/event.h>
#include <sys/file.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <vector>

#include "xenon_overlay/chrome/browser/updater/xenon_update_mac_privileged_protocol.h"

namespace {

class FileDescriptor {
 public:
  explicit FileDescriptor(int value) : value_(value) {}
  ~FileDescriptor() {
    if (value_ >= 0) {
      close(value_);
    }
  }
  int get() const { return value_; }

 private:
  int value_;
};

bool Fail(NSString** error, NSString* message) {
  if (error) {
    *error = message;
  }
  return false;
}

bool SystemFailure(NSString** error, NSString* message) {
  return Fail(error,
              [NSString stringWithFormat:@"%@: %s", message, strerror(errno)]);
}

bool SameFile(const struct stat& first, const struct stat& second) {
  return first.st_dev == second.st_dev && first.st_ino == second.st_ino;
}

NSDictionary* BundleInfo(NSString* path);

bool MutableInstallation(NSString* target, NSString** error) {
  for (NSString* path in
       @[ target.stringByDeletingLastPathComponent, target ]) {
    struct stat metadata;
    struct statfs volume;
    if (lstat(path.fileSystemRepresentation, &metadata) != 0 ||
        !S_ISDIR(metadata.st_mode) ||
        statfs(path.fileSystemRepresentation, &volume) != 0) {
      return Fail(
          error,
          @"The installed application must be a real accessible directory");
    }
    if (volume.f_flags & MNT_RDONLY) {
      return Fail(error, @"The application is on a read-only volume; elevation "
                         @"cannot update it");
    }
    if (metadata.st_flags &
        (UF_IMMUTABLE | SF_IMMUTABLE | UF_APPEND | SF_APPEND | SF_RESTRICTED)) {
      return Fail(error, @"The application or its parent is protected; its "
                         @"flags will not be changed");
    }
  }
  return true;
}

bool NoWritableACL(int descriptor) {
  acl_t acl = acl_get_fd_np(descriptor, ACL_TYPE_EXTENDED);
  // Darwin returns ENOENT for an existing open object with no extended ACL.
  if (!acl) {
    return errno == ENOTSUP || errno == ENOENT;
  }
  acl_entry_t entry;
  bool safe = true;
  int next = ACL_FIRST_ENTRY;
  while (acl_get_entry(acl, next, &entry) == 0) {
    next = ACL_NEXT_ENTRY;
    acl_tag_t tag;
    acl_permset_t permissions;
    if (acl_get_tag_type(entry, &tag) != 0 ||
        acl_get_permset(entry, &permissions) != 0) {
      safe = false;
      break;
    }
    if (tag != ACL_EXTENDED_ALLOW) {
      continue;
    }
    for (acl_perm_t permission :
         {ACL_WRITE_DATA, ACL_APPEND_DATA, ACL_DELETE, ACL_DELETE_CHILD,
          ACL_WRITE_ATTRIBUTES, ACL_WRITE_EXTATTRIBUTES, ACL_WRITE_SECURITY,
          ACL_CHANGE_OWNER}) {
      if (acl_get_perm_np(permissions, permission) != 0) {
        safe = false;
      }
    }
    if (!safe) {
      break;
    }
  }
  acl_free(acl);
  return safe;
}

bool ClearCopiedACL(int descriptor) {
  acl_t empty = acl_init(0);
  if (!empty) {
    return false;
  }
  const bool success = acl_set_fd_np(descriptor, empty, ACL_TYPE_EXTENDED) == 0;
  acl_free(empty);
  return success;
}

int OpenStagingRoot(NSString* target,
                    bool testing,
                    uid_t owner,
                    NSString** path,
                    NSString** error) {
  NSString* parent = testing ? target.stringByDeletingLastPathComponent
                             : @"/Library/Application Support";
  FileDescriptor library_fd(
      testing
          ? -1
          : open("/Library", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  if (!testing) {
    struct stat library;
    if (library_fd.get() < 0 || fstat(library_fd.get(), &library) != 0 ||
        !S_ISDIR(library.st_mode) || library.st_uid != 0 ||
        (library.st_mode & 0022) || !NoWritableACL(library_fd.get())) {
      Fail(error, @"The root update staging parent is not protected");
      return -1;
    }
  }
  FileDescriptor parent_fd(
      testing ? open(parent.fileSystemRepresentation,
                     O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
              : openat(library_fd.get(), "Application Support",
                       O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  struct stat metadata;
  if (parent_fd.get() < 0 || fstat(parent_fd.get(), &metadata) != 0 ||
      metadata.st_uid != owner || (metadata.st_mode & 0022) ||
      !NoWritableACL(parent_fd.get())) {
    Fail(error,
         @"The root update staging parent is writable by another identity");
    return -1;
  }
  const char* name = testing ? "PrivateStaging" : "XenonUpdater";
  if (mkdirat(parent_fd.get(), name, 0700) != 0 && errno != EEXIST) {
    SystemFailure(error, @"Could not create the root update staging directory");
    return -1;
  }
  int descriptor = openat(parent_fd.get(), name,
                          O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (descriptor < 0 || fstat(descriptor, &metadata) != 0 ||
      metadata.st_uid != owner || (metadata.st_mode & 0777) != 0700 ||
      !NoWritableACL(descriptor)) {
    if (descriptor >= 0) {
      close(descriptor);
    }
    Fail(error, @"The update staging directory must be private and owned by "
                @"its daemon");
    return -1;
  }
  *path = [parent
      stringByAppendingPathComponent:[NSString stringWithUTF8String:name]];
  return descriptor;
}

bool OtherBrowserInstancesExited(NSString* target, NSString** error) {
  NSString* executable = BundleInfo(target)[@"CFBundleExecutable"];
  if (![executable isKindOfClass:NSString.class] || executable.length == 0 ||
      [executable containsString:@"/"] || [executable isEqual:@"."] ||
      [executable isEqual:@".."]) {
    return Fail(error, @"Installed browser executable is invalid");
  }
  NSString* main_path =
      [[target stringByAppendingPathComponent:@"Contents/MacOS"]
          stringByAppendingPathComponent:executable];
  char* resolved = realpath(main_path.fileSystemRepresentation, nullptr);
  NSString* canonical =
      resolved ? [NSString stringWithUTF8String:resolved] : nil;
  free(resolved);
  if (!canonical) {
    return Fail(error, @"Could not resolve the installed browser executable");
  }
  int count = proc_listallpids(nullptr, 0);
  if (count <= 0 || count > 1000000) {
    return Fail(error, @"Could not inspect other running browser instances");
  }
  std::vector<pid_t> pids(static_cast<size_t>(count) + 1024);
  count = proc_listallpids(pids.data(),
                           static_cast<int>(pids.size() * sizeof(pid_t)));
  if (count <= 0 || static_cast<size_t>(count) >= pids.size()) {
    return Fail(error, @"Process list changed while checking running browsers; "
                       @"retry the update");
  }
  pids.resize(static_cast<size_t>(count));
  for (pid_t pid : pids) {
    if (pid <= 1 || pid == getpid()) {
      continue;
    }
    std::array<char, PROC_PIDPATHINFO_MAXSIZE> buffer;
    if (proc_pidpath(pid, buffer.data(),
                     static_cast<uint32_t>(buffer.size())) <= 0) {
      continue;  // The process may have exited, or may be a protected OS task.
    }
    if ([canonical isEqual:[NSString stringWithUTF8String:buffer.data()]]) {
      return Fail(error, @"Another instance of this browser is running; close "
                         @"all instances and retry");
    }
  }
  return true;
}

bool SafeIdentityComponent(NSString* value) {
  if (![value isKindOfClass:NSString.class] || value.length == 0 ||
      value.length > 255) {
    return false;
  }
  NSCharacterSet* allowed = [NSCharacterSet
      characterSetWithCharactersInString:
          @"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-"];
  return [value rangeOfCharacterFromSet:allowed.invertedSet].location ==
         NSNotFound;
}

bool VersionString(NSString* value) {
  if (![value isKindOfClass:NSString.class] || value.length == 0 ||
      value.length > 128) {
    return false;
  }
  NSArray<NSString*>* parts = [value componentsSeparatedByString:@"."];
  if (parts.count > 8) {
    return false;
  }
  NSCharacterSet* nondigits =
      [NSCharacterSet characterSetWithCharactersInString:@"0123456789"]
          .invertedSet;
  for (NSString* part in parts) {
    if (!part.length || part.length > 10 ||
        [part rangeOfCharacterFromSet:nondigits].location != NSNotFound) {
      return false;
    }
  }
  return true;
}

NSDictionary* BundleInfo(NSString* path) {
  return [NSDictionary
      dictionaryWithContentsOfFile:
          [path stringByAppendingPathComponent:@"Contents/Info.plist"]];
}

NSString* BundleVersion(NSString* path) {
  id value = BundleInfo(path)[@"CFBundleShortVersionString"];
  return VersionString(value) ? value : nil;
}

// Descriptor-relative traversal never follows a source symlink. A source file
// must be owned by the authenticated client, and only bytes/xattrs are copied.
// Ownership changes therefore operate exclusively on our new destination FDs.
bool CopyDirectory(int source,
                   int destination,
                   NSString* relative,
                   uid_t source_owner,
                   uid_t destination_owner,
                   gid_t destination_group,
                   NSMutableArray<NSString*>* symlinks,
                   unsigned depth,
                   NSString** error) {
  if (depth > 128) {
    return Fail(error, @"Update bundle nesting exceeds the supported limit");
  }
  DIR* entries = fdopendir(dup(source));
  if (!entries) {
    return SystemFailure(error, @"Could not enumerate update bundle");
  }
  bool success = true;
  for (;;) {
    errno = 0;
    dirent* entry = readdir(entries);
    if (!entry) {
      if (errno != 0) {
        success = SystemFailure(error, @"Could not read update directory");
      }
      break;
    }
    NSString* name = [NSString stringWithUTF8String:entry->d_name];
    if ([name isEqualToString:@"."] || [name isEqualToString:@".."]) {
      continue;
    }
    struct stat metadata;
    if (!name ||
        fstatat(source, entry->d_name, &metadata, AT_SYMLINK_NOFOLLOW) != 0 ||
        metadata.st_uid != source_owner || (metadata.st_mode & 07000)) {
      success = Fail(error, @"Update contains an unsafe owner or file mode");
      break;
    }
    NSString* child = [relative stringByAppendingPathComponent:name];
    if (S_ISLNK(metadata.st_mode)) {
      std::array<char, PATH_MAX> buffer;
      const ssize_t length =
          readlinkat(source, entry->d_name, buffer.data(), buffer.size());
      if (length <= 0 || static_cast<size_t>(length) >= buffer.size()) {
        success = Fail(error, @"Could not read update symbolic link");
        break;
      }
      NSString* link =
          [[NSString alloc] initWithBytes:buffer.data()
                                   length:static_cast<NSUInteger>(length)
                                 encoding:NSUTF8StringEncoding];
      NSString* resolved =
          [[relative stringByAppendingPathComponent:link ?: @""]
              stringByStandardizingPath];
      if (!link.length || link.isAbsolutePath ||
          [resolved isEqualToString:@".."] || [resolved hasPrefix:@"../"] ||
          symlinkat(link.fileSystemRepresentation, destination,
                    entry->d_name) != 0 ||
          fchownat(destination, entry->d_name, destination_owner,
                   destination_group, AT_SYMLINK_NOFOLLOW) != 0) {
        success = Fail(error, @"Update contains an unsafe symbolic link");
        break;
      }
      [symlinks addObject:child];
      continue;
    }
    if (!S_ISDIR(metadata.st_mode) && !S_ISREG(metadata.st_mode)) {
      success = Fail(error, @"Update contains a non-regular filesystem object");
      break;
    }
    const bool directory = S_ISDIR(metadata.st_mode);
    FileDescriptor input(openat(source, entry->d_name,
                                O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK |
                                    (directory ? O_DIRECTORY : 0)));
    struct stat opened;
    if (input.get() < 0 || fstat(input.get(), &opened) != 0 ||
        !SameFile(metadata, opened) || opened.st_uid != source_owner ||
        (opened.st_mode & 07000) || (!directory && opened.st_nlink != 1)) {
      success = Fail(error, @"Update file changed or contains a hard link");
      break;
    }
    if (directory && mkdirat(destination, entry->d_name, 0700) != 0) {
      success =
          SystemFailure(error, @"Could not create private update directory");
      break;
    }
    FileDescriptor output(openat(
        destination, entry->d_name,
        O_NOFOLLOW | O_CLOEXEC |
            (directory ? O_RDONLY | O_DIRECTORY : O_WRONLY | O_CREAT | O_EXCL),
        0600));
    if (output.get() < 0 || !ClearCopiedACL(output.get()) ||
        (directory
             ? !CopyDirectory(input.get(), output.get(), child, source_owner,
                              destination_owner, destination_group, symlinks,
                              depth + 1, error)
             : fcopyfile(input.get(), output.get(), nullptr,
                         COPYFILE_DATA | COPYFILE_XATTR) != 0) ||
        fchown(output.get(), destination_owner, destination_group) != 0 ||
        fchmod(output.get(), opened.st_mode & 0755) != 0 ||
        (!directory && fsync(output.get()) != 0)) {
      success =
          Fail(error, @"Could not copy update into the private staging area");
      break;
    }
  }
  closedir(entries);
  return success;
}

bool TrustedBackupTreeAt(int parent,
                         const char* name,
                         uid_t owner,
                         unsigned depth,
                         NSString** error) {
  struct stat metadata;
  if (depth > 128 ||
      fstatat(parent, name, &metadata, AT_SYMLINK_NOFOLLOW) != 0 ||
      metadata.st_uid != owner) {
    return Fail(
        error,
        @"Backup contains an untrusted owner; retaining it for recovery");
  }
  if (S_ISLNK(metadata.st_mode)) {
    return true;
  }
  if ((!S_ISDIR(metadata.st_mode) && !S_ISREG(metadata.st_mode)) ||
      (metadata.st_mode & 0022)) {
    return Fail(error, @"Backup contains writable or special files; retaining "
                       @"it for recovery");
  }
  FileDescriptor descriptor(
      openat(parent, name,
             O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK |
                 (S_ISDIR(metadata.st_mode) ? O_DIRECTORY : 0)));
  struct stat opened;
  if (descriptor.get() < 0 || fstat(descriptor.get(), &opened) != 0 ||
      !SameFile(metadata, opened) || !NoWritableACL(descriptor.get())) {
    return Fail(error, @"Backup permissions could permit outside changes; "
                       @"retaining it for recovery");
  }
  if (!S_ISDIR(metadata.st_mode)) {
    return true;
  }
  DIR* entries = fdopendir(dup(descriptor.get()));
  if (!entries) {
    return SystemFailure(error, @"Could not inspect update backup");
  }
  bool success = true;
  for (;;) {
    errno = 0;
    dirent* entry = readdir(entries);
    if (!entry) {
      if (errno) {
        success = SystemFailure(error, @"Could not inspect update backup");
      }
      break;
    }
    NSString* child = [NSString stringWithUTF8String:entry->d_name];
    if ([child isEqual:@"."] || [child isEqual:@".."]) {
      continue;
    }
    if (!child || !TrustedBackupTreeAt(descriptor.get(), entry->d_name, owner,
                                       depth + 1, error)) {
      success = false;
      break;
    }
  }
  closedir(entries);
  return success;
}

// Used only for our verified private directory or an app inode recorded by the
// transaction. Child symlinks are unlinked, never followed during cleanup.
bool RemoveTreeAt(int parent,
                  const char* name,
                  const struct stat* expected,
                  NSString** error) {
  struct stat metadata;
  if (fstatat(parent, name, &metadata, AT_SYMLINK_NOFOLLOW) != 0) {
    return errno == ENOENT ? true
                           : SystemFailure(error, @"Could not inspect backup");
  }
  if (expected && !SameFile(*expected, metadata)) {
    return Fail(error, @"Backup path changed; leaving it untouched");
  }
  if (!S_ISDIR(metadata.st_mode)) {
    return unlinkat(parent, name, 0) == 0 ||
           SystemFailure(error, @"Could not remove temporary update file");
  }
  FileDescriptor directory(
      openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  struct stat opened;
  if (directory.get() < 0 || fstat(directory.get(), &opened) != 0 ||
      !SameFile(metadata, opened)) {
    return Fail(error, @"Backup directory changed; leaving it untouched");
  }
  // The original installed bundle is never chmod'ed. Only discarded private
  // copies may need owner write access in unprivileged fixture tests.
  DIR* entries = fdopendir(dup(directory.get()));
  if (!entries) {
    return SystemFailure(error, @"Could not enumerate temporary update files");
  }
  bool success = true;
  for (dirent* entry = readdir(entries); entry; entry = readdir(entries)) {
    NSString* child = [NSString stringWithUTF8String:entry->d_name];
    if ([child isEqual:@"."] || [child isEqual:@".."]) {
      continue;
    }
    if (!RemoveTreeAt(directory.get(), entry->d_name, nullptr, error)) {
      success = false;
      break;
    }
  }
  closedir(entries);
  return success && (unlinkat(parent, name, AT_REMOVEDIR) == 0 ||
                     SystemFailure(error, @"Could not remove update backup"));
}

bool ValidateSymlinks(NSString* path,
                      NSArray<NSString*>* links,
                      NSString** error) {
  char* root = realpath(path.fileSystemRepresentation, nullptr);
  NSString* canonical = root ? [NSString stringWithUTF8String:root] : nil;
  free(root);
  if (!canonical) {
    return Fail(error, @"Could not resolve private update directory");
  }
  NSString* prefix = [canonical stringByAppendingString:@"/"];
  for (NSString* relative in links) {
    NSString* full_path = [path stringByAppendingPathComponent:relative];
    char* resolved = realpath(full_path.fileSystemRepresentation, nullptr);
    NSString* destination =
        resolved ? [NSString stringWithUTF8String:resolved] : nil;
    free(resolved);
    if (!destination || ![destination hasPrefix:prefix]) {
      return Fail(error,
                  @"Update symbolic link escapes the application or is broken");
    }
  }
  return true;
}

}  // namespace

namespace xenon::updater {

NSString* PrivilegedPeerRequirement(NSString* team,
                                    NSArray<NSString*>* identifiers,
                                    NSString** error) {
  if (!SafeIdentityComponent(team) || identifiers.count == 0) {
    Fail(error, @"A trusted signing team and identifiers are required");
    return nil;
  }
  NSMutableArray<NSString*>* alternatives = [NSMutableArray array];
  for (NSString* identifier in identifiers) {
    if (!SafeIdentityComponent(identifier)) {
      Fail(error, @"A code signing identifier is invalid");
      return nil;
    }
    [alternatives
        addObject:[NSString stringWithFormat:@"identifier \"%@\"", identifier]];
  }
  NSString* requirement = [NSString
      stringWithFormat:@"anchor apple generic and certificate leaf[subject.OU] "
                       @"= \"%@\" and (%@)",
                       team, [alternatives componentsJoinedByString:@" or "]];
  SecRequirementRef parsed = nullptr;
  const OSStatus status = SecRequirementCreateWithString(
      (__bridge CFStringRef)requirement, kSecCSDefaultFlags, &parsed);
  if (status != errSecSuccess) {
    Fail(error, @"Could not construct the XPC code signing requirement");
    return nil;
  }
  CFRelease(parsed);
  return requirement;
}

NSDictionary* ReadTrustedCodeIdentity(NSString* path, NSString** error) {
  SecStaticCodeRef code = nullptr;
  SecRequirementRef apple_requirement = nullptr;
  CFDictionaryRef information = nullptr;
  OSStatus status = SecStaticCodeCreateWithPath(
      (__bridge CFURLRef)[NSURL fileURLWithPath:path], kSecCSDefaultFlags,
      &code);
  if (status == errSecSuccess) {
    status = SecRequirementCreateWithString(
        CFSTR("anchor apple generic"), kSecCSDefaultFlags, &apple_requirement);
  }
  if (status == errSecSuccess) {
    status = SecStaticCodeCheckValidity(code,
                                        kSecCSCheckAllArchitectures |
                                            kSecCSCheckNestedCode |
                                            kSecCSStrictValidate,
                                        apple_requirement);
  }
  if (status == errSecSuccess) {
    status = SecCodeCopySigningInformation(code, kSecCSSigningInformation,
                                           &information);
  }
  NSDictionary* details = CFBridgingRelease(information);
  NSString* team = details[(__bridge NSString*)kSecCodeInfoTeamIdentifier];
  NSString* identifier = details[(__bridge NSString*)kSecCodeInfoIdentifier];
  NSNumber* flags = details[(__bridge NSString*)kSecCodeInfoFlags];
  if (code) {
    CFRelease(code);
  }
  if (apple_requirement) {
    CFRelease(apple_requirement);
  }
  if (status != errSecSuccess || !SafeIdentityComponent(team) ||
      !SafeIdentityComponent(identifier) ||
      ![flags isKindOfClass:NSNumber.class] ||
      (flags.unsignedIntValue & kSecCodeSignatureAdhoc)) {
    Fail(
        error,
        [NSString
            stringWithFormat:@"Privileged updating requires valid Apple-issued "
                             @"signatures and a signing team (%d)",
                             static_cast<int>(status)]);
    return nil;
  }
  return @{@"team_id" : team, @"identifier" : identifier};
}

NSString* PrivilegedTargetFromExecutable(NSString* executable,
                                         NSString** error) {
  NSString* canonical = executable.stringByStandardizingPath;
  if (!executable.isAbsolutePath || ![executable isEqual:canonical] ||
      ![executable.lastPathComponent isEqual:kPrivilegedUpdateHelperName]) {
    Fail(error,
         @"Privileged helper is not in its expected application location");
    return nil;
  }
  NSString* helpers = executable.stringByDeletingLastPathComponent;
  NSString* contents = helpers.stringByDeletingLastPathComponent;
  NSString* target = contents.stringByDeletingLastPathComponent;
  if (![helpers.lastPathComponent isEqual:@"Helpers"] ||
      ![contents.lastPathComponent isEqual:@"Contents"] ||
      ![target.pathExtension isEqual:@"app"] ||
      ![target.stringByDeletingLastPathComponent isEqual:@"/Applications"]) {
    Fail(error,
         @"Privileged updating supports only its own /Applications bundle");
    return nil;
  }
  for (NSString* path in
       @[ @"/Applications", target, contents, helpers, executable ]) {
    struct stat metadata;
    if (lstat(path.fileSystemRepresentation, &metadata) != 0 ||
        metadata.st_uid != 0 || (metadata.st_mode & S_IWOTH) ||
        ([path isEqual:executable] ? !S_ISREG(metadata.st_mode)
                                   : !S_ISDIR(metadata.st_mode)) ||
        (![path isEqual:@"/Applications"] && (metadata.st_mode & S_IWGRP))) {
      Fail(error,
           @"Privileged helper and its app must be root-owned real paths");
      return nil;
    }
    if (![path isEqual:@"/Applications"]) {
      FileDescriptor descriptor(
          open(path.fileSystemRepresentation,
               O_RDONLY | O_NOFOLLOW | O_CLOEXEC |
                   ([path isEqual:executable] ? 0 : O_DIRECTORY)));
      if (descriptor.get() < 0 || !NoWritableACL(descriptor.get())) {
        Fail(error, @"Privileged application paths must not grant write access "
                    @"through ACLs");
        return nil;
      }
    }
  }
  return target;
}

bool CanUsePrivilegedUpdateForTarget(NSString* target, NSString** error) {
  NSString* daemon =
      [target stringByAppendingPathComponent:
                  @"Contents/Helpers/xenon_privileged_update_helper"];
  if (![PrivilegedTargetFromExecutable(daemon, error) isEqual:target] ||
      !MutableInstallation(target, error)) {
    return false;
  }
  NSDictionary* main_identity = ReadTrustedCodeIdentity(target, error);
  if (!main_identity) {
    return false;
  }
  NSDictionary* daemon_identity = ReadTrustedCodeIdentity(daemon, error);
  if (!daemon_identity) {
    return false;
  }
  NSDictionary* helper_identity = ReadTrustedCodeIdentity(
      [target stringByAppendingPathComponent:
                  @"Contents/Helpers/xenon_update_helper"],
      error);
  if (!helper_identity) {
    return false;
  }
  NSString* identifier = main_identity[@"identifier"];
  return (main_identity && daemon_identity && helper_identity &&
          [identifier isEqual:BundleInfo(target)[@"CFBundleIdentifier"]] &&
          [main_identity[@"team_id"] isEqual:daemon_identity[@"team_id"]] &&
          [main_identity[@"team_id"] isEqual:helper_identity[@"team_id"]] &&
          [daemon_identity[@"identifier"]
              isEqual:[identifier stringByAppendingString:
                                      @".xenon-privileged-update-helper"]] &&
          [helper_identity[@"identifier"]
              isEqual:[identifier
                          stringByAppendingString:@".xenon-update-helper"]]) ||
         Fail(error, @"The application and both update helpers must have "
                     @"matching trusted identities");
}

NSXPCConnection* CreatePrivilegedUpdateConnection(NSString* daemon_path,
                                                  NSString** error) {
  NSString* target = PrivilegedTargetFromExecutable(daemon_path, error);
  if (!target || !CanUsePrivilegedUpdateForTarget(target, error)) {
    return nil;
  }
  NSDictionary* identity = ReadTrustedCodeIdentity(daemon_path, error);
  NSString* requirement =
      identity ? PrivilegedPeerRequirement(identity[@"team_id"],
                                           @[ identity[@"identifier"] ], error)
               : nil;
  if (!requirement) {
    return nil;
  }
  NSXPCConnection* connection = [[NSXPCConnection alloc]
      initWithMachServiceName:kPrivilegedUpdateService
                      options:NSXPCConnectionPrivileged];
  connection.remoteObjectInterface = [NSXPCInterface
      interfaceWithProtocol:@protocol(XenonPrivilegedUpdateProtocol)];
  [connection setCodeSigningRequirement:requirement];
  return connection;
}

}  // namespace xenon::updater

@implementation XenonPrivilegedUpdateJob {
  NSString* _target;
  NSString* _token;
  NSString* _state;
  NSString* _baseVersion;
  NSString* _targetVersion;
  NSString* _privateName;
  NSString* _stagingRoot;
  NSString* _preparedPath;
  NSDictionary* _identity;
  uid_t _owner;
  gid_t _group;
  uid_t _callerUID;
  int _parentFD;
  int _stagingFD;
  int _privateFD;
  int _lockFD;
  int _watchFD;
  BOOL _testing;
  BOOL _exited;
  BOOL _claimed;
  BOOL _conflict;
  NSTimeInterval _lastActivity;
  struct stat _original;
  struct stat _incoming;
  struct stat _privateMetadata;
}

- (instancetype)initWithTarget:(NSString*)target error:(NSString**)error {
  self = [super init];
  if (self) {
    _parentFD = _stagingFD = _privateFD = _lockFD = _watchFD = -1;
    _owner = _group = 0;
    _target = [target copy];
    if (geteuid() != 0 ||
        !xenon::updater::CanUsePrivilegedUpdateForTarget(target, error)) {
      Fail(error, @"Privileged installation requires an approved root daemon "
                  @"and trusted app");
      return nil;
    }
    _identity = xenon::updater::ReadTrustedCodeIdentity(target, error);
  }
  return self;
}

#if defined(XENON_PRIVILEGED_UPDATE_TESTING)
- (instancetype)initForTestingWithTarget:(NSString*)target
                                   error:(NSString**)error {
  self = [super init];
  if (self) {
    _parentFD = _stagingFD = _privateFD = _lockFD = _watchFD = -1;
    _owner = geteuid();
    _group = getegid();
    _target = [target copy];
    _testing = YES;
    _identity =
        @{@"identifier" : BundleInfo(target)[@"CFBundleIdentifier"] ?: @""};
    if (!target.isAbsolutePath || !BundleVersion(target)) {
      Fail(error, @"Invalid test bundle");
      return nil;
    }
  }
  return self;
}

- (void)expireLeaseForTesting {
  _lastActivity = -3600;
}
#endif

- (void)dealloc {
  if (_watchFD >= 0) {
    close(_watchFD);
  }
  if (_lockFD >= 0) {
    close(_lockFD);
  }
  if (_privateFD >= 0) {
    close(_privateFD);
  }
  if (_stagingFD >= 0) {
    close(_stagingFD);
  }
  if (_parentFD >= 0) {
    close(_parentFD);
  }
}

- (BOOL)checkToken:(NSString*)token uid:(uid_t)uid error:(NSString**)error {
  return (_token && [token isKindOfClass:NSString.class] &&
          [_token isEqual:token] && uid == _callerUID) ||
         Fail(error, @"The update token does not belong to this caller");
}

- (BOOL)validateIncoming:(NSString*)path error:(NSString**)error {
  NSDictionary* info = BundleInfo(path);
  if (![_targetVersion isEqual:BundleVersion(path)] ||
      ![_identity[@"identifier"] isEqual:info[@"CFBundleIdentifier"]]) {
    return Fail(
        error, @"Prepared application identity or target version is incorrect");
  }
  if (_testing) {
    return YES;
  }
  NSDictionary* identity = xenon::updater::ReadTrustedCodeIdentity(path, error);
  if (!identity) {
    return NO;
  }
  if (![identity isEqual:_identity]) {
    return Fail(error, @"Prepared application is not signed by the installed "
                       @"application's team");
  }
  for (NSString* name in
       @[ @"xenon_update_helper", @"xenon_privileged_update_helper" ]) {
    NSString* suffix = [name isEqual:@"xenon_update_helper"]
                           ? @".xenon-update-helper"
                           : @".xenon-privileged-update-helper";
    NSDictionary* helper = xenon::updater::ReadTrustedCodeIdentity(
        [[path stringByAppendingPathComponent:@"Contents/Helpers"]
            stringByAppendingPathComponent:name],
        error);
    if (!helper) {
      return NO;
    }
    if (![helper[@"team_id"] isEqual:_identity[@"team_id"]] ||
        ![helper[@"identifier"] isEqual:[_identity[@"identifier"]
                                            stringByAppendingString:suffix]]) {
      return Fail(error, @"Prepared update helpers do not match the trusted "
                         @"application identity");
    }
  }
  return YES;
}

- (void)releaseDescriptors {
  if (_watchFD >= 0) {
    close(_watchFD);
    _watchFD = -1;
  }
  if (_lockFD >= 0) {
    close(_lockFD);
    _lockFD = -1;
  }
  if (_privateFD >= 0) {
    close(_privateFD);
    _privateFD = -1;
  }
  if (_stagingFD >= 0) {
    close(_stagingFD);
    _stagingFD = -1;
  }
  if (_parentFD >= 0) {
    close(_parentFD);
    _parentFD = -1;
  }
}

- (NSString*)preparePath:(NSString*)source
             baseVersion:(NSString*)base_version
           targetVersion:(NSString*)target_version
               callerPID:(pid_t)caller_pid
               callerUID:(uid_t)caller_uid
                   error:(NSString**)error {
  if (_token || !VersionString(base_version) ||
      !VersionString(target_version) ||
      [target_version compare:base_version
                      options:NSNumericSearch] != NSOrderedDescending ||
      caller_pid <= 1 || caller_uid == 0 || !source.isAbsolutePath) {
    Fail(error, @"Invalid or concurrent privileged update request");
    return nil;
  }
  _callerUID = caller_uid;
  _baseVersion = [base_version copy];
  _targetVersion = [target_version copy];
  _exited = NO;
  _claimed = NO;
  _conflict = NO;
  _privateMetadata = {};
  if (!MutableInstallation(_target, error)) {
    return nil;
  }
  _parentFD =
      open(_target.stringByDeletingLastPathComponent.fileSystemRepresentation,
           O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  NSString* staging_root = nil;
  _stagingFD = OpenStagingRoot(_target, _testing, _owner, &staging_root, error);
  _stagingRoot = staging_root;
  struct stat installation_volume;
  struct stat staging_volume;
  if (_parentFD < 0 || _stagingFD < 0 ||
      fstat(_parentFD, &installation_volume) != 0 ||
      fstat(_stagingFD, &staging_volume) != 0 ||
      installation_volume.st_dev != staging_volume.st_dev) {
    if (error && !*error) {
      Fail(error, @"Root staging and the application must be on the same "
                  @"writable volume");
    }
    [self releaseDescriptors];
    return nil;
  }
  NSString* lock_name = [_target.lastPathComponent
      stringByAppendingString:@".xenon-privileged-update.lock"];
  _lockFD =
      openat(_stagingFD, lock_name.fileSystemRepresentation,
             O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
  struct stat lock_metadata;
  if (_lockFD < 0 || fstat(_lockFD, &lock_metadata) != 0 ||
      !S_ISREG(lock_metadata.st_mode) || lock_metadata.st_uid != _owner ||
      lock_metadata.st_nlink != 1 || (lock_metadata.st_mode & 0777) != 0600 ||
      !NoWritableACL(_lockFD) || flock(_lockFD, LOCK_EX | LOCK_NB) != 0 ||
      fstatat(_parentFD, _target.lastPathComponent.fileSystemRepresentation,
              &_original, AT_SYMLINK_NOFOLLOW) != 0 ||
      !S_ISDIR(_original.st_mode) || _original.st_uid != _owner ||
      ![_baseVersion isEqual:BundleVersion(_target)]) {
    Fail(error,
         @"Another update is active or the installed application changed");
    [self releaseDescriptors];
    return nil;
  }
  _watchFD = kqueue();
  struct kevent watch;
  EV_SET(&watch, static_cast<uintptr_t>(caller_pid), EVFILT_PROC,
         EV_ADD | EV_ENABLE | EV_ONESHOT, NOTE_EXIT, 0, nullptr);
  if (_watchFD < 0 || kevent(_watchFD, &watch, 1, nullptr, 0, nullptr) != 0) {
    Fail(error, @"Could not watch the authenticated browser process");
    [self releaseDescriptors];
    return nil;
  }
  NSString* uuid = NSUUID.UUID.UUIDString;
  _privateName = [@".xenon-privileged-" stringByAppendingString:uuid];
  if (mkdirat(_stagingFD, _privateName.fileSystemRepresentation, 0700) != 0) {
    SystemFailure(error, @"Could not create root private update staging");
    [self releaseDescriptors];
    return nil;
  }
  _privateFD = openat(_stagingFD, _privateName.fileSystemRepresentation,
                      O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  bool success =
      _privateFD >= 0 && fstat(_privateFD, &_privateMetadata) == 0 &&
      _privateMetadata.st_uid == _owner &&
      (_privateMetadata.st_mode & 0777) == 0700 && ClearCopiedACL(_privateFD) &&
      mkdirat(_privateFD, _target.lastPathComponent.fileSystemRepresentation,
              0700) == 0;
  _preparedPath = [[_stagingRoot stringByAppendingPathComponent:_privateName]
      stringByAppendingPathComponent:_target.lastPathComponent];
  FileDescriptor input(open(source.fileSystemRepresentation,
                            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  FileDescriptor output(
      success ? openat(_privateFD,
                       _target.lastPathComponent.fileSystemRepresentation,
                       O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
              : -1);
  struct stat source_metadata;
  NSMutableArray<NSString*>* links = [NSMutableArray array];
  success = success && input.get() >= 0 && output.get() >= 0 &&
            fstat(input.get(), &source_metadata) == 0 &&
            source_metadata.st_uid == caller_uid &&
            !(source_metadata.st_mode & 07000) &&
            ClearCopiedACL(output.get()) &&
            CopyDirectory(input.get(), output.get(), @"", caller_uid, _owner,
                          _group, links, 0, error) &&
            fchown(output.get(), _owner, _group) == 0 &&
            fchmod(output.get(), source_metadata.st_mode & 0755) == 0 &&
            ValidateSymlinks(_preparedPath, links, error) &&
            [self validateIncoming:_preparedPath error:error] &&
            fstat(output.get(), &_incoming) == 0;
  if (!success) {
    if (error && !*error) {
      Fail(error, @"Could not prepare a safe private update copy");
    }
    RemoveTreeAt(_stagingFD, _privateName.fileSystemRepresentation,
                 &_privateMetadata, nullptr);
    [self releaseDescriptors];
    return nil;
  }
  _token = NSUUID.UUID.UUIDString;
  _state = @"prepared";
  _lastActivity = NSProcessInfo.processInfo.systemUptime;
  return _token;
}

- (BOOL)pollBrowserExit:(NSString**)error {
  if (!_exited && [_state isEqual:@"prepared"]) {
    struct kevent event;
    const timespec timeout = {0, 0};
    const int count = kevent(_watchFD, nullptr, 0, &event, 1, &timeout);
    if (count < 0 || (count == 1 && (event.flags & EV_ERROR))) {
      Fail(error, @"Could not confirm browser exit");
      return NO;
    }
    if (count == 1 && (event.fflags & NOTE_EXIT)) {
      _exited = YES;
      _state = @"ready";
    }
  }
  return YES;
}

- (NSString*)stateForToken:(NSString*)token
                 callerUID:(uid_t)caller_uid
                     error:(NSString**)error {
  if (![self checkToken:token uid:caller_uid error:error] ||
      ![self pollBrowserExit:error]) {
    return nil;
  }
  _claimed = YES;
  _lastActivity = NSProcessInfo.processInfo.systemUptime;
  return _state;
}

- (BOOL)reclaimIfExpired {
  if (!_token) {
    return YES;
  }
  // A prepare response that never reaches its caller cannot leak a permanent
  // root lock. A helper claims the token with getUpdateState before readiness.
  NSTimeInterval lease = _claimed ? 30 * 60 : 2 * 60;
  if (NSProcessInfo.processInfo.systemUptime - _lastActivity < lease) {
    return NO;
  }
  NSString* error = nil;
  BOOL retain =
      [_state isEqual:@"committed"] || [_state isEqual:@"rolled_back"];
  [self finishToken:_token
           callerUID:_callerUID
      preserveBackup:retain
               error:&error];
  NSLog(@"Expired privileged update lease; recovery bundles retained when "
        @"needed: %@",
        error ?: @"none");
  return YES;
}

- (BOOL)reclaimUncommittedForCallerUID:(uid_t)caller_uid {
  if (!_token) {
    return YES;
  }
  if (_claimed || caller_uid != _callerUID || ![self pollBrowserExit:nullptr] ||
      ![_state isEqual:@"ready"]) {
    return NO;
  }
  NSString* error = nil;
  [self finishToken:_token callerUID:_callerUID preserveBackup:NO error:&error];
  NSLog(@"Reclaimed abandoned uncommitted update after browser exit: %@",
        error ?: @"none");
  return YES;
}

- (BOOL)commitToken:(NSString*)token
          callerUID:(uid_t)caller_uid
              error:(NSString**)error {
  NSString* state = [self stateForToken:token callerUID:caller_uid error:error];
  if (![state isEqual:@"ready"]) {
    return Fail(error,
                @"Update cannot commit before the authenticated browser exits");
  }
  struct stat current;
  if (!MutableInstallation(_target, error)) {
    return NO;
  }
  if (fstatat(_parentFD, _target.lastPathComponent.fileSystemRepresentation,
              &current, AT_SYMLINK_NOFOLLOW) != 0 ||
      !SameFile(current, _original) || current.st_uid != _owner ||
      ![_baseVersion isEqual:BundleVersion(_target)] ||
      ![self validateIncoming:_preparedPath error:error]) {
    return Fail(error,
                @"Installed application changed before privileged replacement");
  }
  if (!OtherBrowserInstancesExited(_target, error)) {
    return NO;
  }
  // Unlike the user-session helper, root can swap across parent directories.
  // Keep the incoming app in the 0700 root directory throughout the operation,
  // so an administrator cannot substitute an exposed /Applications sibling.
  if (fstatat(_privateFD, _target.lastPathComponent.fileSystemRepresentation,
              &current, AT_SYMLINK_NOFOLLOW) != 0 ||
      !SameFile(current, _incoming) ||
      renameatx_np(
          _privateFD, _target.lastPathComponent.fileSystemRepresentation,
          _parentFD, _target.lastPathComponent.fileSystemRepresentation,
          RENAME_SWAP) != 0) {
    return SystemFailure(error,
                         @"Atomic privileged application replacement failed");
  }
  _state = @"committed";
  // Detect another installer racing the last pre-swap check. Restore only while
  // our incoming inode is still installed; otherwise retain both for recovery.
  struct stat previous;
  if (fstatat(_privateFD, _target.lastPathComponent.fileSystemRepresentation,
              &previous, AT_SYMLINK_NOFOLLOW) != 0 ||
      !SameFile(previous, _original) ||
      fstatat(_parentFD, _target.lastPathComponent.fileSystemRepresentation,
              &current, AT_SYMLINK_NOFOLLOW) != 0 ||
      !SameFile(current, _incoming)) {
    _conflict = YES;
    if (fstatat(_parentFD, _target.lastPathComponent.fileSystemRepresentation,
                &current, AT_SYMLINK_NOFOLLOW) == 0 &&
        SameFile(current, _incoming) &&
        renameatx_np(
            _privateFD, _target.lastPathComponent.fileSystemRepresentation,
            _parentFD, _target.lastPathComponent.fileSystemRepresentation,
            RENAME_SWAP) == 0) {
      _state = @"rolled_back";
    }
    return Fail(
        error,
        @"A concurrent installation changed the target; backups were retained");
  }
  fsync(_parentFD);
  return YES;
}

- (BOOL)rollbackToken:(NSString*)token
            callerUID:(uid_t)caller_uid
                error:(NSString**)error {
  if (![self checkToken:token uid:caller_uid error:error] ||
      ![_state isEqual:@"committed"]) {
    return Fail(error,
                @"This transaction has no committed update to roll back");
  }
  struct stat installed;
  struct stat previous;
  if (fstatat(_parentFD, _target.lastPathComponent.fileSystemRepresentation,
              &installed, AT_SYMLINK_NOFOLLOW) != 0 ||
      fstatat(_privateFD, _target.lastPathComponent.fileSystemRepresentation,
              &previous, AT_SYMLINK_NOFOLLOW) != 0 ||
      !SameFile(installed, _incoming) || !SameFile(previous, _original) ||
      renameatx_np(
          _privateFD, _target.lastPathComponent.fileSystemRepresentation,
          _parentFD, _target.lastPathComponent.fileSystemRepresentation,
          RENAME_SWAP) != 0) {
    _conflict = YES;
    return Fail(
        error,
        @"Installed application changed; rollback left both bundles untouched");
  }
  _state = @"rolled_back";
  fsync(_parentFD);
  return YES;
}

- (BOOL)finishToken:(NSString*)token
          callerUID:(uid_t)caller_uid
     preserveBackup:(BOOL)preserve_backup
              error:(NSString**)error {
  if (![self checkToken:token uid:caller_uid error:error]) {
    return NO;
  }
  BOOL success = YES;
  const BOOL replaced =
      [_state isEqual:@"committed"] || [_state isEqual:@"rolled_back"];
  struct stat current;
  const struct stat& expected_target =
      [_state isEqual:@"committed"] ? _incoming : _original;
  if (replaced &&
      (fstatat(_parentFD, _target.lastPathComponent.fileSystemRepresentation,
               &current, AT_SYMLINK_NOFOLLOW) != 0 ||
       !SameFile(current, expected_target))) {
    _conflict = YES;
    success = Fail(
        error,
        @"Installed application changed; its recovery bundle was retained");
  }
  if (_conflict || (replaced && preserve_backup)) {
    NSLog(@"Privileged update retained bundle at %@", _preparedPath);
  } else {
    const struct stat& expected_backup =
        [_state isEqual:@"committed"] ? _original : _incoming;
    if ((replaced &&
         !TrustedBackupTreeAt(
             _privateFD, _target.lastPathComponent.fileSystemRepresentation,
             _owner, 0, error)) ||
        !RemoveTreeAt(_privateFD,
                      _target.lastPathComponent.fileSystemRepresentation,
                      &expected_backup, error) ||
        !RemoveTreeAt(_stagingFD, _privateName.fileSystemRepresentation,
                      &_privateMetadata, error)) {
      success = NO;
    }
  }
  _token = nil;
  _state = @"finished";
  [self releaseDescriptors];
  return success;
}

@end
