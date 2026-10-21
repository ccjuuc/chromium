// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_UPDATER_XENON_UPDATE_MANAGER_H_
#define XENON_OVERLAY_CHROME_BROWSER_UPDATER_XENON_UPDATE_MANAGER_H_

#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "base/files/file_path.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/observer_list.h"
#include "base/observer_list_types.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "xenon_overlay/chrome/browser/updater/xenon_system_updater.h"
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
    // Download/preparation transitions have no corresponding legacy event.
    virtual void OnUpdateStateChanged(UpdateState state) {}
    virtual void OnDownloadProgress(const DownloadProgress& progress) {}
    virtual void OnUpdateDownloaded(const UpdateManifest& manifest) {}
    // The PKG was handed to macOS Installer; installation is still pending.
    virtual void OnInstallerOpened() {}
    virtual void OnElevationApprovalRequired() {}
    virtual void OnUpdateError(const std::string& error_message) {}
  };

  static XenonUpdateManager* GetInstance();

  XenonUpdateManager();
  ~XenonUpdateManager();

  XenonUpdateManager(const XenonUpdateManager&) = delete;
  XenonUpdateManager& operator=(const XenonUpdateManager&) = delete;

  void AddObserver(Observer* observer);
  void RemoveObserver(Observer* observer);

  bool UsesSystemUpdater() const { return !!system_updater_; }
  bool SystemUpdateInstalled() const { return system_update_installed_; }
  void SetSystemUpdaterForTesting(std::unique_ptr<XenonSystemUpdater> updater) {
    system_updater_ = std::move(updater);
  }

  void SetFeedURL(const std::string& url);
  std::string GetFeedURL() const;

  void SetAutoDownload(bool auto_download);
  bool GetAutoDownload() const;
  void SetAutoInstallOnAppQuit(bool enabled);
  bool GetAutoInstallOnAppQuit() const;

  std::string GetCurrentVersion() const;
  void SetCurrentVersionForTesting(const std::string& version);

  void SetURLLoaderFactoryForTesting(
      scoped_refptr<network::SharedURLLoaderFactory> factory);

  // Sets base file path for testing differential updates.
  void SetBaseFileForTesting(const base::FilePath& base_file);
  struct DifferentialEligibilityResult {
    bool allowed = false;
    bool requires_elevation = false;
    std::string reason;
  };
  using DifferentialEligibilityCallback =
      base::RepeatingCallback<DifferentialEligibilityResult(base::FilePath)>;
  void SetDifferentialEligibilityForTesting(
      DifferentialEligibilityCallback callback) {
    differential_eligibility_for_testing_ = std::move(callback);
  }

  // Initiates update check with configured or provided feed URL.
  void CheckForUpdates(const std::string& feed_url = "");

  // Initiates download of the discovered update.
  void DownloadUpdate();
  // Explicit alternative while a prepared differential update awaits approval.
  void DownloadFullInstaller();

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
      std::vector<std::string> deletions = {},
      std::string expected_base_version = {},
      std::string expected_target_version = {});

  UpdateState GetState() const { return state_; }
  const std::string& GetLastError() const { return last_error_; }
  const std::optional<UpdateManifest>& GetManifest() const { return manifest_; }
  const std::optional<DownloadProgress>& GetDownloadProgress() const {
    return download_progress_;
  }
  base::FilePath GetStagedPath() const { return staged_output_file_; }
  bool GetStagedIsInstallerPackage() const {
    return staged_is_installer_package_;
  }
  bool GetInstallerOpened() const { return installer_opened_; }
  bool GetRequiresElevation() const { return requires_elevation_; }
  bool GetElevationApprovalRequired() const {
    return elevation_approval_required_;
  }

  using PrivilegedInstaller = base::RepeatingCallback<void(
      base::FilePath,
      base::FilePath,
      base::FilePath,
      base::OnceCallback<void(bool, bool, std::string)>)>;
  void SetPrivilegedInstallerForTesting(PrivilegedInstaller installer) {
    privileged_installer_for_testing_ = std::move(installer);
  }

  using InstallerPackageOpener = base::RepeatingCallback<
      void(base::FilePath, base::OnceCallback<void(bool, std::string)>)>;
  void SetInstallerPackageOpenerForTesting(InstallerPackageOpener opener) {
    installer_package_opener_for_testing_ = std::move(opener);
  }
  void SetInstallerPackageCacheRootForTesting(const base::FilePath& path) {
    installer_package_cache_root_for_testing_ = path;
  }

 private:
  scoped_refptr<network::SharedURLLoaderFactory> GetURLLoaderFactory();
  base::FilePath GetDefaultInstallDir() const;
  base::FilePath GetDefaultExecutable() const;

  // Resolve filesystem-dependent paths on a MayBlock worker using copied
  // configuration, without accessing this manager from another sequence.
  static std::string ResolveCurrentVersion(const std::string& version_override);
  static base::FilePath ResolveInstallDir(const base::FilePath& dir_override);
  static base::FilePath ResolveCurrentVersionDir(
      const base::FilePath& install_dir,
      const std::string& current_version);
  static base::FilePath ResolveBaseFile(const base::FilePath& version_dir,
                                        const base::FilePath& install_dir,
                                        const base::FilePath& file_override);

  void SetState(UpdateState new_state);
  void NotifyPhaseStateIfCurrent(UpdateState state, uint64_t revision);
  void NotifyError(const std::string& message);

  void StartSystemUpdate(bool check_only);
  void OnSystemUpdateEvent(const XenonSystemUpdater::Event& event);
  void OnCheckResponse(std::optional<std::string> response_body);
  void OnDownloadTempDirCreated(base::ScopedTempDir dir, bool created);
  void BeginPackageDownload();
  static DifferentialEligibilityResult EvaluateDifferentialEligibility(
      base::FilePath install_override,
      DifferentialEligibilityCallback check_for_testing);
  void OnDifferentialEligibilityChecked(uint64_t revision,
                                        DifferentialEligibilityResult result);
  void OnInstallAccessChecked(DifferentialEligibilityResult result);
  void StartDifferentialInstallation();
  void OnPrivilegedInstallInitiated(bool ready_to_quit,
                                    bool requires_approval,
                                    std::string error);
  void OnInstallInitiated(std::pair<bool, std::string> result);
  void OnInstallerPackagePrepared(base::FilePath path, std::string error);
  void OnInstallerPackageOpened(bool opened, std::string error);
  void OnLastInstallErrorRead(std::string error);
  void StartPackageDownload(const UpdatePackageInfo& package, bool is_diff);
  void OnDownloadProgress(const DownloadProgress& progress);
  void OnDownloadComplete(bool is_diff, bool success, const std::string& error);
  void OnPatchCompleteWithOutput(VersionStagingResult result);

  std::unique_ptr<XenonSystemUpdater> system_updater_;
  bool system_update_active_ = false;
  bool system_update_installed_ = false;
  UpdateState state_ = UpdateState::kIdle;
  uint64_t state_revision_ = 0;
  std::string feed_url_;
  std::string last_error_;
  bool auto_download_ = true;
  bool auto_install_on_app_quit_ = true;
  std::string current_version_override_;
  std::optional<UpdateManifest> manifest_;
  std::optional<DownloadProgress> download_progress_;

  base::FilePath install_dir_override_;
  base::FilePath base_file_override_;
  base::ScopedTempDir temp_download_dir_;
  bool download_temp_dir_ready_ = false;
  bool install_started_ = false;
  bool staged_is_installer_package_ = false;
  bool installer_opened_ = false;
  bool requires_elevation_ = false;
  bool elevation_approval_required_ = false;
  uint64_t download_attempt_ = 0;
  base::FilePath downloaded_file_;
  base::FilePath staged_output_file_;

  std::unique_ptr<network::SimpleURLLoader> check_loader_;
  std::unique_ptr<XenonUpdateDownloader> downloader_;
  scoped_refptr<network::SharedURLLoaderFactory> test_url_loader_factory_;
  DifferentialEligibilityCallback differential_eligibility_for_testing_;
  InstallerPackageOpener installer_package_opener_for_testing_;
  base::FilePath installer_package_cache_root_for_testing_;
  PrivilegedInstaller privileged_installer_for_testing_;

  base::ObserverList<Observer> observers_;
  base::WeakPtrFactory<XenonUpdateManager> weak_factory_{this};
};

}  // namespace xenon::updater

#endif  // XENON_OVERLAY_CHROME_BROWSER_UPDATER_XENON_UPDATE_MANAGER_H_
