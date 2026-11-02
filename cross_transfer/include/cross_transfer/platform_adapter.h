#ifndef CROSS_TRANSFER_INCLUDE_CROSS_TRANSFER_PLATFORM_ADAPTER_H_
#define CROSS_TRANSFER_INCLUDE_CROSS_TRANSFER_PLATFORM_ADAPTER_H_

#include <string>
#include <memory>

namespace cross_transfer {

class ScopedSystemSleepBlocker {
 public:
  explicit ScopedSystemSleepBlocker(const std::string& reason);
  ~ScopedSystemSleepBlocker();

  ScopedSystemSleepBlocker(const ScopedSystemSleepBlocker&) = delete;
  ScopedSystemSleepBlocker& operator=(const ScopedSystemSleepBlocker&) = delete;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace cross_transfer

#endif  // CROSS_TRANSFER_INCLUDE_CROSS_TRANSFER_PLATFORM_ADAPTER_H_
