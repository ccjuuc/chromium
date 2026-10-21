// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/updater/xenon_update_mac_privileged_generation.h"

#include <CommonCrypto/CommonDigest.h>
#import <Security/Security.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdint>

#include "xenon_overlay/chrome/browser/updater/xenon_update_mac_privileged_protocol.h"

namespace xenon::updater {
namespace {

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
  int fd_;
};

NSString* Fail(NSString** error, NSString* message) {
  if (error) {
    *error = message;
  }
  return nil;
}

bool SameFile(const struct stat& a, const struct stat& b) {
  return a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}

// Walk from the filesystem root so even intermediate aliases are rejected.
int OpenDirectory(NSString* path) {
  if (!path.isAbsolutePath) {
    return -1;
  }
  int fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  for (NSString* component in path.pathComponents) {
    if ([component isEqual:@"/"]) {
      continue;
    }
    if ([component isEqual:@"."] || [component isEqual:@".."]) {
      if (fd >= 0) {
        close(fd);
      }
      return -1;
    }
    const int child = openat(fd, component.fileSystemRepresentation,
                             O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd >= 0) {
      close(fd);
    }
    fd = child;
    if (fd < 0) {
      break;
    }
  }
  return fd;
}

NSData* ReadFileAt(int directory, const char* name, size_t limit) {
  ScopedFD file(
      openat(directory, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
  struct stat before;
  if (file.get() < 0 || fstat(file.get(), &before) != 0 ||
      !S_ISREG(before.st_mode) || before.st_nlink != 1 || before.st_size <= 0 ||
      static_cast<uint64_t>(before.st_size) > limit) {
    return nil;
  }
  NSMutableData* bytes = [NSMutableData dataWithCapacity:before.st_size];
  std::array<char, 16384> chunk;
  const size_t expected_length = static_cast<size_t>(before.st_size);
  while (bytes.length < expected_length) {
    const size_t wanted =
        std::min(chunk.size(), expected_length - bytes.length);
    const ssize_t count = read(file.get(), chunk.data(), wanted);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      return nil;
    }
    [bytes appendBytes:chunk.data() length:static_cast<size_t>(count)];
  }
  char extra;
  ssize_t count;
  do {
    count = read(file.get(), &extra, 1);
  } while (count < 0 && errno == EINTR);
  struct stat after;
  if (count != 0 || fstat(file.get(), &after) != 0 ||
      !SameFile(before, after) || before.st_size != after.st_size ||
      before.st_mtimespec.tv_sec != after.st_mtimespec.tv_sec ||
      before.st_mtimespec.tv_nsec != after.st_mtimespec.tv_nsec ||
      before.st_ctimespec.tv_sec != after.st_ctimespec.tv_sec ||
      before.st_ctimespec.tv_nsec != after.st_ctimespec.tv_nsec) {
    return nil;
  }
  return bytes;
}

NSData* CodeHash(NSString* helper, bool running_code) {
  SecStaticCodeRef code = nullptr;
  OSStatus status;
  if (running_code) {
    SecCodeRef self = nullptr;
    status = SecCodeCopySelf(kSecCSDefaultFlags, &self);
    if (status == errSecSuccess) {
      // SigningInformation includes some disk-derived fields. Validate the
      // dynamic code first so a replaced pathname cannot impersonate this
      // image.
      status = SecCodeCheckValidity(self, kSecCSStrictValidate, nullptr);
    }
    code = reinterpret_cast<SecStaticCodeRef>(self);
  } else {
    status = SecStaticCodeCreateWithPath(
        (__bridge CFURLRef)[NSURL fileURLWithPath:helper], kSecCSDefaultFlags,
        &code);
  }
  CFDictionaryRef information = nullptr;
  if (status == errSecSuccess) {
    status = SecCodeCopySigningInformation(
        code,
        kSecCSSigningInformation |
            (running_code ? kSecCSDynamicInformation : 0),
        &information);
  }
  NSDictionary* values = CFBridgingRelease(information);
  id hash = values[(__bridge NSString*)kSecCodeInfoUnique];
  if (code) {
    CFRelease(code);
  }
  return status == errSecSuccess && [hash isKindOfClass:NSData.class] &&
                 [hash length] > 0 && [hash length] <= 64
             ? [hash copy]
             : nil;
}

void AppendInteger(NSMutableData* data, uint64_t number) {
  std::array<unsigned char, 8> bytes;
  for (int i = 7; i >= 0; --i) {
    bytes[i] = number & 0xff;
    number >>= 8;
  }
  [data appendBytes:bytes.data() length:bytes.size()];
}

void AppendField(NSMutableData* data, NSData* field) {
  AppendInteger(data, field.length);
  [data appendData:field];
}

}  // namespace

NSString* PrivilegedServiceGeneration(NSString* target,
                                      bool running_code,
                                      NSString** error) {
  if (![target isKindOfClass:NSString.class] || !target.isAbsolutePath) {
    return Fail(error, @"Invalid privileged service application path");
  }
  ScopedFD app(OpenDirectory(target));
  struct stat app_metadata;
  if (app.get() < 0 || fstat(app.get(), &app_metadata) != 0) {
    return Fail(error, @"Could not open the privileged service application");
  }
  ScopedFD contents(openat(app.get(), "Contents",
                           O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  ScopedFD library(openat(contents.get(), "Library",
                          O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  ScopedFD launchd(openat(library.get(), "LaunchDaemons",
                          O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  ScopedFD helpers(openat(contents.get(), "Helpers",
                          O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  NSData* info_data = ReadFileAt(contents.get(), "Info.plist", 1024 * 1024);
  NSData* plist_data =
      ReadFileAt(launchd.get(), kPrivilegedUpdatePlist.fileSystemRepresentation,
                 64 * 1024);
  id info = info_data ? [NSPropertyListSerialization
                            propertyListWithData:info_data
                                         options:NSPropertyListImmutable
                                          format:nil
                                           error:nil]
                      : nil;
  id version = [info isKindOfClass:NSDictionary.class]
                   ? info[@"CFBundleShortVersionString"]
                   : nil;
  struct stat helper_metadata;
  if (![version isKindOfClass:NSString.class] || ![version length] ||
      [version length] > 128 || !plist_data ||
      fstatat(helpers.get(),
              kPrivilegedUpdateHelperName.fileSystemRepresentation,
              &helper_metadata, AT_SYMLINK_NOFOLLOW) != 0 ||
      !S_ISREG(helper_metadata.st_mode) || helper_metadata.st_nlink != 1) {
    return Fail(error,
                @"Invalid privileged service version, plist, or executable");
  }
  NSString* helper_path =
      [target stringByAppendingPathComponent:
                  @"Contents/Helpers/xenon_privileged_update_helper"];
  NSData* code_hash = CodeHash(helper_path, running_code);
  if (!code_hash) {
    return Fail(error, @"Could not read the privileged service code hash");
  }
  // Detect a concurrent whole-bundle or helper replacement while reading.
  struct stat current_app;
  struct stat current_helper;
  if (lstat(target.fileSystemRepresentation, &current_app) != 0 ||
      !SameFile(app_metadata, current_app) ||
      lstat(helper_path.fileSystemRepresentation, &current_helper) != 0 ||
      !SameFile(helper_metadata, current_helper)) {
    return Fail(error,
                @"Privileged service changed while reading its generation");
  }
  unsigned char plist_hash[CC_SHA256_DIGEST_LENGTH];
  CC_SHA256(plist_data.bytes, static_cast<CC_LONG>(plist_data.length),
            plist_hash);
  NSMutableData* generation = [[@"XenonPrivilegedServiceGeneration/v1"
      dataUsingEncoding:NSUTF8StringEncoding] mutableCopy];
  AppendField(generation, [version dataUsingEncoding:NSUTF8StringEncoding]);
  AppendInteger(generation, static_cast<uint64_t>(app_metadata.st_dev));
  AppendInteger(generation, static_cast<uint64_t>(app_metadata.st_ino));
  AppendField(generation, code_hash);
  AppendField(generation, [NSData dataWithBytes:plist_hash
                                         length:sizeof(plist_hash)]);
  unsigned char digest[CC_SHA256_DIGEST_LENGTH];
  CC_SHA256(generation.bytes, static_cast<CC_LONG>(generation.length), digest);
  NSMutableString* result =
      [NSMutableString stringWithCapacity:sizeof(digest) * 2];
  for (unsigned char byte : digest) {
    [result appendFormat:@"%02x", byte];
  }
  return result;
}

}  // namespace xenon::updater
