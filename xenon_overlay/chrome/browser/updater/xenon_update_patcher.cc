// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/updater/xenon_update_patcher.h"

#include "base/files/file_util.h"
#include "base/logging.h"
#include "components/zucchini/zucchini_integration.h"

namespace xenon::updater {

bool XenonUpdatePatcher::ApplyPatch(const base::FilePath& base_file,
                                    const base::FilePath& patch_file,
                                    const base::FilePath& output_file,
                                    std::string* error_detail) {
  if (!base::PathExists(base_file)) {
    std::string err = "Base file does not exist: " + base_file.MaybeAsASCII();
    LOG(ERROR) << "[XenonUpdatePatcher] " << err;
    if (error_detail) *error_detail = err;
    return false;
  }
  if (!base::PathExists(patch_file)) {
    std::string err = "Patch file does not exist: " + patch_file.MaybeAsASCII();
    LOG(ERROR) << "[XenonUpdatePatcher] " << err;
    if (error_detail) *error_detail = err;
    return false;
  }

  // Ensure output directory exists.
  base::FilePath output_dir = output_file.DirName();
  if (!base::DirectoryExists(output_dir) && !base::CreateDirectory(output_dir)) {
    std::string err = "Failed to create output directory: " + output_dir.MaybeAsASCII();
    LOG(ERROR) << "[XenonUpdatePatcher] " << err;
    if (error_detail) *error_detail = err;
    return false;
  }

  zucchini::status::Code status = zucchini::Apply(base_file, patch_file, output_file, /*force_keep=*/true);
  if (status != zucchini::status::kStatusSuccess) {
    std::string err = "zucchini::Apply failed with code " +
                      std::to_string(static_cast<int>(status)) +
                      " (base=" + base_file.BaseName().MaybeAsASCII() +
                      ", patch=" + patch_file.BaseName().MaybeAsASCII() + ")";
    LOG(ERROR) << "[XenonUpdatePatcher] " << err;
    if (error_detail) *error_detail = err;
    return false;
  }

  return true;
}

bool XenonUpdatePatcher::GeneratePatch(const base::FilePath& old_file,
                                      const base::FilePath& new_file,
                                      const base::FilePath& patch_file) {
  if (!base::PathExists(old_file) || !base::PathExists(new_file)) {
    return false;
  }
  base::FilePath patch_dir = patch_file.DirName();
  if (!base::DirectoryExists(patch_dir) && !base::CreateDirectory(patch_dir)) {
    return false;
  }

  zucchini::GenerateOptions options;
  zucchini::status::Code status = zucchini::Generate(old_file, new_file, patch_file, options, /*force_keep=*/true);
  return status == zucchini::status::kStatusSuccess;
}

}  // namespace xenon::updater
