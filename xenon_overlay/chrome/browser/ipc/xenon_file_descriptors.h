// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_FILE_DESCRIPTORS_H_
#define XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_FILE_DESCRIPTORS_H_

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>

#include "base/files/file_path.h"
#include "base/memory/ref_counted.h"
#include "base/synchronization/lock.h"
#include "base/values.h"
#include "xenon_overlay/public/mojom/xenon_ipc.mojom.h"

namespace xenon::ipc {

// Owns real libuv descriptors for one application main environment. Async
// workers retain the table, but CloseAll revokes it before V8 teardown.
// Numeric descriptors from another environment are never accepted.
class XenonFileDescriptors
    : public base::RefCountedThreadSafe<XenonFileDescriptors> {
 public:
  explicit XenonFileDescriptors(base::FilePath working_directory,
                                bool owns_process_directory = false);
  // Main and its Workers share the actual process cwd only in a dedicated
  // application process. Embedded runtimes must never change their host cwd.
  int GetWorkingDirectory(base::FilePath* directory) const;
  int ChangeWorkingDirectory(const std::string& directory);
  bool owns_process_directory() const { return owns_process_directory_; }
  static base::DictValue Constants();
  mojom::IpcResultPtr Call(base::Value arguments);
  // Workers share access to the application's real descriptor table. Owner
  // tokens affect cleanup only, never which environment may use a descriptor.
  mojom::IpcResultPtr CallOwned(base::Value arguments, uint64_t owner);
  void CloseOwned(uint64_t owner);
  void CloseAll();

 private:
  friend class base::RefCountedThreadSafe<XenonFileDescriptors>;
  ~XenonFileDescriptors();
  struct Entry;
  base::Lock lock_;
  const base::FilePath working_directory_;
  const bool owns_process_directory_;
  bool closed_ = false;
  std::set<uint64_t> closed_owners_;
  std::map<int, std::unique_ptr<Entry>> files_;
};

}  // namespace xenon::ipc
#endif
