#include "cross_transfer/platform_adapter.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(__APPLE__) && TARGET_OS_OSX
#import <Foundation/Foundation.h>

namespace cross_transfer {

struct ScopedSystemSleepBlocker::Impl {
  id<NSObject> activity_token{nil};

  Impl(const std::string& reason) {
    @autoreleasepool {
      NSString* reason_str = [NSString stringWithUTF8String:reason.c_str()];
      activity_token = [[NSProcessInfo processInfo]
          beginActivityWithOptions:(NSActivityUserInitiated | NSActivityIdleSystemSleepDisabled)
                            reason:reason_str];
      if (activity_token) {
        CFRetain((__bridge CFTypeRef)activity_token);
      }
    }
  }

  ~Impl() {
    if (activity_token) {
      @autoreleasepool {
        [[NSProcessInfo processInfo] endActivity:activity_token];
        CFRelease((__bridge CFTypeRef)activity_token);
        activity_token = nil;
      }
    }
  }
};

ScopedSystemSleepBlocker::ScopedSystemSleepBlocker(const std::string& reason)
    : impl_(std::make_unique<Impl>(reason)) {}

ScopedSystemSleepBlocker::~ScopedSystemSleepBlocker() = default;

} // namespace cross_transfer

#elif defined(_WIN32)
#include <windows.h>

namespace cross_transfer {

struct ScopedSystemSleepBlocker::Impl {
  Impl() {
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_AWAYMODE_REQUIRED);
  }
  ~Impl() {
    SetThreadExecutionState(ES_CONTINUOUS);
  }
};

ScopedSystemSleepBlocker::ScopedSystemSleepBlocker(const std::string& reason)
    : impl_(std::make_unique<Impl>()) {}

ScopedSystemSleepBlocker::~ScopedSystemSleepBlocker() = default;

} // namespace cross_transfer

#else

namespace cross_transfer {

struct ScopedSystemSleepBlocker::Impl {};
ScopedSystemSleepBlocker::ScopedSystemSleepBlocker(const std::string& reason)
    : impl_(std::make_unique<Impl>()) {}
ScopedSystemSleepBlocker::~ScopedSystemSleepBlocker() = default;

} // namespace cross_transfer
#endif
