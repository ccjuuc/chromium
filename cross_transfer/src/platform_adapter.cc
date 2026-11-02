#include "cross_transfer/platform_adapter.h"

#if defined(_WIN32)
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

#elif !defined(__APPLE__)

namespace cross_transfer {

struct ScopedSystemSleepBlocker::Impl {};

ScopedSystemSleepBlocker::ScopedSystemSleepBlocker(const std::string& reason)
    : impl_(std::make_unique<Impl>()) {}

ScopedSystemSleepBlocker::~ScopedSystemSleepBlocker() = default;

} // namespace cross_transfer

#endif
