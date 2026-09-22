// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/updater/xenon_update_downloader.h"

#include <array>
#include <vector>

#include "base/files/file.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/task/task_traits.h"
#include "base/task/thread_pool.h"
#include "crypto/secure_hash.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"

namespace xenon::updater {

namespace {

constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("xenon_update_downloader", R"(
        semantics {
          sender: "Xenon Application Updater"
          description: "Downloads application update package or differential patch."
          trigger: "User requested update or automatic background update check."
          data: "None."
          destination: OTHER
        }
        policy {
          cookies_allowed: NO
          setting: "This feature cannot be disabled by settings."
          policy_exception_justification: "Essential for application updates."
        })");

std::pair<bool, std::string> VerifyFileHashBlocking(
    const base::FilePath& path,
    const std::string& expected_sha256) {
  if (expected_sha256.empty()) {
    return {true, ""};
  }

  base::File file(path, base::File::FLAG_OPEN | base::File::FLAG_READ);
  if (!file.IsValid()) {
    return {false, "Failed to open downloaded file for hash verification"};
  }

  std::unique_ptr<crypto::SecureHash> hash =
      crypto::SecureHash::Create(crypto::SecureHash::SHA256);
  std::vector<uint8_t> buffer(64 * 1024);
  while (true) {
    std::optional<size_t> bytes_read =
        file.ReadAtCurrentPosNoBestEffort(base::span(buffer));
    if (!bytes_read.has_value()) {
      return {false, "I/O error while reading downloaded file"};
    }
    if (*bytes_read == 0) {
      break;
    }
    hash->Update(base::span(buffer).first(*bytes_read));
  }

  std::array<uint8_t, 32> digest;
  hash->Finish(digest);
  std::string actual_hex = base::HexEncode(digest);

  if (!base::EqualsCaseInsensitiveASCII(actual_hex, expected_sha256)) {
    return {false, "Hash mismatch: expected " + expected_sha256 +
                       ", got " + actual_hex};
  }

  return {true, ""};
}

}  // namespace

XenonUpdateDownloader::XenonUpdateDownloader() = default;

XenonUpdateDownloader::~XenonUpdateDownloader() {
  Cancel();
}

void XenonUpdateDownloader::Cancel() {
  url_loader_.reset();
  weak_factory_.InvalidateWeakPtrs();
  if (completion_callback_) {
    std::move(completion_callback_).Run(false, "Download cancelled");
  }
}

void XenonUpdateDownloader::StartDownload(
    scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory,
    const UpdatePackageInfo& package_info,
    const base::FilePath& destination_path,
    ProgressCallback progress_callback,
    CompletionCallback completion_callback) {
  Cancel();

  if (!url_loader_factory || !package_info.IsValid() || destination_path.empty()) {
    if (completion_callback) {
      std::move(completion_callback).Run(false, "Invalid download parameters");
    }
    return;
  }

  package_info_ = package_info;
  destination_path_ = destination_path;
  progress_callback_ = std::move(progress_callback);
  completion_callback_ = std::move(completion_callback);

  auto request = std::make_unique<network::ResourceRequest>();
  request->url = package_info_.url;
  request->method = "GET";
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;

  url_loader_ = network::SimpleURLLoader::Create(std::move(request), kTrafficAnnotation);
  url_loader_->SetOnDownloadProgressCallback(
      base::BindRepeating(&XenonUpdateDownloader::OnDownloadProgress,
                          weak_factory_.GetWeakPtr()));

  // Download directly to the specified destination path.
  url_loader_->DownloadToFile(
      url_loader_factory.get(),
      base::BindOnce(&XenonUpdateDownloader::OnDownloadComplete,
                     weak_factory_.GetWeakPtr()),
      destination_path_);
}

void XenonUpdateDownloader::OnDownloadProgress(uint64_t current) {
  if (!progress_callback_) {
    return;
  }

  DownloadProgress progress;
  progress.bytes_downloaded = static_cast<int64_t>(current);
  progress.total_bytes = package_info_.size;
  if (progress.total_bytes > 0) {
    progress.percent = static_cast<int>(
        (static_cast<double>(progress.bytes_downloaded) / progress.total_bytes) * 100);
    if (progress.percent > 100) {
      progress.percent = 100;
    }
  }

  progress_callback_.Run(progress);
}

void XenonUpdateDownloader::OnDownloadComplete(base::FilePath file_path) {
  int net_error = url_loader_ ? url_loader_->NetError() : net::ERR_FAILED;
  url_loader_.reset();

  if (file_path.empty() || net_error != net::OK) {
    if (completion_callback_) {
      std::move(completion_callback_).Run(
          false, "Network error downloading file: " + net::ErrorToString(net_error));
    }
    return;
  }

  // Verify hash on ThreadPool
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&VerifyFileHashBlocking, file_path, package_info_.sha256),
      base::BindOnce(&XenonUpdateDownloader::OnVerifyHashComplete,
                     weak_factory_.GetWeakPtr()));
}

void XenonUpdateDownloader::OnVerifyHashComplete(
    std::pair<bool, std::string> result) {
  bool matches = result.first;
  const std::string& error = result.second;
  if (!matches) {
    // Delete corrupt file
    base::ThreadPool::PostTask(
        FROM_HERE, {base::MayBlock(), base::TaskPriority::BEST_EFFORT},
        base::GetDeleteFileCallback(destination_path_));
  }

  if (completion_callback_) {
    std::move(completion_callback_).Run(matches, error);
  }
}

}  // namespace xenon::updater
