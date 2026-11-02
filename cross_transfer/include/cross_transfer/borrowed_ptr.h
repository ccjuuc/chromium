#ifndef CROSS_TRANSFER_INCLUDE_CROSS_TRANSFER_BORROWED_PTR_H_
#define CROSS_TRANSFER_INCLUDE_CROSS_TRANSFER_BORROWED_PTR_H_

// Chromium builds retain its dangling-pointer instrumentation. Standalone
// builds do not depend on PartitionAlloc; the documented owner/lifetime
// contract is identical in both configurations. GN exports the define to all
// consumers so public structs always have a consistent ABI within a build.
#if defined(CROSS_TRANSFER_CHROMIUM)
#include "partition_alloc/pointers/raw_ptr.h"
#endif

namespace cross_transfer {
template <typename T>
#if defined(CROSS_TRANSFER_CHROMIUM)
using BorrowedPtr = raw_ptr<T>;
#else
using BorrowedPtr = T*;
#endif
}  // namespace cross_transfer

#endif  // CROSS_TRANSFER_INCLUDE_CROSS_TRANSFER_BORROWED_PTR_H_
