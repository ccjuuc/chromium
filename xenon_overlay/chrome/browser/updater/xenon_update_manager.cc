// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/updater/xenon_update_manager.h"

#include <utility>

#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "base/task/task_traits.h"
#include "base/task/thread_pool.h"
#include "base/version.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/lifetime/application_lifetime.h"
#include "chrome/browser/net/system_network_context_manager.h"
#include "chrome/common/chrome_constants.h"
#include "components/version_info/version_info.h"
#if BUILDFLAG(IS_WIN)
#include <windows.h>
#include "base/win/registry.h"
#elif BUILDFLAG(IS_MAC)
#include "base/apple/bundle_locations.h"
#include "base/apple/foundation_util.h"
#endif
#include "net/base/load_flags.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "third_party/zlib/google/zip.h"
#include "xenon_overlay/chrome/browser/updater/xenon_update_installer.h"
#include "xenon_overlay/chrome/browser/updater/xenon_update_patcher.h"

namespace xenon::updater {

namespace {

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
    : downloader_(std::make_unique<XenonUpdateDownloader>()) {}

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
  feed_url_ = url;
}

std::string XenonUpdateManager::GetFeedURL() const {
  return feed_url_;
}

void XenonUpdateManager::SetAutoDownload(bool auto_download) {
  auto_download_ = auto_download;
}

bool XenonUpdateManager::GetAutoDownload() const {
  return auto_download_;
}

std::string XenonUpdateManager::GetCurrentVersion() const {
  if (!current_version_override_.empty()) {
    return current_version_override_;
  }
  base::FilePath module_dir;
  if (base::PathService::Get(base::DIR_MODULE, &module_dir)) {
    std::string dir_version = module_dir.BaseName().MaybeAsASCII();
    if (base::Version(dir_version).IsValid()) {
      return dir_version;
    }
  }
  return std::string(chrome::kChromeVersion);
}

void XenonUpdateManager::SetCurrentVersionForTesting(const std::string& version) {
  current_version_override_ = version;
}

void XenonUpdateManager::SetURLLoaderFactoryForTesting(
    scoped_refptr<network::SharedURLLoaderFactory> factory) {
  test_url_loader_factory_ = std::move(factory);
}

void XenonUpdateManager::SetBaseFileForTesting(const base::FilePath& base_file) {
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
  if (g_browser_process && g_browser_process->system_network_context_manager()) {
    return g_browser_process->system_network_context_manager()
        ->GetSharedURLLoaderFactory();
  }
  return nullptr;
}

base::FilePath XenonUpdateManager::GetDefaultInstallDir() const {
  if (!install_dir_override_.empty()) {
    return install_dir_override_;
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
      if (reg_key.Open(root, subkey.c_str(), KEY_QUERY_VALUE | KEY_WOW64_32KEY) ==
              ERROR_SUCCESS ||
          reg_key.Open(root, subkey.c_str(), KEY_QUERY_VALUE) == ERROR_SUCCESS) {
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
  base::FilePath install_dir = GetDefaultInstallDir();
#if BUILDFLAG(IS_MAC)
  return install_dir;
#else
  std::string version = GetCurrentVersion();
  base::FilePath version_dir = install_dir.AppendASCII(version);
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

base::FilePath XenonUpdateManager::GetDefaultBaseFile() const {
  if (!base_file_override_.empty()) {
    return base_file_override_;
  }
  base::FilePath version_dir = GetCurrentVersionDir();
  base::FilePath install_dir = GetDefaultInstallDir();

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
}

void XenonUpdateManager::NotifyError(const std::string& message) {
  SetState(UpdateState::kError);
  for (auto& observer : observers_) {
    observer.OnUpdateError(message);
  }
}

void XenonUpdateManager::CheckForUpdates(const std::string& feed_url) {
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

  check_loader_ =
      network::SimpleURLLoader::Create(std::move(request), kCheckTrafficAnnotation);
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
    SetState(UpdateState::kUpdateNotAvailable);
    for (auto& observer : observers_) {
      observer.OnUpdateNotAvailable(GetCurrentVersion());
    }
    return;
  }

  SetState(UpdateState::kUpdateAvailable);
  for (auto& observer : observers_) {
    observer.OnUpdateAvailable(*manifest_);
  }

  if (auto_download_) {
    DownloadUpdate();
  }
}

void XenonUpdateManager::DownloadUpdate() {
  if (state_ == UpdateState::kDownloading ||
      state_ == UpdateState::kPatching ||
      state_ == UpdateState::kUpdateDownloaded) {
    LOG(WARNING) << "[XenonUpdateManager] DownloadUpdate ignored while in state: "
                 << UpdateStateToString(state_);
    return;
  }

  if (!manifest_) {
    NotifyError("No active update manifest to download");
    return;
  }

  if (!temp_download_dir_.IsValid() && !temp_download_dir_.CreateUniqueTempDir()) {
    NotifyError("Failed to create temporary directory for update download");
    return;
  }

  if (manifest_->is_diff && manifest_->diff_package) {
    StartPackageDownload(*manifest_->diff_package, /*is_diff=*/true);
  } else if (manifest_->full_package) {
    StartPackageDownload(*manifest_->full_package, /*is_diff=*/false);
  } else {
    NotifyError("Manifest does not specify valid package info");
  }
}

void XenonUpdateManager::StartPackageDownload(
    const UpdatePackageInfo& package,
    bool is_diff) {
  auto factory = GetURLLoaderFactory();
  if (!factory) {
    NotifyError("Network context is not available for update download");
    return;
  }

  SetState(UpdateState::kDownloading);

  std::string filename;
  std::string_view url_path = package.url.path();
  if (base::EndsWith(url_path, ".zip", base::CompareCase::INSENSITIVE_ASCII)) {
    filename = "update_patch.zip";
  } else if (base::EndsWith(url_path, ".zucc", base::CompareCase::INSENSITIVE_ASCII)) {
    filename = "update_patch.zucc";
  } else {
    filename = is_diff ? "update_patch.zip" : "update_package.bin";
  }
  downloaded_file_ = temp_download_dir_.GetPath().AppendASCII(filename);

  downloader_->StartDownload(
      std::move(factory), package, downloaded_file_,
      base::BindRepeating(&XenonUpdateManager::OnDownloadProgress,
                          weak_factory_.GetWeakPtr()),
      base::BindOnce(&XenonUpdateManager::OnDownloadComplete,
                     weak_factory_.GetWeakPtr(), is_diff));
}

void XenonUpdateManager::OnDownloadProgress(const DownloadProgress& progress) {
  for (auto& observer : observers_) {
    observer.OnDownloadProgress(progress);
  }
}

void XenonUpdateManager::OnDownloadComplete(
    bool is_diff,
    bool success,
    const std::string& error) {
  if (!success) {
    if (is_diff && manifest_->full_package) {
      LOG(WARNING) << "[XenonUpdateManager] Differential download failed: " << error
                   << ". Falling back to full package download.";
      manifest_->is_diff = false;
      StartPackageDownload(*manifest_->full_package, /*is_diff=*/false);
      return;
    }
    NotifyError("Download failed: " + error);
    return;
  }

  if (is_diff) {
    // Perform Zucchini patch and stage version-isolated directory in background
    SetState(UpdateState::kPatching);

    base::FilePath base_file = GetDefaultBaseFile();
    base::FilePath current_version_dir = GetCurrentVersionDir();
    base::FilePath target_version_dir =
        GetTargetVersionDir(manifest_->target_version);
    base::FilePath temp_staging_dir =
        temp_download_dir_.GetPath().AppendASCII("staging_version");

    std::vector<std::string> deletions = manifest_->deletions;
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE,
        {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce(&XenonUpdateManager::PrepareVersionDirectory,
                       current_version_dir, base_file, downloaded_file_,
                       target_version_dir, temp_staging_dir,
                       std::move(deletions)),
        base::BindOnce(&XenonUpdateManager::OnPatchCompleteWithOutput,
                       weak_factory_.GetWeakPtr()));
  } else {
    staged_output_file_ = downloaded_file_;
    SetState(UpdateState::kUpdateDownloaded);
    for (auto& observer : observers_) {
      observer.OnUpdateDownloaded(*manifest_);
    }
  }
}

// static
XenonUpdateManager::VersionStagingResult
XenonUpdateManager::PrepareVersionDirectory(
    base::FilePath current_version_dir,
    base::FilePath base_file,
    base::FilePath patch_file,
    base::FilePath target_version_dir,
    base::FilePath temp_staging_dir,
    std::vector<std::string> deletions) {
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
  base::FilePath patch_dir = patch_file.DirName().AppendASCII("extracted_patches");
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
    // =========================================================================
    // Full Directory Manifest-Driven Differential Update Mode
    // =========================================================================
    LOG(INFO) << "[XenonUpdateManager] Executing manifest-driven full directory diff update";

    std::string manifest_content;
    if (!base::ReadFileToString(manifest_path, &manifest_content)) {
      result.error = "Failed to read manifest.json from patch archive";
      base::DeletePathRecursively(temp_staging_dir);
      return result;
    }

    std::optional<base::DictValue> parsed_manifest =
        base::JSONReader::ReadDict(manifest_content, base::JSON_PARSE_RFC);
    if (!parsed_manifest) {
      result.error = "Failed to parse manifest.json as JSON dictionary";
      base::DeletePathRecursively(temp_staging_dir);
      return result;
    }

    const std::string* target_ver_str_ptr =
        parsed_manifest->FindString("target_version");
    std::string target_ver_str =
        target_ver_str_ptr ? *target_ver_str_ptr
                           : target_version_dir.BaseName().MaybeAsASCII();
    std::string target_ver_prefix = target_ver_str + "/";

    const std::string* base_ver_str_ptr =
        parsed_manifest->FindString("base_version");
    std::string base_ver_str =
        base_ver_str_ptr ? *base_ver_str_ptr
                         : current_version_dir.BaseName().MaybeAsASCII();

    const std::string* platform_str_ptr = parsed_manifest->FindString("platform");
    bool is_bundle_mode = false;
#if BUILDFLAG(IS_MAC)
    is_bundle_mode = true;
#endif
    if (platform_str_ptr && *platform_str_ptr == "mac") {
      is_bundle_mode = true;
    }

    const base::ListValue* actions = parsed_manifest->FindList("actions");
    if (!actions) {
      result.error = "manifest.json missing 'actions' list";
      base::DeletePathRecursively(temp_staging_dir);
      return result;
    }

    size_t count_copied = 0;
    size_t count_added = 0;
    size_t count_patched = 0;

    for (const auto& action_val : *actions) {
      if (!action_val.is_dict()) {
        continue;
      }
      const base::DictValue& act = action_val.GetDict();
      const std::string* target_ptr = act.FindString("target");
      const std::string* action_type_ptr = act.FindString("action");
      if (!target_ptr || !action_type_ptr) {
        continue;
      }

      std::string target_rel = *target_ptr;
      std::string action_type = *action_type_ptr;

      bool is_in_version_dir =
          is_bundle_mode || (target_rel.rfind(target_ver_prefix, 0) == 0);
      std::string sub_rel;
      if (is_bundle_mode) {
        sub_rel = target_rel;
      } else if (target_rel.rfind(target_ver_prefix, 0) == 0) {
        sub_rel = target_rel.substr(target_ver_prefix.length());
      } else {
        sub_rel = target_rel;
      }

      base::FilePath dest_path;
      if (is_in_version_dir) {
        dest_path = temp_staging_dir.Append(
            base::FilePath::FromUTF8Unsafe(sub_rel).NormalizePathSeparators());
        base::CreateDirectory(dest_path.DirName());
      }

      if (action_type == "copy") {
        const std::string* base_ptr = act.FindString("base");
        std::string base_rel = base_ptr ? *base_ptr : "";
        if (is_in_version_dir) {
          base::FilePath src_file = install_dir.Append(
              base::FilePath::FromUTF8Unsafe(base_rel).NormalizePathSeparators());
          if (!base::PathExists(src_file) && !current_version_dir.empty()) {
            base::FilePath candidate = current_version_dir.Append(
                base::FilePath::FromUTF8Unsafe(sub_rel).NormalizePathSeparators());
            if (base::PathExists(candidate)) {
              src_file = candidate;
            }
          }
          if (base::PathExists(src_file)) {
            base::CopyFile(src_file, dest_path);
            count_copied++;
          } else {
            LOG(WARNING) << "[XenonUpdateManager] Missing base file for copy: "
                         << base_rel;
          }
        } else {
          base::FilePath target_file = install_dir.Append(
              base::FilePath::FromUTF8Unsafe(target_rel).NormalizePathSeparators());
          if (!base::PathExists(target_file)) {
            base::FilePath src_file = install_dir.Append(
                base::FilePath::FromUTF8Unsafe(base_rel).NormalizePathSeparators());
            if (base::PathExists(src_file)) {
              base::CreateDirectory(target_file.DirName());
              base::CopyFile(src_file, target_file);
              count_copied++;
            }
          }
        }
      } else if (action_type == "add") {
        const std::string* source_ptr = act.FindString("source");
        std::string source_rel = source_ptr ? *source_ptr : "";
        base::FilePath src_file = patch_dir.Append(
            base::FilePath::FromUTF8Unsafe(source_rel).NormalizePathSeparators());
        if (!base::PathExists(src_file)) {
          LOG(ERROR) << "[XenonUpdateManager] Missing added file: "
                     << src_file.value() << " for target: " << target_rel;
          result.error = "Missing file for add action: " + source_rel + " (" + src_file.MaybeAsASCII() + ")";
          base::DeletePathRecursively(temp_staging_dir);
          return result;
        }

        if (is_in_version_dir) {
          base::CopyFile(src_file, dest_path);
          count_added++;
        } else {
          base::FilePath target_file = install_dir.Append(
              base::FilePath::FromUTF8Unsafe(target_rel).NormalizePathSeparators());
          base::CreateDirectory(target_file.DirName());
          if (!base::CopyFile(src_file, target_file)) {
            base::FilePath staged_new = target_file.DirName().Append(
                FILE_PATH_LITERAL("new_") + target_file.BaseName().value());
            base::CopyFile(src_file, staged_new);
          }
          count_added++;
        }
      } else if (action_type == "patch") {
        const std::string* base_ptr = act.FindString("base");
        const std::string* patch_rel_ptr = act.FindString("patch");
        std::string base_rel = base_ptr ? *base_ptr : "";
        std::string patch_rel = patch_rel_ptr ? *patch_rel_ptr : "";

        base::FilePath patch_file_full = patch_dir.Append(
            base::FilePath::FromUTF8Unsafe(patch_rel).NormalizePathSeparators());
        if (!base::PathExists(patch_file_full)) {
          LOG(ERROR) << "[XenonUpdateManager] Missing patch file: "
                     << patch_file_full.value() << " for target: " << target_rel;
          result.error = "Missing patch file in archive: " + patch_rel + " (" + patch_file_full.MaybeAsASCII() + ")";
          base::DeletePathRecursively(temp_staging_dir);
          return result;
        }

        base::FilePath src_base = install_dir.Append(
            base::FilePath::FromUTF8Unsafe(base_rel).NormalizePathSeparators());
        if (!base::PathExists(src_base) && !current_version_dir.empty()) {
          base::FilePath candidate = current_version_dir.Append(
              base::FilePath::FromUTF8Unsafe(sub_rel).NormalizePathSeparators());
          if (base::PathExists(candidate)) {
            src_base = candidate;
          }
        }
        if (!base::PathExists(src_base) && base::PathExists(base_file)) {
          if (base::FilePath::FromUTF8Unsafe(base_rel).BaseName() ==
              base_file.BaseName()) {
            src_base = base_file;
          }
        }
        if (!base::PathExists(src_base) && base::PathExists(current_exe)) {
          if (base::FilePath::FromUTF8Unsafe(base_rel).BaseName() ==
              current_exe.BaseName()) {
            src_base = current_exe;
          }
        }

        if (!base::PathExists(src_base)) {
          result.error = "Missing base file for patch: " + base_rel;
          base::DeletePathRecursively(temp_staging_dir);
          return result;
        }

        if (is_in_version_dir) {
          std::string patch_err;
          if (!XenonUpdatePatcher::ApplyPatch(src_base, patch_file_full,
                                              dest_path, &patch_err)) {
            result.error = "Failed to apply differential patch to " + target_rel + " [" + patch_err + "]";
            base::DeletePathRecursively(temp_staging_dir);
            return result;
          }
          count_patched++;
        } else {
          base::FilePath target_file = install_dir.Append(
              base::FilePath::FromUTF8Unsafe(target_rel).NormalizePathSeparators());
          base::FilePath staged_new = target_file.DirName().Append(
              FILE_PATH_LITERAL("new_") + target_file.BaseName().value());
          std::string patch_err;
          if (!XenonUpdatePatcher::ApplyPatch(src_base, patch_file_full,
                                              staged_new, &patch_err)) {
            result.error = "Failed to apply differential patch to " + target_rel + " [" + patch_err + "]";
            base::DeletePathRecursively(temp_staging_dir);
            return result;
          }
          count_patched++;
        }
      }
    }

    LOG(INFO) << "[XenonUpdateManager] Manifest diff processed: "
              << count_copied << " copied, " << count_patched << " patched, "
              << count_added << " added.";

#if BUILDFLAG(IS_WIN)
    // Ensure <target_version>.manifest exists for Windows SxS assembly binding
    if (!target_ver_str.empty()) {
      base::FilePath target_manifest =
          temp_staging_dir.AppendASCII(target_ver_str + ".manifest");
      if (!base::PathExists(target_manifest)) {
        base::FilePath src_manifest =
            temp_staging_dir.AppendASCII(base_ver_str + ".manifest");
        std::string manifest_file_content;
        if (base::PathExists(src_manifest) &&
            base::ReadFileToString(src_manifest, &manifest_file_content)) {
          base::ReplaceSubstringsAfterOffset(&manifest_file_content, 0,
                                             base_ver_str, target_ver_str);
          base::WriteFile(target_manifest, manifest_file_content);
        }
      }
    }
#endif

    base::DeletePathRecursively(patch_dir);

    if (base::PathExists(target_version_dir)) {
      base::DeletePathRecursively(target_version_dir);
    }
    base::CreateDirectory(target_version_dir.DirName());
    if (!base::Move(temp_staging_dir, target_version_dir)) {
      if (!base::CopyDirectory(temp_staging_dir, target_version_dir, true)) {
        result.error =
            "Failed to move staged directory to target version directory";
        base::DeletePathRecursively(temp_staging_dir);
        return result;
      }
      base::DeletePathRecursively(temp_staging_dir);
    }

    result.success = true;
    result.staged_dir = target_version_dir;
    return result;
  }

  LOG(WARNING) << "[XenonUpdateManager] Not using manifest mode (is_zip="
               << is_zip << ", manifest_exists=" << base::PathExists(manifest_path)
               << "). Falling back to legacy single-file mode.";

  // Fallback: Legacy Single-File / Companion Patch Mode
  base::FilePath dll_patch = patch_file;
  base::FilePath exe_patch;

  if (is_zip) {
    LOG(INFO) << "[XenonUpdateManager] Successfully unpacked differential update archive";
    // Check for DLL patch in archive
    base::FilePath candidate_dll =
        patch_dir.Append(base_file.BaseName().value() + FILE_PATH_LITERAL(".zucc"));
    if (base::PathExists(candidate_dll)) {
      dll_patch = candidate_dll;
    } else {
      candidate_dll = patch_dir.AppendASCII("patch.zucc");
      if (base::PathExists(candidate_dll)) {
        dll_patch = candidate_dll;
      }
    }

    // Check for Launcher patch in archive
    base::FilePath candidate_exe =
        patch_dir.Append(current_exe.BaseName().value() + FILE_PATH_LITERAL(".zucc"));
    if (base::PathExists(candidate_exe)) {
      exe_patch = candidate_exe;
    } else {
      candidate_exe = patch_dir.AppendASCII("launcher.zucc");
      if (base::PathExists(candidate_exe)) {
        exe_patch = candidate_exe;
      }
    }
  } else {
    // If patch_file is a standalone .zucc, check if a companion .zucc exists beside it
    base::FilePath candidate_exe =
        patch_file.DirName().Append(current_exe.BaseName().value() + FILE_PATH_LITERAL(".zucc"));
    if (base::PathExists(candidate_exe)) {
      exe_patch = candidate_exe;
    }
  }

  // 2. Apply Zucchini patch to generate new main binary (xlbrowser.dll)
  base::FilePath target_binary = temp_staging_dir.Append(base_file.BaseName());
  if (!XenonUpdatePatcher::ApplyPatch(base_file, dll_patch, target_binary)) {
    result.error = "Zucchini differential patch application failed for main binary";
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
      if (enumerator.GetInfo().IsDirectory()) {
        base::CopyDirectory(item, dest, /*recursive=*/true);
      } else {
        base::CopyFile(item, dest);
      }
    }
  }

  std::string current_ver_str = current_version_dir.BaseName().MaybeAsASCII();
  std::string target_ver_str = target_version_dir.BaseName().MaybeAsASCII();

  // 3b. Ensure <target_version>.manifest exists for Windows SxS assembly binding
  if (!target_ver_str.empty()) {
    base::FilePath target_manifest =
        temp_staging_dir.AppendASCII(target_ver_str + ".manifest");
    if (!base::PathExists(target_manifest)) {
      base::FilePath src_manifest =
          temp_staging_dir.AppendASCII(current_ver_str + ".manifest");
      std::string manifest_content;
      if (base::PathExists(src_manifest) &&
          base::ReadFileToString(src_manifest, &manifest_content)) {
        base::ReplaceSubstringsAfterOffset(&manifest_content, 0, current_ver_str, target_ver_str);
        base::WriteFile(target_manifest, manifest_content);
      }
    }
  }

  // 2b. Stage new_<launcher>.exe in install_dir via genuine Zucchini differential patch
  if (base::PathExists(current_exe) && !target_ver_str.empty()) {
    base::FilePath new_exe =
        install_dir.Append(FILE_PATH_LITERAL("new_") + current_exe.BaseName().value());
    base::FilePath staged_launcher = temp_staging_dir.Append(current_exe.BaseName());

    if (!exe_patch.empty() && base::PathExists(exe_patch)) {
      if (XenonUpdatePatcher::ApplyPatch(current_exe, exe_patch, new_exe)) {
        LOG(INFO) << "[XenonUpdateManager] Successfully generated new launcher via Zucchini patch: "
                  << new_exe.value();
        base::CopyFile(new_exe, staged_launcher);
      } else {
        LOG(ERROR) << "[XenonUpdateManager] Failed to apply Zucchini patch to launcher executable!";
      }
    } else {
      LOG(WARNING) << "[XenonUpdateManager] No differential patch found for launcher executable.";
    }
  }

  // 2c. Update setup.exe in Installer directory if a differential patch or new binary is provided
  base::FilePath setup_patch = patch_dir.Append(FILE_PATH_LITERAL("setup.exe.zucc"));
  if (base::PathExists(setup_patch)) {
    base::FilePath current_setup =
        current_version_dir.Append(FILE_PATH_LITERAL("Installer"))
                           .Append(FILE_PATH_LITERAL("setup.exe"));
    base::FilePath target_installer_dir =
        temp_staging_dir.Append(FILE_PATH_LITERAL("Installer"));
    base::FilePath target_setup =
        target_installer_dir.Append(FILE_PATH_LITERAL("setup.exe"));
    if (base::PathExists(current_setup)) {
      base::CreateDirectory(target_installer_dir);
      if (XenonUpdatePatcher::ApplyPatch(current_setup, setup_patch, target_setup)) {
        LOG(INFO) << "[XenonUpdateManager] Successfully updated setup.exe via differential patch";
      } else {
        LOG(ERROR) << "[XenonUpdateManager] Failed to apply differential patch to setup.exe";
      }
    }
  }

  // Clean up extracted patches inside temp_staging_dir before committing
  if (base::PathExists(patch_dir)) {
    base::DeletePathRecursively(patch_dir);
  }

  // 4. Move/commit temp_staging_dir to target_version_dir
  if (base::PathExists(target_version_dir)) {
    base::DeletePathRecursively(target_version_dir);
  }
  if (!base::Move(temp_staging_dir, target_version_dir)) {
    if (!base::CopyDirectory(temp_staging_dir, target_version_dir, true)) {
      result.error = "Failed to move staged directory to target version directory";
      base::DeletePathRecursively(temp_staging_dir);
      return result;
    }
    base::DeletePathRecursively(temp_staging_dir);
  }

  result.success = true;
  result.staged_dir = target_version_dir;
  return result;
}

void XenonUpdateManager::OnPatchCompleteWithOutput(
    VersionStagingResult result) {
  if (!result.success) {
    if (manifest_->full_package) {
      LOG(WARNING) << "[XenonUpdateManager] Patch application failed: "
                   << result.error << ". Falling back to full package download.";
      manifest_->is_diff = false;
      StartPackageDownload(*manifest_->full_package, /*is_diff=*/false);
      return;
    }
    NotifyError("Failed to apply differential patch: " + result.error);
    return;
  }

  staged_output_file_ = result.staged_dir;
  SetState(UpdateState::kUpdateDownloaded);
  for (auto& observer : observers_) {
    observer.OnUpdateDownloaded(*manifest_);
  }
}

void XenonUpdateManager::CleanupOldVersions() {
  base::ThreadPool::PostDelayedTask(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&XenonUpdateManager::DoCleanupOldVersions,
                     GetDefaultInstallDir(), GetCurrentVersion()),
      base::Seconds(3));
}

// static
void XenonUpdateManager::DoCleanupOldVersions(base::FilePath install_dir,
                                             std::string current_ver_str) {
  LOG(INFO) << "[XenonUpdateManager] DoCleanupOldVersions started: install_dir="
            << install_dir.value() << ", current_ver=" << current_ver_str;
  base::Version current_version(current_ver_str);
  if (!current_version.IsValid() || !base::DirectoryExists(install_dir)) {
    LOG(WARNING) << "[XenonUpdateManager] DoCleanupOldVersions: invalid version or missing install dir";
    return;
  }
  // 1. Clean up older version directories
  base::FileEnumerator enumerator(
      install_dir, /*recursive=*/false, base::FileEnumerator::DIRECTORIES);
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
        for (base::FilePath f = sub_enum.Next(); !f.empty(); f = sub_enum.Next()) {
          if (!base::DeleteFile(f)) {
            ::MoveFileExW(f.value().c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
          }
        }
        base::DeletePathRecursively(dir);
#endif
      }
    }
  }

  // 2. Clean up any leftover old_* or new_* binaries in install_dir
  base::FileEnumerator file_enumerator(
      install_dir, /*recursive=*/false, base::FileEnumerator::FILES,
      FILE_PATH_LITERAL("old_*"));
  for (base::FilePath file = file_enumerator.Next(); !file.empty();
       file = file_enumerator.Next()) {
    if (!base::DeleteFile(file)) {
#if BUILDFLAG(IS_WIN)
      ::MoveFileExW(file.value().c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
#endif
    }
  }

  base::FileEnumerator new_file_enumerator(
      install_dir, /*recursive=*/false, base::FileEnumerator::FILES,
      FILE_PATH_LITERAL("new_*"));
  for (base::FilePath file = new_file_enumerator.Next(); !file.empty();
       file = new_file_enumerator.Next()) {
    base::DeleteFile(file);
  }
}

void XenonUpdateManager::OnVersionStartup() {
  base::FilePath install_dir = GetDefaultInstallDir();
  std::string current_ver = GetCurrentVersion();
  UpdateRegistryVersion(install_dir, current_ver);
  CleanupOldVersions();
}

// static
void XenonUpdateManager::UpdateRegistryVersion(base::FilePath install_dir,
                                              const std::string& version) {
#if BUILDFLAG(IS_WIN)
  if (version.empty()) {
    return;
  }
  base::FilePath current_exe;
  std::wstring product_name;
  if (base::PathService::Get(base::FILE_EXE, &current_exe)) {
    product_name = current_exe.BaseName().RemoveFinalExtension().value();
  }
  if (product_name.empty()) {
    return;
  }

  std::wstring app_reg = L"Software\\" + product_name;
  std::wstring uninst_reg =
      L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" + product_name;

  std::wstring version_w = base::ASCIIToWide(version);
  base::FilePath setup_path = install_dir.AppendASCII(version)
                                        .Append(FILE_PATH_LITERAL("Installer"))
                                        .Append(FILE_PATH_LITERAL("setup.exe"));
  std::wstring setup_exe = setup_path.value();

  for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
    base::win::RegKey app_key;
    if (app_key.Open(root, app_reg.c_str(), KEY_SET_VALUE | KEY_WOW64_32KEY) == ERROR_SUCCESS ||
        app_key.Open(root, app_reg.c_str(), KEY_SET_VALUE) == ERROR_SUCCESS) {
      app_key.WriteValue(L"pv", version_w.c_str());
      if (base::PathExists(setup_path)) {
        app_key.WriteValue(L"UninstallString", setup_exe.c_str());
        std::wstring cleanup_cmd = L"\"" + setup_exe + L"\" --cleanup-for-downgrade-version=$1 --cleanup-for-downgrade-operation=$2";
        app_key.WriteValue(L"DowngradeCleanupCommand", cleanup_cmd.c_str());
      }
    }

    base::win::RegKey uninstall_key;
    if (uninstall_key.Open(root, uninst_reg.c_str(), KEY_SET_VALUE | KEY_WOW64_32KEY) == ERROR_SUCCESS ||
        uninstall_key.Open(root, uninst_reg.c_str(), KEY_SET_VALUE) == ERROR_SUCCESS) {
      uninstall_key.WriteValue(L"DisplayVersion", version_w.c_str());
      uninstall_key.WriteValue(L"Version", version_w.c_str());
      if (base::PathExists(setup_path)) {
        std::wstring uninst_cmd = L"\"" + setup_exe + L"\" --uninstall";
        uninstall_key.WriteValue(L"UninstallString", uninst_cmd.c_str());
      }
    }
  }
#endif
}

void XenonUpdateManager::InstallOnExit() {
  if (state_ != UpdateState::kUpdateDownloaded || staged_output_file_.empty()) {
    return;
  }
  if (manifest_ && !manifest_->target_version.empty()) {
    UpdateRegistryVersion(GetDefaultInstallDir(), manifest_->target_version);
  }
  // Launch update helper without relaunching browser
  XenonUpdateInstaller::InstallAndRelaunch(
      staged_output_file_, GetDefaultInstallDir(), /*relaunch_executable=*/base::FilePath());
}

void XenonUpdateManager::QuitAndInstall() {
  if (state_ != UpdateState::kUpdateDownloaded || staged_output_file_.empty()) {
    NotifyError("No update ready for installation");
    return;
  }

  if (manifest_ && !manifest_->target_version.empty()) {
    UpdateRegistryVersion(GetDefaultInstallDir(), manifest_->target_version);
  }

  bool initiated = XenonUpdateInstaller::InstallAndRelaunch(
      staged_output_file_, GetDefaultInstallDir(), GetDefaultExecutable());

  if (!initiated) {
    NotifyError("Failed to initiate update installation and relaunch");
    return;
  }

  // Gracefully exit the application to allow the external helper to replace files.
  chrome::ExitIgnoreUnloadHandlers();
}

}  // namespace xenon::updater
