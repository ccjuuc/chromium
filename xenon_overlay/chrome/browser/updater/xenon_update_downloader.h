// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_UPDATER_XENON_UPDATE_DOWNLOADER_H_
#define XENON_OVERLAY_CHROME_BROWSER_UPDATER_XENON_UPDATE_DOWNLOADER_H_

#include <memory>
#include <string>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "xenon_overlay/chrome/browser/updater/xenon_update_types.h"

namespace xenon::updater {

class XenonUpdateDownloader {
 public:
  using ProgressCallback = base::RepeatingCallback<void(const DownloadProgress&)>;
  using CompletionCallback = base::OnceCallback<void(bool success, const std::string& error)>;

  XenonUpdateDownloader();
  ~XenonUpdateDownloader();

  XenonUpdateDownloader(const XenonUpdateDownloader&) = delete;
  XenonUpdateDownloader& operator=(const XenonUpdateDownloader&) = delete;

  // Starts downloading |package_info| to |destination_path|.
  // |url_loader_factory| is used to issue the network request.
  void StartDownload(
      scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory,
      const UpdatePackageInfo& package_info,
      const base::FilePath& destination_path,
      ProgressCallback progress_callback,
      CompletionCallback completion_callback);

  void Cancel();

  bool is_downloading() const { return url_loader_ != nullptr; }

 private:
  void OnDownloadProgress(uint64_t current);
  void OnDownloadComplete(base::FilePath file_path);
  void OnVerifyHashComplete(std::pair<bool, std::string> result);

  UpdatePackageInfo package_info_;
  base::FilePath destination_path_;
  ProgressCallback progress_callback_;
  CompletionCallback completion_callback_;

  std::unique_ptr<network::SimpleURLLoader> url_loader_;
  base::WeakPtrFactory<XenonUpdateDownloader> weak_factory_{this};
};

}  // namespace xenon::updater

#endif  // XENON_OVERLAY_CHROME_BROWSER_UPDATER_XENON_UPDATE_DOWNLOADER_H_
