#ifndef CROSS_TRANSFER_TESTS_TEST_ARGUMENTS_H_
#define CROSS_TRANSFER_TESTS_TEST_ARGUMENTS_H_

#include <cstddef>
#include <span>

#if defined(CROSS_TRANSFER_CHROMIUM)
#include "partition_alloc/partition_alloc_base/compiler_specific.h"
#endif

namespace cross_transfer::test {

inline std::span<char* const> CommandLineArguments(int argc, char** argv) {
  // SAFETY: The C++ runtime supplies main with a nonnegative argc and an argv
  // array containing that many argument pointers, all valid for main's lifetime.
#if defined(CROSS_TRANSFER_CHROMIUM)
  return PA_UNSAFE_BUFFERS(
      std::span<char* const>(argv, static_cast<std::size_t>(argc)));
#else
  return std::span<char* const>(argv, static_cast<std::size_t>(argc));
#endif
}

}  // namespace cross_transfer::test

#endif  // CROSS_TRANSFER_TESTS_TEST_ARGUMENTS_H_
