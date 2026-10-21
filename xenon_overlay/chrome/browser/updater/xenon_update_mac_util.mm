// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/updater/xenon_update_mac_util.h"

#include <CommonCrypto/CommonDigest.h>
#import <Security/Security.h>
#include <copyfile.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

#include <array>

namespace xenon::updater {
namespace {

template <typename T>
class ScopedCF {
 public:
  ~ScopedCF() {
    if (value) {
      CFRelease(value);
    }
  }
  T value = nullptr;
};

class ScopedFD {
 public:
  explicit ScopedFD(int fd) : fd_(fd) {}
  ~ScopedFD() {
    if (fd_ >= 0) {
      close(fd_);
    }
  }
  int get() const { return fd_; }

 private:
  const int fd_;
};

constexpr NSUInteger kMaximumResultBytes = 64 * 1024;

bool Fail(NSString** error, NSString* message) {
  if (error) {
    *error = message;
  }
  return false;
}

bool ReadBundle(NSString* path, NSDictionary** info, NSString** error) {
  struct stat metadata;
  if (![path isAbsolutePath] || ![path.pathExtension isEqualToString:@"app"] ||
      lstat(path.fileSystemRepresentation, &metadata) != 0 ||
      !S_ISDIR(metadata.st_mode)) {
    return Fail(error,
                @"Update paths must be existing application directories");
  }
  NSDictionary* properties = [NSDictionary
      dictionaryWithContentsOfFile:
          [path stringByAppendingPathComponent:@"Contents/Info.plist"]];
  NSString* executable = properties[@"CFBundleExecutable"];
  NSString* identifier = properties[@"CFBundleIdentifier"];
  if (![identifier isKindOfClass:[NSString class]] || identifier.length == 0 ||
      ![executable isKindOfClass:[NSString class]] || executable.length == 0 ||
      [executable containsString:@"/"] || [executable isEqualToString:@"."] ||
      [executable isEqualToString:@".."]) {
    return Fail(error,
                @"Application Info.plist has no valid identifier/executable");
  }
  NSString* executable_path =
      [[path stringByAppendingPathComponent:@"Contents/MacOS"]
          stringByAppendingPathComponent:executable];
  if (stat(executable_path.fileSystemRepresentation, &metadata) != 0 ||
      !S_ISREG(metadata.st_mode) ||
      access(executable_path.fileSystemRepresentation, X_OK) != 0) {
    return Fail(error, @"Application executable is missing or not executable");
  }
  *info = properties;
  return true;
}

bool CheckSignature(SecStaticCodeRef code,
                    SecRequirementRef requirement,
                    bool allow_unsigned,
                    NSString** error,
                    bool* is_unsigned = nullptr) {
  const OSStatus status = SecStaticCodeCheckValidity(
      code,
      kSecCSCheckAllArchitectures | kSecCSCheckNestedCode |
          kSecCSStrictValidate,
      requirement);
  if (is_unsigned) {
    *is_unsigned = status == errSecCSUnsigned;
  }
  if (status == errSecSuccess ||
      (allow_unsigned && status == errSecCSUnsigned)) {
    return true;
  }
  return Fail(
      error,
      [NSString
          stringWithFormat:@"Application signature verification failed (%d)",
                           static_cast<int>(status)]);
}

NSString* CanonicalFilePath(NSURL* url) {
  if (!url.isFileURL || !url.path.isAbsolutePath) {
    return nil;
  }
  return url.URLByStandardizingPath.URLByResolvingSymlinksInPath.path;
}

bool IsResultStatus(NSString* status) {
  return [@[
    @"validation_failed", @"launch_policy_failed", @"watch_failed",
    @"handshake_failed", @"wait_failed", @"replacement_failed",
    @"relaunch_failed_rolled_back", @"relaunch_failed_rollback_failed",
    @"launching", @"installed_pending_launch", @"awaiting_user_approval",
    @"installed_not_started", @"success"
  ] containsObject:status];
}

bool IsPrivateResultFile(const struct stat& metadata) {
  return S_ISREG(metadata.st_mode) && metadata.st_uid == geteuid() &&
         metadata.st_nlink == 1 && (metadata.st_mode & 0777) == 0600 &&
         metadata.st_size >= 0 &&
         static_cast<uint64_t>(metadata.st_size) <= kMaximumResultBytes;
}

NSString* ResultName(NSString* target) {
  return [target.lastPathComponent
      stringByAppendingString:@".xenon-update-result.json"];
}

bool HasQuarantine(NSString* path, bool* quarantined, NSString** error) {
  const ssize_t size =
      getxattr(path.fileSystemRepresentation, "com.apple.quarantine", nullptr,
               0, 0, XATTR_NOFOLLOW);
  if (size >= 0) {
    *quarantined = true;
    return true;
  }
  if (errno == ENOATTR || errno == ENOTSUP) {
    return true;
  }
  return Fail(
      error,
      [NSString stringWithFormat:
                    @"Could not inspect update quarantine attributes: %s",
                    strerror(errno)]);
}

bool ValidateInstallerPackageFD(int descriptor,
                                NSString* expected_sha256,
                                NSString** error) {
  NSFileHandle* reader = [[NSFileHandle alloc] initWithFileDescriptor:descriptor
                                                       closeOnDealloc:NO];
  NSError* read_error = nil;
  if (![reader seekToOffset:0 error:&read_error]) {
    return Fail(error, @"Could not verify persistent installer package");
  }
  NSData* magic = [reader readDataUpToLength:4 error:&read_error];
  if (![magic isEqual:[@"xar!" dataUsingEncoding:NSASCIIStringEncoding]] ||
      ![reader seekToOffset:0 error:&read_error]) {
    return Fail(error, @"Installer package is not a flat XAR package");
  }
  CC_SHA256_CTX context;
  CC_SHA256_Init(&context);
  for (;;) {
    NSData* chunk = [reader readDataUpToLength:1024 * 1024 error:&read_error];
    if (!chunk || read_error) {
      return Fail(error, @"Could not read persistent installer package");
    }
    if (chunk.length == 0) {
      break;
    }
    CC_SHA256_Update(&context, chunk.bytes, static_cast<CC_LONG>(chunk.length));
  }
  std::array<unsigned char, CC_SHA256_DIGEST_LENGTH> digest;
  CC_SHA256_Final(digest.data(), &context);
  NSMutableString* actual = [NSMutableString stringWithCapacity:64];
  for (unsigned char byte : digest) {
    [actual appendFormat:@"%02x", byte];
  }
  if (![actual isEqualToString:expected_sha256]) {
    return Fail(
        error,
        @"Persistent installer package SHA-256 does not match the update feed");
  }
  return true;
}

}  // namespace

bool ValidateUpdateBundles(NSString* staged_app,
                           NSString* target_app,
                           NSString** error) {
  NSDictionary* staged_info = nil;
  NSDictionary* target_info = nil;
  if (!ReadBundle(staged_app, &staged_info, error) ||
      !ReadBundle(target_app, &target_info, error)) {
    return false;
  }
  if (![staged_info[@"CFBundleIdentifier"]
          isEqual:target_info[@"CFBundleIdentifier"]]) {
    return Fail(error,
                @"Update bundle identifier differs from installed application");
  }

  ScopedCF<SecStaticCodeRef> target_code;
  ScopedCF<SecStaticCodeRef> staged_code;
  if (SecStaticCodeCreateWithPath(
          (__bridge CFURLRef)[NSURL fileURLWithPath:target_app],
          kSecCSDefaultFlags, &target_code.value) != errSecSuccess ||
      SecStaticCodeCreateWithPath(
          (__bridge CFURLRef)[NSURL fileURLWithPath:staged_app],
          kSecCSDefaultFlags, &staged_code.value) != errSecSuccess) {
    return Fail(error, @"Could not inspect application code signature");
  }

  // Development builds can be unsigned/ad-hoc signed. A production-signed
  // installation must accept only updates satisfying its designated identity;
  // removing _CodeSignature cannot downgrade that requirement.
  bool target_unsigned = false;
  if (!CheckSignature(target_code.value, nullptr, true, error,
                      &target_unsigned)) {
    return false;
  }
  ScopedCF<CFDictionaryRef> signing_info;
  OSStatus status = SecCodeCopySigningInformation(
      target_code.value, kSecCSSigningInformation, &signing_info.value);
  if (status != errSecSuccess && status != errSecCSUnsigned) {
    return Fail(error,
                @"Could not read installed application's signing identity");
  }
  NSDictionary* signing_dictionary = (__bridge NSDictionary*)signing_info.value;
  const bool development_build =
      target_unsigned ||
      ([signing_dictionary[(__bridge NSString*)kSecCodeInfoFlags]
           unsignedIntValue] &
       kSecCodeSignatureAdhoc);
  ScopedCF<SecRequirementRef> requirement;
  if (!development_build &&
      SecCodeCopyDesignatedRequirement(target_code.value, kSecCSDefaultFlags,
                                       &requirement.value) != errSecSuccess) {
    return Fail(error,
                @"Could not read installed application's signing requirement");
  }
  return CheckSignature(staged_code.value, requirement.value, development_build,
                        error);
}

bool HasUpdateQuarantine(NSString* app, bool* quarantined, NSString** error) {
  if (error) {
    *error = nil;
  }
  if (!quarantined) {
    return Fail(error, @"Missing quarantine query output");
  }
  *quarantined = false;
  NSDictionary* info = nil;
  if (!ReadBundle(app, &info, error)) {
    return false;
  }
  NSString* executable = [[app stringByAppendingPathComponent:@"Contents/MacOS"]
      stringByAppendingPathComponent:info[@"CFBundleExecutable"]];
  return HasQuarantine(app, quarantined, error) &&
         HasQuarantine(executable, quarantined, error);
}

NSString* PendingUpdateStatusAfterLaunchFailure(NSString* app,
                                                NSString** error) {
  bool quarantined = false;
  if (!HasUpdateQuarantine(app, &quarantined, error)) {
    return @"installed_not_started";
  }
  // Quarantine is only a reason to leave room for the user's security prompt.
  // It is not proof that Gatekeeper caused this particular launch failure.
  return quarantined ? @"awaiting_user_approval" : nil;
}

bool WriteUpdateResult(NSString* target_app,
                       NSString* target_version,
                       NSString* status,
                       NSString* message,
                       NSString* retained_path,
                       NSString** error) {
  NSString* target = CanonicalFilePath([NSURL fileURLWithPath:target_app]);
  if (!target_app.isAbsolutePath || !target || !IsResultStatus(status) ||
      !target_version || !message) {
    return Fail(error, @"Invalid update result fields");
  }
  NSDictionary* result = @{
    @"schema_version" : @1,
    @"target" : target,
    @"target_version" : target_version,
    @"status" : status,
    @"message" : message,
    @"retained_path" : retained_path ?: @"",
    @"timestamp" : @([NSDate date].timeIntervalSince1970),
  };
  NSData* data = [NSJSONSerialization dataWithJSONObject:result
                                                 options:0
                                                   error:nil];
  if (!data || data.length > kMaximumResultBytes) {
    return Fail(error, @"Update result is too large or cannot be serialized");
  }
  ScopedFD parent(
      open(target.stringByDeletingLastPathComponent.fileSystemRepresentation,
           O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  if (parent.get() < 0) {
    return Fail(error, @"Could not open update result directory");
  }
  NSString* name = ResultName(target);
  struct stat metadata;
  if (fstatat(parent.get(), name.fileSystemRepresentation, &metadata,
              AT_SYMLINK_NOFOLLOW) == 0) {
    if (!IsPrivateResultFile(metadata)) {
      return Fail(error,
                  @"Existing update result is not a private regular file");
    }
  } else if (errno != ENOENT) {
    return Fail(error, @"Could not inspect existing update result");
  }
  NSString* temporary = [NSString
      stringWithFormat:@".xenon-result-%@.tmp", NSUUID.UUID.UUIDString];
  ScopedFD output(openat(parent.get(), temporary.fileSystemRepresentation,
                         O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                         0600));
  if (output.get() < 0) {
    return Fail(error, @"Could not create private update result");
  }
  NSFileHandle* writer =
      [[NSFileHandle alloc] initWithFileDescriptor:output.get()
                                    closeOnDealloc:NO];
  NSError* write_error = nil;
  const bool written = fchmod(output.get(), 0600) == 0 &&
                       [writer writeData:data error:&write_error] &&
                       fsync(output.get()) == 0;
  if (written && renameat(parent.get(), temporary.fileSystemRepresentation,
                          parent.get(), name.fileSystemRepresentation) == 0) {
    // Best effort directory durability; the complete file is already synced.
    fsync(parent.get());
    return true;
  }
  const int saved_errno = errno;
  unlinkat(parent.get(), temporary.fileSystemRepresentation, 0);
  if (write_error) {
    return Fail(
        error,
        [NSString stringWithFormat:@"Could not write update result: %@ (%ld)",
                                   write_error.domain,
                                   static_cast<long>(write_error.code)]);
  }
  return Fail(error,
              [NSString stringWithFormat:@"Could not persist update result: %s",
                                         strerror(saved_errno)]);
}

NSDictionary* ReadUpdateResult(NSString* target_app, NSString** error) {
  if (error) {
    *error = nil;
  }
  NSString* target = CanonicalFilePath([NSURL fileURLWithPath:target_app]);
  if (!target_app.isAbsolutePath || !target) {
    Fail(error, @"Invalid update result target");
    return nil;
  }
  ScopedFD parent(
      open(target.stringByDeletingLastPathComponent.fileSystemRepresentation,
           O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  if (parent.get() < 0) {
    Fail(error, @"Could not open update result directory");
    return nil;
  }
  ScopedFD input(openat(parent.get(),
                        ResultName(target).fileSystemRepresentation,
                        O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
  if (input.get() < 0) {
    if (errno != ENOENT) {
      Fail(error, @"Could not safely open update result");
    }
    return nil;
  }
  struct stat metadata;
  if (fstat(input.get(), &metadata) != 0 || !IsPrivateResultFile(metadata)) {
    Fail(error, @"Update result is not a bounded private regular file");
    return nil;
  }
  const NSUInteger size = static_cast<NSUInteger>(metadata.st_size);
  NSMutableData* data = [NSMutableData dataWithCapacity:size];
  NSFileHandle* reader =
      [[NSFileHandle alloc] initWithFileDescriptor:input.get()
                                    closeOnDealloc:NO];
  while (data.length < size) {
    NSData* chunk = [reader readDataUpToLength:size - data.length error:nil];
    if (!chunk || chunk.length == 0) {
      Fail(error, @"Could not read complete update result");
      return nil;
    }
    [data appendData:chunk];
  }
  id result = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
  if (![result isKindOfClass:[NSDictionary class]] ||
      ![result[@"schema_version"] isEqual:@1] ||
      ![result[@"target"] isEqual:target] ||
      ![result[@"target_version"] isKindOfClass:[NSString class]] ||
      ![result[@"status"] isKindOfClass:[NSString class]] ||
      !IsResultStatus(result[@"status"]) ||
      ![result[@"message"] isKindOfClass:[NSString class]] ||
      ![result[@"retained_path"] isKindOfClass:[NSString class]] ||
      ![result[@"timestamp"] isKindOfClass:[NSNumber class]]) {
    Fail(error, @"Update result has invalid schema or target");
    return nil;
  }
  return result;
}

bool CanInstallOwnedUpdateBundle(NSString* target, NSString** error) {
  struct stat metadata;
  if (!target.isAbsolutePath ||
      lstat(target.fileSystemRepresentation, &metadata) != 0 ||
      !S_ISDIR(metadata.st_mode)) {
    return Fail(error,
                @"The installed application must be an existing directory");
  }
  if (metadata.st_uid == 0 || metadata.st_uid != geteuid()) {
    return Fail(error, @"This application requires a system Installer package "
                       @"to preserve its installed ownership");
  }
  if (access(target.stringByDeletingLastPathComponent.fileSystemRepresentation,
             W_OK | X_OK) != 0) {
    return Fail(
        error,
        @"The application directory requires a system Installer package");
  }
  return true;
}

NSString* PrepareInstallerPackageInCache(NSString* source,
                                         NSString* expected_sha256,
                                         NSString* cache_root,
                                         NSString** error) {
  NSString* digest_text = expected_sha256.lowercaseString;
  NSCharacterSet* non_hex = [[NSCharacterSet
      characterSetWithCharactersInString:@"0123456789abcdef"] invertedSet];
  if (!source.isAbsolutePath ||
      ![source.pathExtension.lowercaseString isEqualToString:@"pkg"] ||
      digest_text.length != 64 ||
      [digest_text rangeOfCharacterFromSet:non_hex].location != NSNotFound ||
      !cache_root.isAbsolutePath) {
    Fail(error,
         @"Installer package needs an absolute .pkg path and SHA-256 digest");
    return nil;
  }
  ScopedFD input(open(source.fileSystemRepresentation,
                      O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
  struct stat source_metadata;
  if (input.get() < 0 || fstat(input.get(), &source_metadata) != 0 ||
      !S_ISREG(source_metadata.st_mode) || source_metadata.st_size < 4) {
    Fail(error, @"Installer package is not a regular flat PKG file");
    return nil;
  }
  ScopedFD cache(open(cache_root.fileSystemRepresentation,
                      O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  struct stat cache_metadata;
  if (cache.get() < 0 || fstat(cache.get(), &cache_metadata) != 0 ||
      !S_ISDIR(cache_metadata.st_mode) || cache_metadata.st_uid != geteuid()) {
    Fail(error, @"Installer package cache is not an owned directory");
    return nil;
  }
  constexpr char kPackagesDirectory[] = "XenonUpdaterPackages";
  if (mkdirat(cache.get(), kPackagesDirectory, 0700) != 0 && errno != EEXIST) {
    Fail(error, @"Could not create persistent installer package cache");
    return nil;
  }
  ScopedFD packages(openat(cache.get(), kPackagesDirectory,
                           O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  struct stat packages_metadata;
  if (packages.get() < 0 || fstat(packages.get(), &packages_metadata) != 0 ||
      packages_metadata.st_uid != geteuid() ||
      (packages_metadata.st_mode & 0777) != 0700) {
    Fail(error, @"Installer package cache is not private");
    return nil;
  }
  // Retries already point at our durable copy. Revalidate the same open file
  // and its private directory instead of accumulating another full PKG.
  NSString* source_directory = source.stringByDeletingLastPathComponent;
  NSString* source_packages = CanonicalFilePath([NSURL
      fileURLWithPath:source_directory.stringByDeletingLastPathComponent]);
  NSString* expected_packages = CanonicalFilePath([NSURL
      fileURLWithPath:
          [cache_root stringByAppendingPathComponent:@"XenonUpdaterPackages"]]);
  if ([source_packages isEqualToString:expected_packages]) {
    NSString* previous_attempt = source_directory.lastPathComponent;
    ScopedFD previous_directory(
        openat(packages.get(), previous_attempt.fileSystemRepresentation,
               O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    struct stat directory_metadata;
    struct stat entry_metadata;
    if (![source.lastPathComponent isEqualToString:@"update.pkg"] ||
        ![[NSUUID alloc] initWithUUIDString:previous_attempt] ||
        previous_directory.get() < 0 ||
        fstat(previous_directory.get(), &directory_metadata) != 0 ||
        directory_metadata.st_uid != geteuid() ||
        (directory_metadata.st_mode & 0777) != 0700 ||
        source_metadata.st_uid != geteuid() || source_metadata.st_nlink != 1 ||
        (source_metadata.st_mode & 0777) != 0600 ||
        fstatat(previous_directory.get(), "update.pkg", &entry_metadata,
                AT_SYMLINK_NOFOLLOW) != 0 ||
        !S_ISREG(entry_metadata.st_mode) ||
        entry_metadata.st_dev != source_metadata.st_dev ||
        entry_metadata.st_ino != source_metadata.st_ino) {
      Fail(error,
           @"Cached installer package is no longer a private original file");
      return nil;
    }
    if (!ValidateInstallerPackageFD(input.get(), digest_text, error)) {
      return nil;
    }
    return source;
  }
  NSString* attempt = NSUUID.UUID.UUIDString;
  if (mkdirat(packages.get(), attempt.fileSystemRepresentation, 0700) != 0) {
    Fail(error, @"Could not create a private installer package directory");
    return nil;
  }
  ScopedFD directory(openat(packages.get(), attempt.fileSystemRepresentation,
                            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  auto fail_copy = [&](NSString* message) -> NSString* {
    if (directory.get() >= 0) {
      unlinkat(directory.get(), "update.pkg", 0);
    }
    unlinkat(packages.get(), attempt.fileSystemRepresentation, AT_REMOVEDIR);
    Fail(error, message);
    return nil;
  };
  if (directory.get() < 0) {
    return fail_copy(@"Could not open private installer package directory");
  }
  ScopedFD output(openat(directory.get(), "update.pkg",
                         O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                         0600));
  if (output.get() < 0 ||
      fcopyfile(input.get(), output.get(), nullptr,
                COPYFILE_DATA | COPYFILE_XATTR) != 0 ||
      fchmod(output.get(), 0600) != 0 || fsync(output.get()) != 0) {
    return fail_copy(
        @"Could not persist installer package and its quarantine metadata");
  }
  NSString* validation_error = nil;
  if (!ValidateInstallerPackageFD(output.get(), digest_text,
                                  &validation_error)) {
    return fail_copy(validation_error);
  }
  fsync(directory.get());
  return [[[cache_root stringByAppendingPathComponent:@"XenonUpdaterPackages"]
      stringByAppendingPathComponent:attempt]
      stringByAppendingPathComponent:@"update.pkg"];
}

bool ValidateRelaunchResult(NSString* target_app,
                            pid_t process_id,
                            bool terminated,
                            NSURL* bundle_url,
                            NSURL* executable_url,
                            NSString** error) {
  // LaunchServices may return an application object with PID -1 and no NSError
  // after Gatekeeper rejects the launch. Never treat that as a successful
  // start.
  if (process_id <= 0 || terminated) {
    return Fail(
        error,
        [NSString stringWithFormat:@"Relaunch returned no live application "
                                   @"(pid=%d, terminated=%d)",
                                   process_id, terminated]);
  }
  NSDictionary* info = nil;
  if (!ReadBundle(target_app, &info, error)) {
    return false;
  }
  NSString* expected_executable =
      [[target_app stringByAppendingPathComponent:@"Contents/MacOS"]
          stringByAppendingPathComponent:info[@"CFBundleExecutable"]];
  NSString* expected_bundle_path =
      CanonicalFilePath([NSURL fileURLWithPath:target_app]);
  NSString* expected_executable_path =
      CanonicalFilePath([NSURL fileURLWithPath:expected_executable]);
  NSString* actual_bundle_path = CanonicalFilePath(bundle_url);
  NSString* actual_executable_path = CanonicalFilePath(executable_url);
  if (!expected_bundle_path || !expected_executable_path ||
      ![actual_bundle_path isEqualToString:expected_bundle_path] ||
      ![actual_executable_path isEqualToString:expected_executable_path]) {
    return Fail(
        error,
        [NSString stringWithFormat:@"Relaunch opened an unexpected application "
                                   @"(bundle=%@, executable=%@, expected=%@)",
                                   actual_bundle_path, actual_executable_path,
                                   expected_bundle_path]);
  }
  return true;
}

bool PromoteUpdateBundleToSibling(NSString* prepared_app,
                                  NSString* target_app,
                                  NSString* sibling_app,
                                  NSString** error) {
  NSString* target_parent =
      target_app.stringByStandardizingPath.stringByDeletingLastPathComponent;
  NSString* sibling_parent =
      sibling_app.stringByStandardizingPath.stringByDeletingLastPathComponent;
  struct stat prepared_stat;
  struct stat target_stat;
  if (!prepared_app.isAbsolutePath || !target_app.isAbsolutePath ||
      !sibling_app.isAbsolutePath ||
      ![prepared_app.pathExtension isEqualToString:@"app"] ||
      ![target_app.pathExtension isEqualToString:@"app"] ||
      ![sibling_app.pathExtension isEqualToString:@"app"] ||
      ![sibling_app.lastPathComponent hasPrefix:@".xenon-swap-"] ||
      ![sibling_parent isEqualToString:target_parent] ||
      lstat(prepared_app.fileSystemRepresentation, &prepared_stat) != 0 ||
      lstat(target_app.fileSystemRepresentation, &target_stat) != 0 ||
      !S_ISDIR(prepared_stat.st_mode) || !S_ISDIR(target_stat.st_mode) ||
      prepared_stat.st_uid != geteuid() ||
      prepared_stat.st_dev != target_stat.st_dev ||
      prepared_stat.st_ino == target_stat.st_ino) {
    return Fail(error, @"Promotion requires an owned prepared application and "
                       @"an unused hidden sibling on the installation volume");
  }
  // Moving a non-writable directory across parents can fail even when both
  // parents are writable. Only move the caller-owned incoming app; the old
  // installed app will remain in this parent during both swap and rollback.
  if (renamex_np(prepared_app.fileSystemRepresentation,
                 sibling_app.fileSystemRepresentation, RENAME_EXCL) != 0) {
    return Fail(
        error, [NSString stringWithFormat:@"Could not promote update to %@: %s",
                                          sibling_app, strerror(errno)]);
  }
  return true;
}

bool AtomicSwapAppBundles(NSString* staged_app,
                          NSString* target_app,
                          NSString** error) {
  struct stat staged_stat;
  struct stat target_stat;
  if (lstat(staged_app.fileSystemRepresentation, &staged_stat) != 0 ||
      lstat(target_app.fileSystemRepresentation, &target_stat) != 0 ||
      !S_ISDIR(staged_stat.st_mode) || !S_ISDIR(target_stat.st_mode) ||
      staged_stat.st_dev != target_stat.st_dev ||
      staged_stat.st_ino == target_stat.st_ino) {
    return Fail(error,
                @"Atomic replacement needs distinct directories on one volume");
  }
  if (renamex_np(staged_app.fileSystemRepresentation,
                 target_app.fileSystemRepresentation, RENAME_SWAP) != 0) {
    return Fail(
        error,
        [NSString stringWithFormat:@"Atomic application replacement failed: %s",
                                   strerror(errno)]);
  }
  return true;
}

}  // namespace xenon::updater
