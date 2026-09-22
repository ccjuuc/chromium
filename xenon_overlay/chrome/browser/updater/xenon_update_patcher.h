// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_UPDATER_XENON_UPDATE_PATCHER_H_
#define XENON_OVERLAY_CHROME_BROWSER_UPDATER_XENON_UPDATE_PATCHER_H_

#include <string>

#include "base/files/file_path.h"
#include "base/functional/callback_forward.h"

namespace xenon::updater {

class XenonUpdatePatcher {
 public:
  // Synchronous patching method. Should typically be executed on a ThreadPool sequence.
  // Applies Zucchini differential patch on |base_file| with |patch_file|,
  // producing |output_file|. Returns true on success.
  static bool ApplyPatch(const base::FilePath& base_file,
                         const base::FilePath& patch_file,
                         const base::FilePath& output_file,
                         std::string* error_detail = nullptr);

  // Synchronous patch generation method. Useful for testing and tooling.
  static bool GeneratePatch(const base::FilePath& old_file,
                            const base::FilePath& new_file,
                            const base::FilePath& patch_file);
};

}  // namespace xenon::updater

#endif  // XENON_OVERLAY_CHROME_BROWSER_UPDATER_XENON_UPDATE_PATCHER_H_
