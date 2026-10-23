// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/updater/xenon_update_manager.h"

#include <algorithm>
#include <array>
#include <set>
#include <utility>
#include <vector>

#include "base/files/file.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/process/process.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/task_traits.h"
#include "base/task/thread_pool.h"
#include "base/values.h"
#include "base/version.h"
#include "chrome/common/chrome_constants.h"
#include "components/version_info/version_info.h"
#include "crypto/secure_hash.h"
#if BUILDFLAG(IS_WIN)
#include <windows.h>

#include "base/win/registry.h"
#elif BUILDFLAG(IS_MAC)
#include "base/apple/bundle_locations.h"
#include "base/apple/foundation_util.h"
#endif
#include "net/base/load_flags.h"
#include "net/base/url_util.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "third_party/zlib/google/zip.h"
#include "xenon_overlay/chrome/browser/updater/xenon_update_browser_context.h"
#include "xenon_overlay/chrome/browser/updater/xenon_update_installer.h"
#include "xenon_overlay/chrome/browser/updater/xenon_update_patcher.h"

namespace xenon::updater {

namespace {

bool ContentSha256Matches(const base::FilePath& path,
                          const std::string& expected_sha256) {
  if (expected_sha256.empty()) {
    return false;
  }
  base::File file(path, base::File::FLAG_OPEN | base::File::FLAG_READ);
  if (!file.IsValid()) {
    return false;
  }
  std::unique_ptr<crypto::SecureHash> hash =
      crypto::SecureHash::Create(crypto::SecureHash::SHA256);
  std::vector<uint8_t> buffer(64 * 1024);
  while (true) {
    std::optional<size_t> bytes_read =
        file.ReadAtCurrentPosNoBestEffort(base::span(buffer));
    if (!bytes_read.has_value()) {
      return false;
    }
    if (*bytes_read == 0) {
      break;
    }
    hash->Update(base::span(buffer).first(*bytes_read));
  }
  std::array<uint8_t, 32> digest;
  hash->Finish(digest);
  return base::EqualsCaseInsensitiveASCII(base::HexEncode(digest),
                                          expected_sha256);
}

bool InstallStagedFile(const base::FilePath& from, const base::FilePath& to) {
  if (!base::CreateDirectory(to.DirName())) {
    return false;
  }
#if !BUILDFLAG(IS_WIN)
  if (base::IsLink(from)) {
    base::FilePath link_target;
    if (!base::ReadSymbolicLink(from, &link_target)) {
      return false;
    }
    base::DeleteFile(to);
    return base::CreateSymbolicLink(link_target, to);
  }
#endif
  return base::CopyFile(from, to);
}

std::string UpdatePlatformName() {
#if BUILDFLAG(IS_MAC)
  return "mac";
#elif BUILDFLAG(IS_WIN)
  return "win";
#else
  return "linux";
#endif
}

constexpr net::NetworkTrafficAnnotationTag kCheckTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("xenon_update_check", R"(
        semantics {
          sender: "Xenon Application Updater"
          description: "Checks for application updates."
          trigger: "User requested update or automatic background update check."
          data: "None."
          destination: OTHER
        }
        policy {
          cookies_allowed: NO
          setting: "This feature cannot be disabled by settings."
          policy_exception_justification: "Essential for application updates."
        })");

}  // namespace

// static
XenonUpdateManager* XenonUpdateManager::GetInstance() {
  static base::NoDestructor<XenonUpdateManager> instance;
  return instance.get();
}

XenonUpdateManager::XenonUpdateManager()
    : system_updater_(CreateXenonSystemUpdater()),
      downloader_(std::make_unique<XenonUpdateDownloader>()) {}

XenonUpdateManager::~XenonUpdateManager() {
  check_loader_.reset();
  downloader_.reset();
}

void XenonUpdateManager::AddObserver(Observer* observer) {
  observers_.AddObserver(observer);
}

void XenonUpdateManager::RemoveObserver(Observer* observer) {
  observers_.RemoveObserver(observer);
}

void XenonUpdateManager::SetFeedURL(const std::string& url) {
  if (system_updater_) {
    return;
  }
  feed_url_ = url;
}

std::string XenonUpdateManager::GetFeedURL() const {
  return system_updater_ ? system_updater_->GetFeedURL() : feed_url_;
}

void XenonUpdateManager::SetAutoDownload(bool auto_download) {
  auto_download_ = auto_download;
}

bool XenonUpdateManager::GetAutoDownload() const {
  return auto_download_;
}

std::string XenonUpdateManager::GetCurrentVersion() const {
  return ResolveCurrentVersion(current_version_override_);
}

// static
std::string XenonUpdateManager::ResolveCurrentVersion(
    const std::string& version_override) {
  if (!version_override.empty()) {
    return version_override;
  }
#if BUILDFLAG(IS_MAC)
  // The framework Versions/ directory is Chromium's milestone (for example
  // 153.0.8004.0). Product updates compare against the app bundle version.
  base::FilePath bundle_path = base::apple::OuterBundlePath();
  if (bundle_path.empty()) {
    base::FilePath exe_dir;
    if (base::PathService::Get(base::DIR_EXE, &exe_dir)) {
      bundle_path = exe_dir.DirName().DirName();
    }
  }
  std::string bundle_version =
      XenonUpdateInstaller::ReadBundleShortVersion(bundle_path);
  if (base::Version(bundle_version).IsValid()) {
    return bundle_version;
  }
#endif
  base::FilePath module_dir;
  if (base::PathService::Get(base::DIR_MODULE, &module_dir)) {
    std::string dir_version = module_dir.BaseName().MaybeAsASCII();
    if (base::Version(dir_version).IsValid()) {
      return dir_version;
    }
  }
  return std::string(chrome::kChromeVersion);
}

void XenonUpdateManager::SetAutoInstallOnAppQuit(bool enabled) {
  auto_install_on_app_quit_ = enabled;
}

bool XenonUpdateManager::GetAutoInstallOnAppQuit() const {
  return auto_install_on_app_quit_;
}

void XenonUpdateManager::SetCurrentVersionForTesting(
    const std::string& version) {
  current_version_override_ = version;
}

void XenonUpdateManager::SetURLLoaderFactoryForTesting(
    scoped_refptr<network::SharedURLLoaderFactory> factory) {
  test_url_loader_factory_ = std::move(factory);
}

void XenonUpdateManager::SetBaseFileForTesting(
    const base::FilePath& base_file) {
  base_file_override_ = base_file;
}

void XenonUpdateManager::SetInstallDirForTesting(const base::FilePath& dir) {
  install_dir_override_ = dir;
}

scoped_refptr<network::SharedURLLoaderFactory>
XenonUpdateManager::GetURLLoaderFactory() {
  if (test_url_loader_factory_) {
    return test_url_loader_factory_;
  }
  return GetUpdateURLLoaderFactory();
}

base::FilePath XenonUpdateManager::GetDefaultInstallDir() const {
  return ResolveInstallDir(install_dir_override_);
}

// static
base::FilePath XenonUpdateManager::ResolveInstallDir(
    const base::FilePath& dir_override) {
  if (!dir_override.empty()) {
    return dir_override;
  }

#if BUILDFLAG(IS_WIN)
  // Check registry for installed application directory:
  // "有安装目录就先安装目录, 没有的话才是运行目录"
  base::FilePath current_exe;
  std::wstring product_name;
  if (base::PathService::Get(base::FILE_EXE, &current_exe)) {
    product_name = current_exe.BaseName().RemoveFinalExtension().value();
  }
  std::vector<std::wstring> reg_subkeys;
#if defined(CUSTOM_CHROME_INSTALLATION_REG_KEY_W)
  reg_subkeys.push_back(CUSTOM_CHROME_INSTALLATION_REG_KEY_W);
#endif
  if (!product_name.empty()) {
    reg_subkeys.push_back(L"Software\\" + product_name);
  }
  reg_subkeys.push_back(L"Software\\Chromium");

  for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
    for (const auto& subkey : reg_subkeys) {
      base::win::RegKey reg_key;
      if (reg_key.Open(root, subkey.c_str(),
                       KEY_QUERY_VALUE | KEY_WOW64_32KEY) == ERROR_SUCCESS ||
          reg_key.Open(root, subkey.c_str(), KEY_QUERY_VALUE) ==
              ERROR_SUCCESS) {
        std::wstring uninstall_str;
        if (reg_key.ReadValue(L"UninstallString", &uninstall_str) ==
                ERROR_SUCCESS &&
            !uninstall_str.empty()) {
          base::FilePath setup_path(uninstall_str);
          // setup.exe is in <InstallDir>/<Version>/Installer/setup.exe
          base::FilePath install_dir = setup_path.DirName().DirName().DirName();
          if (base::DirectoryExists(install_dir)) {
            return install_dir;
          }
        }

        std::wstring launch_cmd;
        if (reg_key.ReadValue(L"InstallerSuccessLaunchCmdLine", &launch_cmd) ==
                ERROR_SUCCESS &&
            !launch_cmd.empty()) {
          std::wstring exe_str = launch_cmd;
          if (!exe_str.empty() && exe_str.front() == L'"') {
            size_t end_quote = exe_str.find(L'"', 1);
            if (end_quote != std::wstring::npos) {
              exe_str = exe_str.substr(1, end_quote - 1);
            }
          }
          base::FilePath exe_path(exe_str);
          base::FilePath install_dir = exe_path.DirName();
          if (base::DirectoryExists(install_dir)) {
            return install_dir;
          }
        }
      }
    }
  }
#elif BUILDFLAG(IS_MAC)
  base::FilePath bundle_path = base::apple::OuterBundlePath();
  if (!bundle_path.empty()) {
    return bundle_path;
  }
#endif

  base::FilePath exe_dir;
  base::PathService::Get(base::DIR_EXE, &exe_dir);
  return exe_dir;
}

base::FilePath XenonUpdateManager::GetCurrentVersionDir() const {
  return ResolveCurrentVersionDir(GetDefaultInstallDir(), GetCurrentVersion());
}

// static
base::FilePath XenonUpdateManager::ResolveCurrentVersionDir(
    const base::FilePath& install_dir,
    const std::string& current_version) {
#if BUILDFLAG(IS_MAC)
  return install_dir;
#else
  base::FilePath version_dir = install_dir.AppendASCII(current_version);
  if (base::DirectoryExists(version_dir)) {
    return version_dir;
  }
  return install_dir;
#endif
}

base::FilePath XenonUpdateManager::GetTargetVersionDir(
    const std::string& target_version) const {
  base::FilePath install_dir = GetDefaultInstallDir();
#if BUILDFLAG(IS_MAC)
  base::FilePath temp_dir;
  if (base::GetTempDir(&temp_dir)) {
    return temp_dir.AppendASCII("xenon_update_" + target_version)
        .Append(install_dir.BaseName());
  }
  return install_dir.DirName().Append(
      install_dir.BaseName().RemoveFinalExtension().value() + "_" +
      target_version + ".app");
#else
  return install_dir.AppendASCII(target_version);
#endif
}

// static
base::FilePath XenonUpdateManager::ResolveBaseFile(
    const base::FilePath& version_dir,
    const base::FilePath& install_dir,
    const base::FilePath& file_override) {
  if (!file_override.empty()) {
    return file_override;
  }

  // 1. Dynamically resolve from current runtime module
  base::FilePath module_file;
  if (base::PathService::Get(base::FILE_MODULE, &module_file)) {
    base::FilePath candidate = version_dir.Append(module_file.BaseName());
    if (base::PathExists(candidate)) {
      return candidate;
    }
    candidate = install_dir.Append(module_file.BaseName());
    if (base::PathExists(candidate)) {
      return candidate;
    }
    return version_dir.Append(module_file.BaseName());
  }

  // 2. Fallback to build configuration constants without hardcoding literals
#if BUILDFLAG(IS_WIN)
  return version_dir.Append(chrome::kBrowserResourcesDll);
#elif BUILDFLAG(IS_MAC)
  return version_dir.Append(chrome::kFrameworkName);
#else
  return version_dir.Append(chrome::kBrowserProcessExecutableName);
#endif
}

base::FilePath XenonUpdateManager::GetDefaultExecutable() const {
#if BUILDFLAG(IS_WIN)
  base::FilePath current_exe;
  if (base::PathService::Get(base::FILE_EXE, &current_exe)) {
    base::FilePath install_dir = GetDefaultInstallDir();
    base::FilePath candidate = install_dir.Append(current_exe.BaseName());
    if (base::PathExists(candidate)) {
      return candidate;
    }
  }
#elif BUILDFLAG(IS_MAC)
  base::FilePath bundle_path = base::apple::OuterBundlePath();
  if (!bundle_path.empty()) {
    return bundle_path;
  }
#endif
  base::FilePath exe_path;
  base::PathService::Get(base::FILE_EXE, &exe_path);
  return exe_path;
}

void XenonUpdateManager::SetState(UpdateState new_state) {
  state_ = new_state;
  ++state_revision_;
  // DownloadUpdate can be called from OnUpdateAvailable. Defer phase events
  // until that observer iteration has unwound; do not permit nested iteration.
  if (new_state == UpdateState::kDownloading ||
      new_state == UpdateState::kPatching) {
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(&XenonUpdateManager::NotifyPhaseStateIfCurrent,
                       weak_factory_.GetWeakPtr(), new_state, state_revision_));
  }
}

void XenonUpdateManager::NotifyPhaseStateIfCurrent(UpdateState state,
                                                   uint64_t revision) {
  // A fast failure, completed preparation, or new download attempt can replace
  // the phase before this task runs. Never publish an older phase afterwards.
  if (state_ != state || state_revision_ != revision) {
    return;
  }
  for (auto& observer : observers_) {
    observer.OnUpdateStateChanged(state);
  }
}

void XenonUpdateManager::NotifyError(const std::string& message) {
  last_error_ = message;
  SetState(UpdateState::kError);
  for (auto& observer : observers_) {
    observer.OnUpdateError(message);
  }
}

void XenonUpdateManager::StartSystemUpdate(bool check_only) {
  last_error_.clear();
  manifest_.reset();
  download_progress_.reset();
  system_update_active_ = !check_only;
  system_update_installed_ = false;
  SetState(UpdateState::kCheckingForUpdate);
  for (auto& observer : observers_) {
    observer.OnCheckingForUpdate();
  }
  system_updater_->Start(
      check_only, base::BindRepeating(&XenonUpdateManager::OnSystemUpdateEvent,
                                      weak_factory_.GetWeakPtr()));
}

void XenonUpdateManager::OnSystemUpdateEvent(
    const XenonSystemUpdater::Event& event) {
  using Phase = XenonSystemUpdater::Event::Phase;
  if (!event.version.empty()) {
    if (!manifest_) {
      manifest_.emplace();
    }
    manifest_->target_version = event.version;
  }
  switch (event.phase) {
    case Phase::kChecking:
      SetState(UpdateState::kCheckingForUpdate);
      break;
    case Phase::kAvailable:
      if (!manifest_) {
        NotifyError("Xenon Updater 未返回目标版本");
        return;
      }
      SetState(UpdateState::kUpdateAvailable);
      for (auto& observer : observers_) {
        observer.OnUpdateAvailable(*manifest_);
      }
      break;
    case Phase::kDownloading: {
      SetState(UpdateState::kDownloading);
      DownloadProgress progress;
      progress.bytes_downloaded = std::max<int64_t>(0, event.downloaded);
      progress.total_bytes = std::max<int64_t>(0, event.total);
      if (progress.total_bytes > 0) {
        progress.percent = static_cast<int>(
            std::clamp(100.0 * progress.bytes_downloaded / progress.total_bytes,
                       0.0, 100.0));
      }
      OnDownloadProgress(progress);
      break;
    }
    case Phase::kPreparing:
      SetState(UpdateState::kPatching);
      break;
    case Phase::kAwaitingExit:
    case Phase::kInstalled:
      if (!manifest_) {
        NotifyError("Xenon Updater 未返回目标版本");
        return;
      }
      system_update_installed_ = event.phase == Phase::kInstalled;
      SetState(UpdateState::kUpdateDownloaded);
      for (auto& observer : observers_) {
        observer.OnUpdateDownloaded(*manifest_);
      }
      break;
    case Phase::kNoUpdate:
      system_update_active_ = false;
      SetState(UpdateState::kUpdateNotAvailable);
      for (auto& observer : observers_) {
        observer.OnUpdateNotAvailable(GetCurrentVersion());
      }
      break;
    case Phase::kError:
      system_update_active_ = false;
      NotifyError(event.error);
      break;
  }
}

void XenonUpdateManager::CheckForUpdates(const std::string& feed_url) {
  if (system_updater_) {
    if (system_update_active_ || state_ == UpdateState::kCheckingForUpdate ||
        state_ == UpdateState::kDownloading ||
        state_ == UpdateState::kPatching ||
        state_ == UpdateState::kUpdateDownloaded || install_started_) {
      return;
    }
    if (!feed_url.empty() && feed_url != system_updater_->GetFeedURL()) {
      NotifyError("系统级更新源由安装包配置，不能在浏览器中更改");
      return;
    }
    StartSystemUpdate(!auto_download_);
    return;
  }
  // A manifest belongs to one operation until installation or failure. Never
  // let a second check change the inputs of an in-flight download/patch.
  if (install_started_ || state_ == UpdateState::kCheckingForUpdate ||
      state_ == UpdateState::kDownloading || state_ == UpdateState::kPatching ||
      state_ == UpdateState::kUpdateDownloaded) {
    return;
  }
  manifest_.reset();
  download_progress_.reset();
  last_error_.clear();
  staged_output_file_.clear();
  staged_is_installer_package_ = false;
  installer_opened_ = false;
  requires_elevation_ = false;
  elevation_approval_required_ = false;
  if (!feed_url.empty()) {
    feed_url_ = feed_url;
  }
  if (feed_url_.empty()) {
    NotifyError("No update feed URL configured");
    return;
  }

  GURL url(feed_url_);
  if (!url.is_valid()) {
    NotifyError("Invalid update feed URL: " + feed_url_);
    return;
  }
  std::string ignored_query;
  if (!net::GetValueForKeyInQuery(url, "platform", &ignored_query)) {
    url = net::AppendQueryParameter(url, "platform", UpdatePlatformName());
  }
  if (!net::GetValueForKeyInQuery(url, "version", &ignored_query)) {
    url = net::AppendQueryParameter(url, "version", GetCurrentVersion());
  }

  auto factory = GetURLLoaderFactory();
  if (!factory) {
    NotifyError("Network context is not available for update check");
    return;
  }

  SetState(UpdateState::kCheckingForUpdate);
  for (auto& observer : observers_) {
    observer.OnCheckingForUpdate();
  }

  auto request = std::make_unique<network::ResourceRequest>();
  request->url = url;
  request->method = "GET";
  request->load_flags = net::LOAD_BYPASS_CACHE | net::LOAD_DISABLE_CACHE;
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;

  check_loader_ = network::SimpleURLLoader::Create(std::move(request),
                                                   kCheckTrafficAnnotation);
  check_loader_->DownloadToString(
      factory.get(),
      base::BindOnce(&XenonUpdateManager::OnCheckResponse,
                     weak_factory_.GetWeakPtr()),
      1024 * 1024 /* 1MB max response size */);
}

void XenonUpdateManager::OnCheckResponse(
    std::optional<std::string> response_body) {
  int net_error = check_loader_ ? check_loader_->NetError() : net::ERR_FAILED;
  check_loader_.reset();

  if (!response_body || net_error != net::OK) {
    NotifyError("Failed to fetch update information: " +
                net::ErrorToString(net_error));
    return;
  }

  auto manifest = UpdateManifest::FromJson(*response_body);
  if (!manifest) {
    NotifyError("Malformed update manifest response");
    return;
  }

  manifest_ = std::move(manifest);

  base::Version current_version(GetCurrentVersion());
  base::Version target_version(manifest_->target_version);

  if (!target_version.IsValid() ||
      (current_version.IsValid() && target_version <= current_version)) {
    manifest_.reset();
    SetState(UpdateState::kUpdateNotAvailable);
    for (auto& observer : observers_) {
      observer.OnUpdateNotAvailable(GetCurrentVersion());
    }
    return;
  }

  SetState(UpdateState::kUpdateAvailable);
  const UpdateManifest available = *manifest_;
  for (auto& observer : observers_) {
    observer.OnUpdateAvailable(available);
  }

  if (auto_download_ && state_ == UpdateState::kUpdateAvailable && manifest_ &&
      manifest_->target_version == available.target_version) {
    DownloadUpdate();
  }
}

void XenonUpdateManager::DownloadUpdate() {
  if (system_updater_) {
    if (state_ == UpdateState::kUpdateAvailable && !system_update_active_) {
      StartSystemUpdate(false);
    }
    return;
  }
  if (state_ == UpdateState::kDownloading || state_ == UpdateState::kPatching ||
      state_ == UpdateState::kUpdateDownloaded) {
    LOG(WARNING)
        << "[XenonUpdateManager] DownloadUpdate ignored while in state: "
        << UpdateStateToString(state_);
    return;
  }

  if (state_ != UpdateState::kUpdateAvailable &&
      state_ != UpdateState::kError) {
    return;
  }
  if (!manifest_) {
    NotifyError("No available update manifest to download");
    return;
  }

  // Reserve the operation before asynchronous temporary-directory creation.
  last_error_.clear();
  staged_is_installer_package_ = false;
  installer_opened_ = false;
  requires_elevation_ = false;
  elevation_approval_required_ = false;
  download_progress_.reset();
  SetState(UpdateState::kDownloading);
  if (!download_temp_dir_ready_) {
    // CreateUniqueTempDir is blocking and WebUI invokes this on the UI thread.
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce([]() {
          base::ScopedTempDir dir;
          const bool created = dir.CreateUniqueTempDir();
          return std::make_pair(std::move(dir), created);
        }),
        base::BindOnce(
            [](base::WeakPtr<XenonUpdateManager> self,
               std::pair<base::ScopedTempDir, bool> result) {
              if (!self) {
                return;
              }
              self->OnDownloadTempDirCreated(std::move(result.first),
                                             result.second);
            },
            weak_factory_.GetWeakPtr()));
    return;
  }

  BeginPackageDownload();
}

void XenonUpdateManager::OnDownloadTempDirCreated(base::ScopedTempDir dir,
                                                  bool created) {
  if (!created) {
    NotifyError("Failed to create temporary directory for update download");
    return;
  }
  temp_download_dir_ = std::move(dir);
  download_temp_dir_ready_ = true;
  BeginPackageDownload();
}

void XenonUpdateManager::DownloadFullInstaller() {
  if (system_updater_) {
    // The service chooses differential/full pipelines and retries safely.
    return;
  }
  if (install_started_ || state_ != UpdateState::kUpdateDownloaded ||
      !elevation_approval_required_ || !manifest_ || !manifest_->is_diff ||
      !manifest_->full_package) {
    return;
  }
  manifest_->is_diff = false;
  staged_output_file_.clear();
  last_error_.clear();
  StartPackageDownload(*manifest_->full_package, /*is_diff=*/false);
}

void XenonUpdateManager::BeginPackageDownload() {
  if (manifest_->is_diff && manifest_->diff_package) {
#if BUILDFLAG(IS_MAC)
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce(&XenonUpdateManager::EvaluateDifferentialEligibility,
                       install_dir_override_,
                       differential_eligibility_for_testing_),
        base::BindOnce(&XenonUpdateManager::OnDifferentialEligibilityChecked,
                       weak_factory_.GetWeakPtr(), state_revision_));
#else
    StartPackageDownload(*manifest_->diff_package, /*is_diff=*/true);
#endif
  } else if (manifest_->full_package) {
    StartPackageDownload(*manifest_->full_package, /*is_diff=*/false);
  } else {
    NotifyError("Manifest does not specify valid package info");
  }
}

// static
XenonUpdateManager::DifferentialEligibilityResult
XenonUpdateManager::EvaluateDifferentialEligibility(
    base::FilePath install_override,
    DifferentialEligibilityCallback check_for_testing) {
  const base::FilePath target = ResolveInstallDir(install_override);
  if (check_for_testing) {
    return check_for_testing.Run(target);
  }
#if BUILDFLAG(IS_MAC)
  DifferentialEligibilityResult result;
  result.allowed =
      XenonUpdateInstaller::CanInstallDifferentially(target, &result.reason);
  result.requires_elevation =
      result.allowed && XenonUpdateInstaller::RequiresElevation(target);
  return result;
#else
  return {true, false, {}};
#endif
}

void XenonUpdateManager::OnDifferentialEligibilityChecked(
    uint64_t revision,
    DifferentialEligibilityResult result) {
  if (state_ != UpdateState::kDownloading || state_revision_ != revision ||
      !manifest_) {
    return;
  }
  if (result.allowed) {
    requires_elevation_ = result.requires_elevation;
    StartPackageDownload(*manifest_->diff_package, /*is_diff=*/true);
    return;
  }
  if (!manifest_->full_package) {
    NotifyError(
        "Differential installation is unavailable and no full installer "
        "package was provided: " +
        result.reason);
    return;
  }
  LOG(WARNING) << "[XenonUpdateManager] Differential installation unavailable: "
               << result.reason << ". Downloading the full installer package.";
  manifest_->is_diff = false;
  StartPackageDownload(*manifest_->full_package, /*is_diff=*/false);
}

void XenonUpdateManager::StartPackageDownload(const UpdatePackageInfo& package,
                                              bool is_diff) {
  auto factory = GetURLLoaderFactory();
  if (!factory) {
    NotifyError("Network context is not available for update download");
    return;
  }

  if (!is_diff) {
    requires_elevation_ = false;
    elevation_approval_required_ = false;
  }
  download_progress_ = DownloadProgress{};
  download_progress_->total_bytes = package.size;
  SetState(UpdateState::kDownloading);

  std::string filename;
  std::string_view url_path = package.url.path();
#if BUILDFLAG(IS_MAC)
  if (!is_diff) {
    if (!base::EndsWith(url_path, ".pkg",
                        base::CompareCase::INSENSITIVE_ASCII)) {
      NotifyError(
          "macOS full updates require a .pkg installer package; "
          "ZIP application bundles are not supported");
      return;
    }
    filename = "update_package.pkg";
  } else
#endif
      if (base::EndsWith(url_path, ".zip",
                         base::CompareCase::INSENSITIVE_ASCII)) {
    filename = "update_patch.zip";
  } else if (base::EndsWith(url_path, ".zucc",
                            base::CompareCase::INSENSITIVE_ASCII)) {
    filename = "update_patch.zucc";
  } else if (!is_diff && base::EndsWith(url_path, ".exe",
                                        base::CompareCase::INSENSITIVE_ASCII)) {
    filename = "update_package.exe";
  } else {
    NotifyError("Unsupported update package type");
    return;
  }
  filename = base::NumberToString(++download_attempt_) + "_" + filename;
  downloaded_file_ = temp_download_dir_.GetPath().AppendASCII(filename);

  downloader_->StartDownload(
      std::move(factory), package, downloaded_file_,
      base::BindRepeating(&XenonUpdateManager::OnDownloadProgress,
                          weak_factory_.GetWeakPtr()),
      base::BindOnce(&XenonUpdateManager::OnDownloadComplete,
                     weak_factory_.GetWeakPtr(), is_diff));
}

void XenonUpdateManager::OnDownloadProgress(const DownloadProgress& progress) {
  download_progress_ = progress;
  for (auto& observer : observers_) {
    observer.OnDownloadProgress(progress);
  }
}

void XenonUpdateManager::OnDownloadComplete(bool is_diff,
                                            bool success,
                                            const std::string& error) {
  if (!success) {
    if (is_diff && manifest_->full_package) {
      LOG(WARNING) << "[XenonUpdateManager] Differential download failed: "
                   << error << ". Falling back to full package download.";
      manifest_->is_diff = false;
      StartPackageDownload(*manifest_->full_package, /*is_diff=*/false);
      return;
    }
    NotifyError("Download failed: " + error);
    return;
  }

  if (is_diff) {
    // Resolving paths may inspect files/registry entries. Copy configuration
    // here and resolve it in the worker, before reconstructing the update.
    SetState(UpdateState::kPatching);
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce(
            [](base::FilePath install_override, base::FilePath base_override,
               std::string version_override, base::FilePath downloaded,
               base::FilePath download_dir, std::string target_version,
               std::vector<std::string> deletions) {
              const base::FilePath install_dir =
                  ResolveInstallDir(install_override);
              const std::string current_version =
                  ResolveCurrentVersion(version_override);
              const base::FilePath current_dir =
                  ResolveCurrentVersionDir(install_dir, current_version);
              const base::FilePath base_file =
                  ResolveBaseFile(current_dir, install_dir, base_override);
#if BUILDFLAG(IS_MAC)
              const base::FilePath target_dir =
                  download_dir.AppendASCII("prepared")
                      .Append(current_dir.BaseName());
#elif BUILDFLAG(IS_WIN)
              const base::FilePath target_dir =
                  download_dir.AppendASCII("versions")
                      .AppendASCII(target_version);
#else
              const base::FilePath target_dir =
                  install_dir.AppendASCII(target_version);
#endif
              return PrepareVersionDirectory(
                  current_dir, base_file, downloaded, target_dir,
                  download_dir.AppendASCII("staging_version"),
                  std::move(deletions), current_version, target_version);
            },
            install_dir_override_, base_file_override_,
            current_version_override_, downloaded_file_,
            temp_download_dir_.GetPath(), manifest_->target_version,
            manifest_->deletions),
        base::BindOnce(&XenonUpdateManager::OnPatchCompleteWithOutput,
                       weak_factory_.GetWeakPtr()));
    return;
  }

#if BUILDFLAG(IS_MAC)
  if (downloaded_file_.MatchesExtension(FILE_PATH_LITERAL(".pkg"))) {
    // SHA-256 and size were verified by the downloader. The system Installer
    // consumes this file; it must never be unpacked or swapped as an app
    // bundle.
    staged_output_file_ = downloaded_file_;
    staged_is_installer_package_ = true;
    SetState(UpdateState::kUpdateDownloaded);
    for (auto& observer : observers_) {
      observer.OnUpdateDownloaded(*manifest_);
    }
    return;
  }
#else
  if (downloaded_file_.MatchesExtension(FILE_PATH_LITERAL(".zip"))) {
    SetState(UpdateState::kPatching);
    const base::FilePath unpacked = temp_download_dir_.GetPath().AppendASCII(
        "full_" + base::NumberToString(download_attempt_));
    const std::string version = manifest_->target_version;
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce(
            [](base::FilePath downloaded, base::FilePath unpacked,
               std::string version) {
              VersionStagingResult result;
              if (!zip::Unzip(downloaded, unpacked, {},
                              zip::UnzipSymlinkOption::DONT_PRESERVE)) {
                result.error = "Failed to unpack full update package";
                base::DeletePathRecursively(unpacked);
                return result;
              }
#if BUILDFLAG(IS_WIN)
              base::FilePath root = unpacked;
              if (base::DirectoryExists(root.AppendASCII("Chrome-bin"))) {
                root = root.AppendASCII("Chrome-bin");
              }
              if (!base::DirectoryExists(root.AppendASCII(version))) {
                result.error = "Full package is missing the target version";
                return result;
              }
              result.staged_dir = root;
#else
              result.error = "Full directory installation is not supported";
              return result;
#endif
              result.success = true;
              return result;
            },
            downloaded_file_, unpacked, version),
        base::BindOnce(
            [](base::WeakPtr<XenonUpdateManager> self,
               VersionStagingResult result) {
              if (!self) {
                return;
              }
              if (!result.success) {
                self->NotifyError(result.error);
                return;
              }
              self->OnPatchCompleteWithOutput(std::move(result));
            },
            weak_factory_.GetWeakPtr()));
    return;
  }
#if BUILDFLAG(IS_WIN)
  if (downloaded_file_.MatchesExtension(FILE_PATH_LITERAL(".exe"))) {
    staged_output_file_ = downloaded_file_;
    SetState(UpdateState::kUpdateDownloaded);
    for (auto& observer : observers_) {
      observer.OnUpdateDownloaded(*manifest_);
    }
    return;
  }
#endif
#endif
  NotifyError("Unsupported full update package");
}

// static
XenonUpdateManager::VersionStagingResult
XenonUpdateManager::PrepareVersionDirectory(
    base::FilePath current_version_dir,
    base::FilePath base_file,
    base::FilePath patch_file,
    base::FilePath target_version_dir,
    base::FilePath temp_staging_dir,
    std::vector<std::string> deletions,
    std::string expected_base_version,
    std::string expected_target_version) {
  VersionStagingResult result;

  if (base::PathExists(temp_staging_dir)) {
    base::DeletePathRecursively(temp_staging_dir);
  }
  if (!base::CreateDirectory(temp_staging_dir)) {
    result.error = "Failed to create temporary staging directory";
    return result;
  }

  base::FilePath install_dir = target_version_dir.DirName();
  base::FilePath current_exe;
  if (base::PathService::Get(base::FILE_EXE, &current_exe)) {
    base::FilePath candidate = install_dir.Append(current_exe.BaseName());
    if (base::PathExists(candidate)) {
      current_exe = candidate;
    }
  }

  // 1. Resolve differential patches (supports .zip bundle or direct .zucc)
  base::FilePath patch_dir =
      patch_file.DirName().AppendASCII("extracted_patches");
  if (base::PathExists(patch_dir)) {
    base::DeletePathRecursively(patch_dir);
  }
  base::CreateDirectory(patch_dir);

  bool is_zip = zip::Unzip(patch_file, patch_dir);
  base::FilePath manifest_path = patch_dir.AppendASCII("manifest.json");

  LOG(INFO) << "[XenonUpdateManager] is_zip=" << is_zip
            << ", patch_file=" << patch_file.value()
            << ", manifest_exists=" << base::PathExists(manifest_path);

  if (is_zip && base::PathExists(manifest_path)) {
    auto fail = [&](const std::string& error) {
      result.error = error;
      base::DeletePathRecursively(temp_staging_dir);
      base::DeletePathRecursively(patch_dir);
      return result;
    };
    std::string manifest_content;
    if (!base::ReadFileToStringWithMaxSize(manifest_path, &manifest_content,
                                           16 * 1024 * 1024)) {
      return fail("Cannot read patch manifest");
    }
    auto parsed =
        base::JSONReader::ReadDict(manifest_content, base::JSON_PARSE_RFC);
    if (!parsed) {
      return fail("Invalid patch manifest");
    }
    const std::string* base_version = parsed->FindString("base_version");
    const std::string* target_version = parsed->FindString("target_version");
    const std::string* platform = parsed->FindString("platform");
    if (!base_version || !target_version ||
        !base::Version(*base_version).IsValid() ||
        !base::Version(*target_version).IsValid() ||
        base::Version(*target_version) <= base::Version(*base_version) ||
        (!expected_base_version.empty() &&
         *base_version != expected_base_version) ||
        (!expected_target_version.empty() &&
         *target_version != expected_target_version) ||
        (!expected_target_version.empty() &&
         (!platform || *platform != UpdatePlatformName()))) {
      return fail("Patch base version, target version or platform mismatch");
    }
    const bool bundle = platform && *platform == "mac";
    const base::FilePath source_root =
        bundle ? current_version_dir : current_version_dir.DirName();
    const std::string version_prefix = *target_version + "/";
    const auto* actions = parsed->FindList("actions");
    if (!actions || actions->empty()) {
      return fail("Patch manifest contains no actions");
    }

    // Do not follow a symlink while reading bases or payloads. Destination
    // links are created last, so they can never redirect a later file write.
    auto safe_path = [](const std::string& relative) {
      if (relative.empty() || relative.front() == '/' ||
          relative.find('\\') != std::string::npos ||
          relative.find(':') != std::string::npos ||
          relative.find('\0') != std::string::npos) {
        return false;
      }
      size_t start = 0;
      do {
        size_t end = relative.find('/', start);
        std::string component = relative.substr(start, end - start);
        if (component.empty() || component == "." || component == ".." ||
            base::StartsWith(component, ".xenon-")) {
          return false;
        }
        if (end == std::string::npos) {
          return true;
        }
        start = end + 1;
      } while (true);
    };
    auto no_links = [](const base::FilePath& root,
                       const base::FilePath& relative) {
      base::FilePath path = root;
      for (const auto& component : relative.GetComponents()) {
        path = path.Append(component);
        if (base::IsLink(path)) {
          return false;
        }
      }
      return true;
    };
    std::set<base::FilePath> targets;
#if !BUILDFLAG(IS_WIN)
    std::vector<std::pair<base::FilePath, base::FilePath>> links;
#endif
    for (const auto& value : *actions) {
      if (!value.is_dict()) {
        return fail("Invalid action in patch manifest");
      }
      const auto& action = value.GetDict();
      const std::string* target = action.FindString("target");
      const std::string* type = action.FindString("action");
      if (!target || !type || !safe_path(*target)) {
        return fail("Invalid action path or type");
      }
      base::FilePath relative = base::FilePath::FromUTF8Unsafe(*target);
#if BUILDFLAG(IS_WIN)
      // Win32 ignores trailing dots/spaces and treats names case-insensitively.
      for (const auto& component : relative.GetComponents()) {
        if (component.back() == L'.' || component.back() == L' ') {
          return fail("Ambiguous Windows action path");
        }
      }
      base::FilePath key =
          base::FilePath::FromUTF8Unsafe(base::ToLowerASCII(*target));
#else
      const base::FilePath& key = relative;
#endif
      if (!targets.insert(key).second) {
        return fail("Duplicate target in patch manifest");
      }
      base::FilePath dest;
      if (bundle) {
        dest = temp_staging_dir.Append(relative);
      } else if (base::StartsWith(*target, version_prefix)) {
        dest = temp_staging_dir.Append(base::FilePath::FromUTF8Unsafe(
            target->substr(version_prefix.size())));
      } else {
        // Root files remain private until installation commits them. Never
        // change the running installation while preparing an update.
        dest = temp_staging_dir.AppendASCII(".xenon-root").Append(relative);
      }
      if (base::PathExists(dest) || base::IsLink(dest) ||
          !base::CreateDirectory(dest.DirName())) {
        return fail("Cannot create destination directory: " + *target);
      }
      if (*type == "symlink") {
#if BUILDFLAG(IS_WIN)
        return fail("Symbolic link actions are not supported on Windows");
#else
        const std::string* link = action.FindString("link");
        if (!bundle || !link || link->empty() || link->front() == '/' ||
            link->find('\\') != std::string::npos ||
            link->find('\0') != std::string::npos) {
          return fail("Invalid symbolic link: " + *target);
        }
        // Resolve '..' lexically against the link's parent, without allowing
        // the result to escape the reconstructed application.
        auto components = relative.DirName().GetComponents();
        if (components.size() == 1 &&
            components.front() == FILE_PATH_LITERAL(".")) {
          components.clear();
        }
        base::FilePath link_path = base::FilePath::FromUTF8Unsafe(*link);
        for (const auto& component : link_path.GetComponents()) {
          if (component == FILE_PATH_LITERAL("..")) {
            if (components.empty()) {
              return fail("Symbolic link escapes application: " + *target);
            }
            components.pop_back();
          } else if (component != FILE_PATH_LITERAL(".")) {
            components.push_back(component);
          }
        }
        links.emplace_back(dest, link_path);
        continue;
#endif
      }
      const std::string* sha = action.FindString("sha256");
      if (!sha || sha->size() != 64) {
        return fail("Missing SHA-256 for " + *target);
      }
      base::FilePath input;
      if (*type == "copy" || *type == "patch") {
        const std::string* base = action.FindString("base");
        if (!base || !safe_path(*base)) {
          return fail("Invalid base file path for " + *target);
        }
        const base::FilePath base_relative =
            base::FilePath::FromUTF8Unsafe(*base);
        if (!no_links(source_root, base_relative)) {
          return fail("Base file is a symbolic link: " + *base);
        }
        input = source_root.Append(base_relative);
        if (!base::PathExists(input) || base::DirectoryExists(input)) {
          return fail("Missing base file: " + *base);
        }
      }
      if (*type == "copy") {
        if (!InstallStagedFile(input, dest)) {
          return fail("Failed to copy " + *target);
        }
      } else if (*type == "add" || *type == "patch") {
        const std::string* payload =
            action.FindString(*type == "add" ? "source" : "patch");
        if (!payload || !safe_path(*payload)) {
          return fail("Invalid payload path for " + *target);
        }
        base::FilePath payload_relative =
            base::FilePath::FromUTF8Unsafe(*payload);
        if (!no_links(patch_dir, payload_relative)) {
          return fail("Payload is a symbolic link: " + *payload);
        }
        base::FilePath payload_path = patch_dir.Append(payload_relative);
        if (*type == "add") {
          input = payload_path;
          if (!InstallStagedFile(input, dest)) {
            return fail("Failed to add " + *target);
          }
        } else if (!XenonUpdatePatcher::ApplyPatch(input, payload_path, dest)) {
          return fail("Failed to patch " + *target);
        }
      } else {
        return fail("Unknown patch action: " + *type);
      }
      if (!ContentSha256Matches(dest, *sha)) {
        return fail("SHA-256 mismatch for " + *target);
      }
#if !BUILDFLAG(IS_WIN)
      int mode = 0;
      if (action.Find("mode") && !action.FindInt("mode")) {
        return fail("File permissions must be an integer for " + *target);
      }
      if (auto declared_mode = action.FindInt("mode")) {
        if (*declared_mode < 0 || *declared_mode > 0777) {
          return fail("Invalid file permissions for " + *target);
        }
        mode = *declared_mode;
      } else if (!base::GetPosixFilePermissions(input, &mode)) {
        return fail("Cannot read base permissions for " + *target);
      }
      if (!base::SetPosixFilePermissions(dest, mode & 0777)) {
        return fail("Cannot restore permissions for " + *target);
      }
#endif
    }
#if !BUILDFLAG(IS_WIN)
    if (bundle) {
      // Application directories need traversal permissions for every user of
      // a shared /Applications installation. CreateDirectory defaults to 0700.
      base::FileEnumerator directories(temp_staging_dir, true,
                                       base::FileEnumerator::DIRECTORIES);
      if (!base::SetPosixFilePermissions(temp_staging_dir, 0755)) {
        return fail("Cannot set application directory permissions");
      }
      for (base::FilePath dir = directories.Next(); !dir.empty();
           dir = directories.Next()) {
        if (!base::SetPosixFilePermissions(dir, 0755)) {
          return fail("Cannot set application directory permissions");
        }
      }
    }
    for (const auto& [dest, link] : links) {
      if (base::PathExists(dest) || base::IsLink(dest) ||
          !base::CreateSymbolicLink(link, dest)) {
        return fail("Cannot create application symbolic link");
      }
    }
    const base::FilePath absolute_staging =
        base::MakeAbsoluteFilePath(temp_staging_dir);
    for (const auto& [dest, link] : links) {
      const base::FilePath resolved = base::MakeAbsoluteFilePath(dest);
      if (resolved.empty() || !absolute_staging.IsParent(resolved)) {
        return fail("Application link is broken or escapes staging directory");
      }
    }
#endif
    if (!bundle) {
      base::ListValue root_deletions;
      if (parsed->Find("deletions") && !parsed->FindList("deletions")) {
        return fail("Invalid deletion list");
      }
      if (const auto* removed = parsed->FindList("deletions")) {
        for (const auto& item : *removed) {
          if (!item.is_string() || !safe_path(item.GetString())) {
            return fail("Unsafe deletion path");
          }
          const std::string& path = item.GetString();
          if (base::StartsWith(path, *base_version + "/")) {
            continue;  // Old version files are excluded from the new tree.
          }
          base::FilePath deletion_key = base::FilePath::FromUTF8Unsafe(path);
#if BUILDFLAG(IS_WIN)
          deletion_key =
              base::FilePath::FromUTF8Unsafe(base::ToLowerASCII(path));
#endif
          if (targets.contains(deletion_key)) {
            return fail("A target cannot be both replaced and deleted");
          }
          root_deletions.Append(path);
        }
      }
      std::string serialized;
      if (!base::JSONWriter::Write(root_deletions, &serialized) ||
          !base::WriteFile(
              temp_staging_dir.AppendASCII(".xenon-deletions.json"),
              serialized)) {
        return fail("Cannot stage root deletion list");
      }
    }
    if (base::PathExists(target_version_dir) ||
        !base::CreateDirectory(target_version_dir.DirName()) ||
        !base::Move(temp_staging_dir, target_version_dir)) {
      return fail("Cannot commit prepared version directory");
    }
    base::DeletePathRecursively(patch_dir);
    result.success = true;
    result.staged_dir = target_version_dir;
    return result;
  }

  LOG(WARNING) << "[XenonUpdateManager] Not using manifest mode (is_zip="
               << is_zip
               << ", manifest_exists=" << base::PathExists(manifest_path)
               << "). Falling back to legacy single-file mode.";

  auto legacy_fail = [&](const std::string& error) {
    result.error = error;
    base::DeletePathRecursively(temp_staging_dir);
    base::DeletePathRecursively(patch_dir);
    return result;
  };
  // Fallback: Legacy Single-File / Companion Patch Mode
  base::FilePath dll_patch = patch_file;
  base::FilePath exe_patch;

  if (is_zip) {
    LOG(INFO) << "[XenonUpdateManager] Successfully unpacked differential "
                 "update archive";
    // Check for DLL patch in archive
    base::FilePath candidate_dll = patch_dir.Append(
        base_file.BaseName().value() + FILE_PATH_LITERAL(".zucc"));
    if (base::PathExists(candidate_dll)) {
      dll_patch = candidate_dll;
    } else {
      candidate_dll = patch_dir.AppendASCII("patch.zucc");
      if (base::PathExists(candidate_dll)) {
        dll_patch = candidate_dll;
      }
    }

    // Check for Launcher patch in archive
    base::FilePath candidate_exe = patch_dir.Append(
        current_exe.BaseName().value() + FILE_PATH_LITERAL(".zucc"));
    if (base::PathExists(candidate_exe)) {
      exe_patch = candidate_exe;
    } else {
      candidate_exe = patch_dir.AppendASCII("launcher.zucc");
      if (base::PathExists(candidate_exe)) {
        exe_patch = candidate_exe;
      }
    }
  } else {
    // If patch_file is a standalone .zucc, check if a companion .zucc exists
    // beside it
    base::FilePath candidate_exe = patch_file.DirName().Append(
        current_exe.BaseName().value() + FILE_PATH_LITERAL(".zucc"));
    if (base::PathExists(candidate_exe)) {
      exe_patch = candidate_exe;
    }
  }

  // 2. Apply Zucchini patch to generate new main binary (xlbrowser.dll)
  base::FilePath target_binary = temp_staging_dir.Append(base_file.BaseName());
  if (!XenonUpdatePatcher::ApplyPatch(base_file, dll_patch, target_binary)) {
    result.error =
        "Zucchini differential patch application failed for main binary";
    base::DeletePathRecursively(temp_staging_dir);
    return result;
  }

  // 3. Replicate non-dll files and assets from current_version_dir
  if (base::DirectoryExists(current_version_dir)) {
    base::FileEnumerator enumerator(
        current_version_dir, /*recursive=*/false,
        base::FileEnumerator::FILES | base::FileEnumerator::DIRECTORIES);
    for (base::FilePath item = enumerator.Next(); !item.empty();
         item = enumerator.Next()) {
      if (item.BaseName() == base_file.BaseName()) {
        continue;  // Already generated via patch
      }
      std::string item_name = item.BaseName().MaybeAsASCII();
      if (std::find(deletions.begin(), deletions.end(), item_name) !=
          deletions.end()) {
        continue;  // File was deprecated/deleted in new version!
      }
      base::FilePath dest = temp_staging_dir.Append(item.BaseName());
      if (item.BaseName() == base::FilePath(FILE_PATH_LITERAL(".xenon-root")) ||
          item.BaseName() ==
              base::FilePath(FILE_PATH_LITERAL(".xenon-deletions.json"))) {
        continue;
      }
      const bool copied =
          enumerator.GetInfo().IsDirectory()
              ? base::CopyDirectory(item, dest, /*recursive=*/true)
              : InstallStagedFile(item, dest);
      if (!copied) {
        return legacy_fail("Failed to copy legacy update resource");
      }
    }
  }

  std::string current_ver_str = current_version_dir.BaseName().MaybeAsASCII();
  std::string target_ver_str = target_version_dir.BaseName().MaybeAsASCII();

  // 3b. Ensure <target_version>.manifest exists for Windows SxS assembly
  // binding
  if (!target_ver_str.empty()) {
    base::FilePath target_manifest =
        temp_staging_dir.AppendASCII(target_ver_str + ".manifest");
    if (!base::PathExists(target_manifest)) {
      base::FilePath src_manifest =
          temp_staging_dir.AppendASCII(current_ver_str + ".manifest");
      std::string manifest_content;
      if (base::PathExists(src_manifest) &&
          base::ReadFileToString(src_manifest, &manifest_content)) {
        base::ReplaceSubstringsAfterOffset(&manifest_content, 0,
                                           current_ver_str, target_ver_str);
        base::WriteFile(target_manifest, manifest_content);
      }
    }
  }

  // Keep companion launcher bytes private until the installer commits.
  if (!exe_patch.empty()) {
    const base::FilePath launcher = temp_staging_dir.AppendASCII(".xenon-root")
                                        .Append(current_exe.BaseName());
    if (!XenonUpdatePatcher::ApplyPatch(current_exe, exe_patch, launcher)) {
      return legacy_fail("Failed to apply companion launcher patch");
    }
  }
#if BUILDFLAG(IS_WIN)
  if (!expected_target_version.empty() && exe_patch.empty()) {
    return legacy_fail(
        "A Windows legacy update requires a companion launcher patch");
  }
#endif

  // 2c. Update setup.exe in Installer directory if a differential patch or new
  // binary is provided
  base::FilePath setup_patch =
      patch_dir.Append(FILE_PATH_LITERAL("setup.exe.zucc"));
  if (base::PathExists(setup_patch)) {
    base::FilePath current_setup =
        current_version_dir.Append(FILE_PATH_LITERAL("Installer"))
            .Append(FILE_PATH_LITERAL("setup.exe"));
    base::FilePath target_installer_dir =
        temp_staging_dir.Append(FILE_PATH_LITERAL("Installer"));
    base::FilePath target_setup =
        target_installer_dir.Append(FILE_PATH_LITERAL("setup.exe"));
    if (!base::PathExists(current_setup) ||
        !base::CreateDirectory(target_installer_dir) ||
        !XenonUpdatePatcher::ApplyPatch(current_setup, setup_patch,
                                        target_setup)) {
      return legacy_fail("Failed to apply installer patch");
    }
  }

  // Clean up extracted patches inside temp_staging_dir before committing
  if (base::PathExists(patch_dir)) {
    base::DeletePathRecursively(patch_dir);
  }

  // Never erase a version directory that may be used by another process.
  if (base::PathExists(target_version_dir) ||
      !base::CreateDirectory(target_version_dir.DirName()) ||
      !base::Move(temp_staging_dir, target_version_dir)) {
    return legacy_fail("Failed to commit legacy version directory");
  }

  result.success = true;
  result.staged_dir = target_version_dir;
  return result;
}

void XenonUpdateManager::OnPatchCompleteWithOutput(
    VersionStagingResult result) {
  if (!result.success) {
    if (manifest_->is_diff && manifest_->full_package) {
      LOG(WARNING) << "[XenonUpdateManager] Patch application failed: "
                   << result.error
                   << ". Falling back to full package download.";
      manifest_->is_diff = false;
      StartPackageDownload(*manifest_->full_package, /*is_diff=*/false);
      return;
    }
    NotifyError("Failed to apply differential patch: " + result.error);
    return;
  }

  staged_output_file_ = result.staged_dir;
  staged_is_installer_package_ = false;
  SetState(UpdateState::kUpdateDownloaded);
  for (auto& observer : observers_) {
    observer.OnUpdateDownloaded(*manifest_);
  }
}

void XenonUpdateManager::CleanupOldVersions() {
  if (system_updater_) {
    return;
  }
  base::ThreadPool::PostDelayedTask(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&XenonUpdateManager::DoCleanupOldVersions,
                     GetDefaultInstallDir(), GetCurrentVersion()),
      base::Seconds(3));
}

// static
void XenonUpdateManager::DoCleanupOldVersions(base::FilePath install_dir,
                                              std::string current_ver_str) {
  LOG(INFO) << "[XenonUpdateManager] DoCleanupOldVersions started: install_dir="
            << install_dir.value() << ", current_ver=" << current_ver_str;
  // macOS keeps one .app bundle. Version directories are a Windows layout, and
  // scanning inside the bundle would treat framework folders as old installs.
  if (install_dir.FinalExtension() == FILE_PATH_LITERAL(".app")) {
    return;
  }
  base::Version current_version(current_ver_str);
  if (!current_version.IsValid() || !base::DirectoryExists(install_dir)) {
    LOG(WARNING) << "[XenonUpdateManager] DoCleanupOldVersions: invalid "
                    "version or missing install dir";
    return;
  }
#if BUILDFLAG(IS_WIN)
  XenonUpdateInstaller::CleanupRecoveryDirectories(install_dir,
                                                   current_ver_str);
  // setup.exe owns version retention and pending in-use updates. Only clean
  // transactions left by the previous directory installer here.
#else
  // 1. Clean up older version directories
  base::FileEnumerator enumerator(install_dir, /*recursive=*/false,
                                  base::FileEnumerator::DIRECTORIES);
  for (base::FilePath dir = enumerator.Next(); !dir.empty();
       dir = enumerator.Next()) {
    std::string dirname = dir.BaseName().MaybeAsASCII();
    base::Version dir_version(dirname);
    if (dir_version.IsValid() && dir_version < current_version) {
      LOG(INFO) << "[XenonUpdateManager] Cleaning up old version directory: "
                << dir.value();
      if (!base::DeletePathRecursively(dir)) {
#if BUILDFLAG(IS_WIN)
        base::FileEnumerator sub_enum(dir, /*recursive=*/true,
                                      base::FileEnumerator::FILES);
        for (base::FilePath f = sub_enum.Next(); !f.empty();
             f = sub_enum.Next()) {
          if (!base::DeleteFile(f)) {
            ::MoveFileExW(f.value().c_str(), nullptr,
                          MOVEFILE_DELAY_UNTIL_REBOOT);
          }
        }
        base::DeletePathRecursively(dir);
#endif
      }
    }
  }

  // Clean up old binaries only. Prepared new files may belong to an update
  // already running in this or another browser process.
  base::FileEnumerator file_enumerator(install_dir, /*recursive=*/false,
                                       base::FileEnumerator::FILES,
                                       FILE_PATH_LITERAL("old_*"));
  for (base::FilePath file = file_enumerator.Next(); !file.empty();
       file = file_enumerator.Next()) {
    if (!base::DeleteFile(file)) {
#if BUILDFLAG(IS_WIN)
      ::MoveFileExW(file.value().c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
#endif
    }
  }
#endif
}

void XenonUpdateManager::OnVersionStartup() {
  if (system_updater_) {
    return;
  }
  CleanupOldVersions();
#if BUILDFLAG(IS_MAC)
  base::FilePath install_dir = GetDefaultInstallDir();
  std::string current_ver = GetCurrentVersion();
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&XenonUpdateInstaller::ReadLastInstallError, install_dir,
                     current_ver),
      base::BindOnce(&XenonUpdateManager::OnLastInstallErrorRead,
                     weak_factory_.GetWeakPtr()));
#endif
}

void XenonUpdateManager::OnLastInstallErrorRead(std::string error) {
  // Startup recovery must not overwrite a newer operation started by the user
  // while the result file was being read.
  if (!error.empty() && state_ == UpdateState::kIdle) {
    NotifyError(error);
  }
}

void XenonUpdateManager::InstallOnExit() {
  if (system_updater_) {
    return;  // The system service already owns the pending transaction.
  }
  // QuitAndInstall already started installation. Do not start it twice.
  if (install_started_ || !auto_install_on_app_quit_ ||
      staged_is_installer_package_ || requires_elevation_) {
    return;
  }
  if (state_ != UpdateState::kUpdateDownloaded || staged_output_file_.empty()) {
    return;
  }
  install_started_ = true;
  // Preparation must finish before the thread pool shuts down. The native
  // macOS helper waits for browser exit before exchanging application bundles.
  base::FilePath staged = staged_output_file_;
  base::FilePath install_dir = GetDefaultInstallDir();
  base::ThreadPool::PostTask(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::USER_BLOCKING,
       base::TaskShutdownBehavior::BLOCK_SHUTDOWN},
      base::BindOnce(
          [](base::FilePath staged, base::FilePath install_dir) {
            std::string error;
            if (!XenonUpdateInstaller::InstallAndRelaunch(
                    staged, install_dir,
                    /*relaunch_executable=*/base::FilePath(), &error)) {
              LOG(ERROR) << "Failed to install update on exit: " << error;
            }
          },
          std::move(staged), std::move(install_dir)));
}

void XenonUpdateManager::QuitAndInstall() {
  if (system_updater_) {
    if (state_ != UpdateState::kUpdateDownloaded || install_started_) {
      return;
    }
    install_started_ = true;
    ExitBrowserForUpdate();
    return;
  }
  if (state_ == UpdateState::kCheckingForUpdate ||
      state_ == UpdateState::kDownloading || state_ == UpdateState::kPatching) {
    return;
  }
  if (install_started_) {
    return;
  }
  if (state_ != UpdateState::kUpdateDownloaded || staged_output_file_.empty()) {
    NotifyError("No update ready for installation");
    return;
  }
  install_started_ = true;

#if BUILDFLAG(IS_MAC)
  if (staged_is_installer_package_) {
    base::FilePath staged = staged_output_file_;
    if (!manifest_ || !manifest_->full_package) {
      OnInstallerPackageOpened(false, "Missing installer package metadata");
      return;
    }
    installer_opened_ = false;
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_BLOCKING},
        base::BindOnce(&XenonUpdateInstaller::PrepareInstallerPackage,
                       std::move(staged), manifest_->full_package->sha256,
                       installer_package_cache_root_for_testing_),
        base::BindOnce(
            [](base::WeakPtr<XenonUpdateManager> manager,
               XenonUpdateInstaller::InstallerPackagePreparation result) {
              if (manager) {
                manager->OnInstallerPackagePrepared(std::move(result.path),
                                                    std::move(result.error));
              }
            },
            weak_factory_.GetWeakPtr()));
    return;
  }
  elevation_approval_required_ = false;
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_BLOCKING},
      base::BindOnce(&XenonUpdateManager::EvaluateDifferentialEligibility,
                     install_dir_override_,
                     differential_eligibility_for_testing_),
      base::BindOnce(&XenonUpdateManager::OnInstallAccessChecked,
                     weak_factory_.GetWeakPtr()));
#else
  StartDifferentialInstallation();
#endif
}

void XenonUpdateManager::OnInstallAccessChecked(
    DifferentialEligibilityResult result) {
  if (!result.allowed) {
    OnInstallInitiated({false, std::move(result.reason)});
    return;
  }
  requires_elevation_ = result.requires_elevation;
  StartDifferentialInstallation();
}

void XenonUpdateManager::StartDifferentialInstallation() {
  base::FilePath staged = staged_output_file_;
  base::FilePath install_dir = GetDefaultInstallDir();
  base::FilePath executable = GetDefaultExecutable();
#if BUILDFLAG(IS_MAC)
  if (requires_elevation_) {
    if (privileged_installer_for_testing_) {
      privileged_installer_for_testing_.Run(
          staged, install_dir, executable,
          base::BindOnce(&XenonUpdateManager::OnPrivilegedInstallInitiated,
                         weak_factory_.GetWeakPtr()));
      return;
    }
    XenonUpdateInstaller::BeginPrivilegedInstallAndRelaunch(
        staged, install_dir, executable,
        base::BindOnce(
            [](base::WeakPtr<XenonUpdateManager> manager,
               XenonUpdateInstaller::PrivilegedInstallResult result) {
              if (manager) {
                using Status = XenonUpdateInstaller::PrivilegedInstallStatus;
                manager->OnPrivilegedInstallInitiated(
                    result.status == Status::kReadyToQuit,
                    result.status == Status::kRequiresApproval,
                    std::move(result.error));
              }
            },
            weak_factory_.GetWeakPtr()));
    return;
  }
#endif
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::USER_BLOCKING,
       base::TaskShutdownBehavior::BLOCK_SHUTDOWN},
      base::BindOnce(
          [](base::FilePath staged, base::FilePath install_dir,
             base::FilePath executable) {
            std::string error;
            const bool initiated = XenonUpdateInstaller::InstallAndRelaunch(
                staged, install_dir, executable, &error);
            return std::make_pair(initiated, std::move(error));
          },
          std::move(staged), std::move(install_dir), std::move(executable)),
      base::BindOnce(&XenonUpdateManager::OnInstallInitiated,
                     weak_factory_.GetWeakPtr()));
}

void XenonUpdateManager::OnPrivilegedInstallInitiated(bool ready_to_quit,
                                                      bool requires_approval,
                                                      std::string error) {
  if (requires_approval) {
    install_started_ = false;
    elevation_approval_required_ = true;
    last_error_.clear();
    for (auto& observer : observers_) {
      observer.OnElevationApprovalRequired();
    }
    return;
  }
  elevation_approval_required_ = false;
  OnInstallInitiated({ready_to_quit, std::move(error)});
}

void XenonUpdateManager::OnInstallInitiated(
    std::pair<bool, std::string> result) {
  if (!result.first) {
    install_started_ = false;
#if BUILDFLAG(IS_MAC)
    if (manifest_ && manifest_->is_diff && manifest_->full_package &&
        !staged_is_installer_package_) {
      LOG(WARNING) << "[XenonUpdateManager] Differential installation failed: "
                   << result.second << ". Downloading the full PKG installer.";
      manifest_->is_diff = false;
      staged_output_file_.clear();
      last_error_.clear();
      StartPackageDownload(*manifest_->full_package, /*is_diff=*/false);
      return;
    }
#endif
    last_error_ = result.second.empty()
                      ? "Failed to initiate update installation and relaunch"
                      : std::move(result.second);
    const std::string installation_error = last_error_;
    // Keep the verified package available for an explicit retry. Reporting an
    // installation error must not discard a successfully downloaded update.
    for (auto& observer : observers_) {
      observer.OnUpdateError(installation_error);
    }
    return;
  }
  ExitBrowserForUpdate();
}

void XenonUpdateManager::OnInstallerPackagePrepared(base::FilePath path,
                                                    std::string error) {
#if BUILDFLAG(IS_MAC)
  if (path.empty()) {
    OnInstallerPackageOpened(false, std::move(error));
    return;
  }
  // Installer may request that the browser quit. Keep the PKG outside the
  // browser's temporary download directory so shutdown cannot remove it.
  staged_output_file_ = std::move(path);
  auto completion =
      base::BindOnce(&XenonUpdateManager::OnInstallerPackageOpened,
                     weak_factory_.GetWeakPtr());
  if (installer_package_opener_for_testing_) {
    installer_package_opener_for_testing_.Run(staged_output_file_,
                                              std::move(completion));
    return;
  }
  XenonUpdateInstaller::OpenInstallerPackage(staged_output_file_,
                                             std::move(completion));
#endif
}

void XenonUpdateManager::OnInstallerPackageOpened(bool opened,
                                                  std::string error) {
  install_started_ = false;
  installer_opened_ = opened;
  if (!opened) {
    OnInstallInitiated({false, error.empty() ? "Failed to open macOS Installer"
                                             : std::move(error)});
    return;
  }
  last_error_.clear();
  // NSWorkspace confirms handoff only. Installer owns authorization, the
  // must-close prompt and package installation; keep this browser running.
  for (auto& observer : observers_) {
    observer.OnInstallerOpened();
  }
}

}  // namespace xenon::updater
