// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_PUBLIC_BROWSER_STORAGE_PARTITION_PATH_H_
#define CONTENT_PUBLIC_BROWSER_STORAGE_PARTITION_PATH_H_

#include "base/files/file_path.h"
#include "content/common/content_export.h"
#include "xenon_overlay/buildflags/buildflags.h"

#if BUILDFLAG(ENABLE_XENON_SERVICE)
namespace content {
class BrowserContext;
class StoragePartitionConfig;

// UI sequence only. Computes a persistent partition's directory using the same
// algorithm as StoragePartition creation, without creating the partition or
// touching disk. In-memory partitions have no storage directory.
CONTENT_EXPORT base::FilePath GetStoragePartitionPath(
    const BrowserContext* context,
    const StoragePartitionConfig& config);
}  // namespace content
#endif

#endif  // CONTENT_PUBLIC_BROWSER_STORAGE_PARTITION_PATH_H_
