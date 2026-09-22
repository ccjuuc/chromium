// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_UPDATER_XENON_UPDATE_MANAGER_H_
#define XENON_OVERLAY_CHROME_BROWSER_UPDATER_XENON_UPDATE_MANAGER_H_

#include <memory>
#include <optional>
#include <string>

#include "base/files/file_path.h"
#include "base/files/scoped_temp_dir.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/observer_list.h"
#include "base/observer_list_types.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "xenon_overlay/chrome/browser/updater/xenon_update_downloader.h"
#include "xenon_overlay/chrome/browser/updater/xenon_update_types.h"

namespace xenon::updater {

class XenonUpdateManager {
 public:
  class Observer : public base::CheckedObserver {
   public:
    virtual void OnCheckingForUpdate() {}
    virtual void OnUpdateAvailable(const UpdateManifest& manifest) {}
    virtual void OnUpdateNotAvailable(const std::string& current_version) {}
    virtual void OnDownloadProgress(const DownloadProgress& progress) {}
    virtual void OnUpdateDownloaded(const UpdateManifest& manifest) {}
    virtual void OnUpdateError(const std::string& error_message) {}
  };

  static XenonUpdateManager* GetInstance();

  XenonUpdateManager();
  ~XenonUpdateManager();

  XenonUpdateManager(const XenonUpdateManager&) = delete;
  XenonUpdateManager& operator=(const XenonUpdateManager&) = delete;

  void AddObserver(Observer* observer);
  void RemoveObserver(Observer* observer);

  void SetFeedURL(const std::string& url);
  std::string GetFeedURL() const;

  void SetAutoDownload(bool auto_download);
  bool GetAutoDownload() const;

  std::string GetCurrentVersion() const;
  void SetCurrentVersionForTesting(const std::string& version);

  void SetURLLoaderFactoryForTesting(
      scoped_refptr<network::SharedURLLoaderFactory> factory);

  // Sets base file path for testing differential updates.
  void SetBaseFileForTesting(const base::FilePath& base_file);

  // Initiates update check with configured or provided feed URL.
  void CheckForUpdates(const std::string& feed_url = "");

  // Initiates download of the discovered update.
  void DownloadUpdate();

  // Replaces files and relaunches application.
  void QuitAndInstall();

  // Applies staged update upon normal exit without relaunching.
  void InstallOnExit();

  // Synchronizes product version in Windows registry.
  static void UpdateRegistryVersion(base::FilePath install_dir,
                                    const std::string& version);

  // Called on browser startup to perform cleanup and registry synchronization.
  void OnVersionStartup();

  // Cleans up version-isolated directories older than the current version.
  void CleanupOldVersions();

  void SetInstallDirForTesting(const base::FilePath& dir);

  base::FilePath GetCurrentVersionDir() const;
  base::FilePath GetTargetVersionDir(const std::string& target_version) const;

  static void DoCleanupOldVersions(base::FilePath install_dir,
                                   std::string current_ver_str);

  struct VersionStagingResult {
    bool success = false;
    base::FilePath staged_dir;
    std::string error;
  };

  static VersionStagingResult PrepareVersionDirectory(
      base::FilePath current_version_dir,
      base::FilePath base_file,
      base::FilePath patch_file,
      base::FilePath target_version_dir,
      base::FilePath temp_staging_dir,
      std::vector<std::string> deletions = {});

  UpdateState GetState() const { return state_; }
  const std::optional<UpdateManifest>& GetManifest() const { return manifest_; }
  base::FilePath GetStagedPath() const { return staged_output_file_; }

 private:
  scoped_refptr<network::SharedURLLoaderFactory> GetURLLoaderFactory();
  base::FilePath GetDefaultBaseFile() const;
  base::FilePath GetDefaultInstallDir() const;
  base::FilePath GetDefaultExecutable() const;

  void SetState(UpdateState new_state);
  void NotifyError(const std::string& message);

  void OnCheckResponse(std::optional<std::string> response_body);
  void StartPackageDownload(const UpdatePackageInfo& package, bool is_diff);
  void OnDownloadProgress(const DownloadProgress& progress);
  void OnDownloadComplete(bool is_diff, bool success, const std::string& error);
  void OnPatchCompleteWithOutput(VersionStagingResult result);

  UpdateState state_ = UpdateState::kIdle;
  std::string feed_url_;
  bool auto_download_ = false;
  std::string current_version_override_;
  std::optional<UpdateManifest> manifest_;

  base::FilePath install_dir_override_;
  base::FilePath base_file_override_;
  base::ScopedTempDir temp_download_dir_;
  base::FilePath downloaded_file_;
  base::FilePath staged_output_file_;

  std::unique_ptr<network::SimpleURLLoader> check_loader_;
  std::unique_ptr<XenonUpdateDownloader> downloader_;
  scoped_refptr<network::SharedURLLoaderFactory> test_url_loader_factory_;

  base::ObserverList<Observer> observers_;
  base::WeakPtrFactory<XenonUpdateManager> weak_factory_{this};
};

}  // namespace xenon::updater

#endif  // XENON_OVERLAY_CHROME_BROWSER_UPDATER_XENON_UPDATE_MANAGER_H_
