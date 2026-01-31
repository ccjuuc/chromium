#include "xenon_overlay/chrome/browser/xenon_extension_manager.h"

#include "base/base_paths.h"
#include "base/files/file_path.h"
#include "base/logging.h"
#include "base/path_service.h"
#include "chrome/browser/extensions/component_loader.h"
#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/browser_context.h"
#include "extensions/browser/extension_registry.h"
#include "extensions/common/extension.h"
#include "extensions/common/extension_id.h"
#include "url/gurl.h"
#include "xenon_overlay/chrome/browser/ui/xenon_web_dialog.h"

#include <memory>
#include <string>
#include <vector>

#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/json/json_file_value_serializer.h"
#include "base/json/json_reader.h"
#include "base/task/thread_pool.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/threading/platform_thread.h"
#include "base/values.h"
#include "base/version.h"
#include "components/services/unzip/content/unzip_service.h"
#include "components/services/unzip/public/cpp/unzip.h"
#include "components/services/unzip/public/mojom/unzipper.mojom.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/storage_partition.h"
#include "net/base/load_flags.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/url_loader_factory.mojom.h"
#include "base/threading/thread_restrictions.h"


namespace xenon {

namespace {

// Helper type to hold the result of the background task
using ExtensionLoadData = std::pair<base::FilePath, std::optional<base::Value::Dict>>;


// Simplified version checker that parses version but discards rest.
base::Version GetExtensionVersionFromPath(const base::FilePath& path) {
  if (!base::PathExists(path.AppendASCII("manifest.json")))
    return base::Version();
  
  std::string error;
  JSONFileValueDeserializer deserializer(path.AppendASCII("manifest.json"));
  std::unique_ptr<base::Value> root = deserializer.Deserialize(nullptr, &error);
  if (!root || !root->is_dict()) return base::Version();
  
  const std::string* ver = root->GetDict().FindString("version");
  return ver ? base::Version(*ver) : base::Version();
}

ExtensionLoadData DetermineBestExtensionPath(
    std::vector<base::FilePath> builtin_paths,
    base::FilePath user_update_path) {
  ExtensionLoadData result;
  
  // Find the best built-in path (highest version)
  base::FilePath best_builtin_path;
  base::Version best_builtin_version;
  
  for (const auto& builtin_path : builtin_paths) {
    if (builtin_path.empty()) continue;
    
    base::Version version = GetExtensionVersionFromPath(builtin_path);
    if (version.IsValid() && (!best_builtin_version.IsValid() || version > best_builtin_version)) {
      best_builtin_path = builtin_path;
      best_builtin_version = version;
    } else if (!best_builtin_path.empty() && !version.IsValid() && base::PathExists(builtin_path.AppendASCII("manifest.json"))) {
      // If no version found but manifest exists, use it as fallback
      if (!best_builtin_version.IsValid()) {
        best_builtin_path = builtin_path;
      }
    }
  }
  
  // If no valid builtin found, use the first non-empty one
  if (best_builtin_path.empty() && !builtin_paths.empty()) {
    for (const auto& builtin_path : builtin_paths) {
      if (!builtin_path.empty()) {
        best_builtin_path = builtin_path;
        break;
      }
    }
  }
  
  // Compare with user update path
  base::Version user_version = GetExtensionVersionFromPath(user_update_path);
  base::Version builtin_version = GetExtensionVersionFromPath(best_builtin_path);

  base::FilePath path_to_load = best_builtin_path;

  // Decide which path to use
  if (user_version.IsValid()) {
    // If user version is valid, check if it's newer than built-in
    if (!builtin_version.IsValid() || user_version > builtin_version) {
      path_to_load = user_update_path;
    }
  }

  result.first = path_to_load;

  if (path_to_load.empty()) {
    LOG(ERROR) << "ComponentExtensionManager: No valid extension path found";
    return result;
  }

  // Parse the manifest of the chosen path
  base::FilePath manifest_path = path_to_load.AppendASCII("manifest.json");
  if (!base::PathExists(manifest_path)) {
    LOG(ERROR) << "ComponentExtensionManager: Manifest not found at " << manifest_path.value();
    return result;
  }

  std::string error;
  JSONFileValueDeserializer deserializer(manifest_path);
  std::unique_ptr<base::Value> root = deserializer.Deserialize(nullptr, &error);
  
  if (!root || !root->is_dict()) {
    LOG(ERROR) << "ComponentExtensionManager: Failed to parse manifest: " << error;
    return result;
  }

  result.second = std::move(root->GetDict());
  return result;
}

}  // namespace

// ComponentExtensionConfig implementation
ComponentExtensionConfig::ComponentExtensionConfig()
    : extension_name(nullptr),
      get_builtin_path(nullptr),
      get_additional_builtin_paths(nullptr),
      get_user_update_path(nullptr),
      user_data_subdir(nullptr),
      extension_subdir(nullptr) {}

ComponentExtensionConfig::ComponentExtensionConfig(const ComponentExtensionConfig& other)
    : extension_name(other.extension_name),
      expected_extension_id(other.expected_extension_id),
      get_builtin_path(other.get_builtin_path),
      get_additional_builtin_paths(other.get_additional_builtin_paths),
      get_user_update_path(other.get_user_update_path),
      user_data_subdir(other.user_data_subdir),
      extension_subdir(other.extension_subdir),
      update_check_url(other.update_check_url) {}

ComponentExtensionConfig::ComponentExtensionConfig(ComponentExtensionConfig&& other) noexcept
    : extension_name(other.extension_name),
      expected_extension_id(std::move(other.expected_extension_id)),
      get_builtin_path(other.get_builtin_path),
      get_additional_builtin_paths(other.get_additional_builtin_paths),
      get_user_update_path(other.get_user_update_path),
      user_data_subdir(other.user_data_subdir),
      extension_subdir(other.extension_subdir),
      update_check_url(std::move(other.update_check_url)) {
  other.extension_name = nullptr;
  other.get_builtin_path = nullptr;
  other.get_additional_builtin_paths = nullptr;
  other.get_user_update_path = nullptr;
  other.user_data_subdir = nullptr;
  other.extension_subdir = nullptr;
}

ComponentExtensionConfig& ComponentExtensionConfig::operator=(const ComponentExtensionConfig& other) {
  if (this != &other) {
    extension_name = other.extension_name;
    expected_extension_id = other.expected_extension_id;
    get_builtin_path = other.get_builtin_path;
    get_additional_builtin_paths = other.get_additional_builtin_paths;
    get_user_update_path = other.get_user_update_path;
    user_data_subdir = other.user_data_subdir;
    extension_subdir = other.extension_subdir;
    update_check_url = other.update_check_url;
  }
  return *this;
}

ComponentExtensionConfig& ComponentExtensionConfig::operator=(ComponentExtensionConfig&& other) noexcept {
  if (this != &other) {
    extension_name = other.extension_name;
    expected_extension_id = std::move(other.expected_extension_id);
    get_builtin_path = other.get_builtin_path;
    get_additional_builtin_paths = other.get_additional_builtin_paths;
    get_user_update_path = other.get_user_update_path;
    user_data_subdir = other.user_data_subdir;
    extension_subdir = other.extension_subdir;
    update_check_url = std::move(other.update_check_url);
    
    other.extension_name = nullptr;
    other.get_builtin_path = nullptr;
    other.get_additional_builtin_paths = nullptr;
    other.get_user_update_path = nullptr;
    other.user_data_subdir = nullptr;
    other.extension_subdir = nullptr;
  }
  return *this;
}

// ComponentExtensionManager implementation
ComponentExtensionManager::ComponentExtensionManager(const ComponentExtensionConfig& config)
    : config_(config) {}

ComponentExtensionManager::~ComponentExtensionManager() = default;

extensions::ComponentLoader* ComponentExtensionManager::GetComponentLoader(
    content::BrowserContext* context) {
  if (!context) {
    return nullptr;
  }
  Profile* profile = Profile::FromBrowserContext(context);
  if (!profile) {
    return nullptr;
  }
  return extensions::ComponentLoader::Get(profile);
}

extensions::ExtensionRegistry* ComponentExtensionManager::GetExtensionRegistry(
    content::BrowserContext* context) {
  if (!context) {
    return nullptr;
  }
  return extensions::ExtensionRegistry::Get(context);
}

extensions::ExtensionId ComponentExtensionManager::AddExtensionWithManifest(
    content::BrowserContext* context,
    base::Value::Dict manifest,
    const base::FilePath& extension_path) {
  extensions::ComponentLoader* loader = GetComponentLoader(context);
  if (!loader) {
    LOG(ERROR) << "ComponentExtensionManager: Failed to get ComponentLoader";
    return std::string();
  }

  extensions::ExtensionId extension_id = loader->Add(std::move(manifest), extension_path);
  if (extension_id.empty()) {
    LOG(ERROR) << "ComponentExtensionManager: Failed to load extension from "
               << extension_path.value();
  } else {
    LOG(INFO) << "ComponentExtensionManager: Successfully loaded extension with ID: "
              << extension_id << " from " << extension_path.value();
  }

  return extension_id;
}

extensions::ExtensionId ComponentExtensionManager::LoadExtension(
    content::BrowserContext* context,
    const base::FilePath& extension_path) {
  if (extension_path.empty()) {
    LOG(ERROR) << "ComponentExtensionManager: Extension path is empty";
    return std::string();
  }

  // Parse manifest from file system
  base::FilePath manifest_path = extension_path.AppendASCII("manifest.json");
  if (!base::PathExists(manifest_path)) {
    LOG(ERROR) << "ComponentExtensionManager: Manifest not found at " << manifest_path.value();
    return std::string();
  }

  std::string error;
  JSONFileValueDeserializer deserializer(manifest_path);
  std::unique_ptr<base::Value> root = deserializer.Deserialize(nullptr, &error);
  
  if (!root || !root->is_dict()) {
    LOG(ERROR) << "ComponentExtensionManager: Failed to parse manifest at "
               << manifest_path.value() << ": " << error;
    return std::string();
  }

  return AddExtensionWithManifest(context, std::move(root->GetDict()), extension_path);
}

void ComponentExtensionManager::LoadExtensionFromDefaultPath(
    content::BrowserContext* context,
    OnExtensionLoadedCallback callback) {
  // 1. Get Built-in Path(s) (Read-only, bundled with Chrome)
  std::vector<base::FilePath> builtin_paths;
  base::FilePath primary_builtin = GetBuiltinPath();
  if (!primary_builtin.empty()) {
    builtin_paths.push_back(primary_builtin);
  }
  
  // Add additional built-in paths if configured
  if (config_.get_additional_builtin_paths) {
    std::vector<base::FilePath> additional = config_.get_additional_builtin_paths();
    builtin_paths.insert(builtin_paths.end(), additional.begin(), additional.end());
  }

  // 2. Get User Update Path (Read-write, in User Data Directory)
  base::FilePath user_update_path = GetUserUpdatePath(context);

  LOG(INFO) << "ComponentExtensionManager: User update path: " << user_update_path.value();

  // Perform version check on background thread to avoid blocking UI thread
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&DetermineBestExtensionPath, std::move(builtin_paths), user_update_path),
      base::BindOnce(&ComponentExtensionManager::OnExtensionPathDetermined,
                     base::Unretained(this), context, std::move(callback)));
}

void ComponentExtensionManager::OnExtensionPathDetermined(
    content::BrowserContext* context,
    OnExtensionLoadedCallback callback,
    std::pair<base::FilePath, std::optional<base::Value::Dict>> result) {
  base::FilePath path_to_load = result.first;
  std::optional<base::Value::Dict> manifest = std::move(result.second);

  LOG(INFO) << "ComponentExtensionManager: DetermineBestExtensionPath result: " << path_to_load.value();

  if (path_to_load.empty() || !manifest.has_value()) {
    LOG(ERROR) << "ComponentExtensionManager: Failed to determine extension path or parse manifest";
    if (callback)
      std::move(callback).Run(std::string());
    return;
  }

  LOG(INFO) << "ComponentExtensionManager: Loading extension from path: " << path_to_load.value();
  
  extensions::ComponentLoader* loader = GetComponentLoader(context);
  if (!loader) {
    LOG(ERROR) << "ComponentExtensionManager: Failed to get ComponentLoader";
    if (callback)
       std::move(callback).Run(std::string());
    return;
  }

  // Remove existing extension if any
  loader->Remove(path_to_load);
  const extensions::Extension* existing = FindExtension(context);
  if (existing) {
     loader->Remove(existing->id());
  }

  extensions::ExtensionId id = AddExtensionWithManifest(context, std::move(*manifest), path_to_load);

  if (callback)
    std::move(callback).Run(id);
}



const extensions::Extension* ComponentExtensionManager::FindExtension(
    content::BrowserContext* context) {
  if (!context) {
    return nullptr;
  }

  extensions::ExtensionRegistry* registry = GetExtensionRegistry(context);
  if (!registry) {
    return nullptr;
  }

  for (const auto& extension : registry->enabled_extensions()) {
    if (extension->name() == config_.extension_name) {
      return extension.get();
    }
  }

  // Extension not found - this is normal during initial load or before extension is loaded.
  // Callers should handle nullptr appropriately (e.g., ShowExtension will log a warning).
  return nullptr;
}

bool ComponentExtensionManager::ShowExtension(content::BrowserContext* context) {
  if (!context) {
    LOG(WARNING) << "ComponentExtensionManager: Invalid context for ShowExtension";
    return false;
  }

  const extensions::Extension* target_extension = FindExtension(context);
  if (!target_extension) {
    LOG(WARNING) << "ComponentExtensionManager: Extension '" << config_.extension_name
                 << "' not found in enabled extensions registry.";
    return false;
  }

  GURL extension_url = target_extension->GetResourceURL("index.html");
  LOG(INFO) << "ComponentExtensionManager: Found extension with ID: "
            << target_extension->id();
  LOG(INFO) << "ComponentExtensionManager: Opening URL: " << extension_url.spec();

  XenonWebDialog::Show(context, extension_url, 400, 300,
                       base::UTF8ToUTF16(std::string(config_.extension_name)));

  // Trigger update check if URL is configured
  if (config_.update_check_url.is_valid()) {
    CheckForUpdates(context, config_.update_check_url);
  }

  return true;
}

void ComponentExtensionManager::CheckForUpdates(content::BrowserContext* context,
                                                 const GURL& update_check_url) {
  if (!context) return;
  
  net::NetworkTrafficAnnotationTag traffic_annotation =
      net::DefineNetworkTrafficAnnotation("component_extension_update_check", R"(
        semantics {
          sender: "Component Extension Manager"
          description: "Checks for updates to the component extension."
          trigger: "Manual check or periodic background check."
          data: "None."
          destination: OTHER
        }
        policy {
          cookies_allowed: NO
          setting: "This feature cannot be disabled."
          policy_exception_justification: "Essential for extension functionality."
        })");

  auto resource_request = std::make_unique<network::ResourceRequest>();
  resource_request->url = update_check_url;
  resource_request->method = "GET";
  resource_request->credentials_mode = network::mojom::CredentialsMode::kOmit;

  data_loader_ = network::SimpleURLLoader::Create(std::move(resource_request),
                                                  traffic_annotation);
  
  data_loader_->DownloadToString(
      context->GetDefaultStoragePartition()->GetURLLoaderFactoryForBrowserProcess().get(),
      base::BindOnce(&ComponentExtensionManager::OnUpdateCheckComplete,
                     base::Unretained(this), context),
      1024 * 1024); // 1MB limit for manifest
}

void ComponentExtensionManager::OnUpdateCheckComplete(
    content::BrowserContext* context,
    std::optional<std::string> response_body) {
  data_loader_.reset();

  if (!response_body) {
    LOG(ERROR) << "ComponentExtensionManager: Update check failed.";
    return;
  }

  auto result = base::JSONReader::ReadAndReturnValueWithError(
      *response_body, base::JSON_PARSE_RFC);
  if (!result.has_value() || !result->is_dict()) {
    LOG(ERROR) << "ComponentExtensionManager: Invalid update manifest JSON.";
    return;
  }

  const std::string* version_str = result->GetDict().FindString("version");
  const std::string* url_str = result->GetDict().FindString("url");

  if (!version_str || !url_str) {
    LOG(ERROR) << "ComponentExtensionManager: Missing version or url in update manifest.";
    return;
  }

  base::Version remote_version(*version_str);
  if (!remote_version.IsValid()) {
    LOG(ERROR) << "ComponentExtensionManager: Invalid remote version.";
    return;
  }

  // Get local version from loaded extension or local paths
  base::Version local_version;
  const extensions::Extension* loaded_extension = FindExtension(context);
  if (loaded_extension && loaded_extension->version().IsValid()) {
    local_version = loaded_extension->version();
        LOG(INFO) << "ComponentExtensionManager: Current loaded extension version: "
              << local_version.GetString();
  } else {
    // Fallback: get version from local paths
    base::FilePath user_update_path = GetUserUpdatePath(context);
    base::Version user_version = GetExtensionVersionFromPath(user_update_path);
    base::Version builtin_version = GetExtensionVersionFromPath(GetBuiltinPath());
      
      // Use the newer of the two local versions
      if (user_version.IsValid() && builtin_version.IsValid()) {
        local_version = (user_version > builtin_version) ? user_version : builtin_version;
        LOG(INFO) << "ComponentExtensionManager: Local versions - Built-in: "
                  << builtin_version.GetString() << ", User: "
                  << user_version.GetString() << ", Using: "
                  << local_version.GetString();
      } else if (user_version.IsValid()) {
        local_version = user_version;
        LOG(INFO) << "ComponentExtensionManager: Local user version: "
                  << local_version.GetString();
      } else if (builtin_version.IsValid()) {
        local_version = builtin_version;
        LOG(INFO) << "ComponentExtensionManager: Local built-in version: "
                  << local_version.GetString();
      }
  }

  LOG(INFO) << "ComponentExtensionManager: Local version: " << local_version.GetString();
  LOG(INFO) << "ComponentExtensionManager: Remote version: " << remote_version.GetString();

  // Compare versions
  if (local_version.IsValid()) {
    LOG(INFO) << "ComponentExtensionManager: Version comparison - Local: "
              << local_version.GetString() << ", Remote: "
              << remote_version.GetString();
    
    if (remote_version > local_version) {
      LOG(INFO) << "ComponentExtensionManager: Update available! Remote version ("
                << remote_version.GetString() << ") is newer than local ("
                << local_version.GetString() << ")";
    } else if (remote_version == local_version) {
      LOG(INFO) << "ComponentExtensionManager: Already up to date. Local version ("
                << local_version.GetString() << ") matches remote ("
                << remote_version.GetString() << ")";
    } else {
      LOG(INFO) << "ComponentExtensionManager: Local version ("
                << local_version.GetString() << ") is newer than remote ("
                << remote_version.GetString() << "). No update needed.";
    }
  } else {
    LOG(WARNING) << "ComponentExtensionManager: Could not determine local version. "
                 << "Proceeding with update check. Remote version: "
                 << remote_version.GetString();
  }
  
  GURL download_url(*url_str);
  if (!download_url.is_valid()) {
    LOG(ERROR) << "ComponentExtensionManager: Invalid download URL.";
    return;
  }
  
  // Only download if remote version is newer
  if (local_version.IsValid() && remote_version > local_version) {
    LOG(INFO) << "ComponentExtensionManager: Downloading update version "
              << remote_version.GetString() << " from " << download_url.spec();
    DownloadUpdate(context, download_url, *version_str);
  } else if (!local_version.IsValid()) {
    // If we can't determine local version, download anyway
    LOG(INFO) << "ComponentExtensionManager: Downloading update version "
              << remote_version.GetString() << " from " << download_url.spec()
              << " (local version unknown)";
    DownloadUpdate(context, download_url, *version_str);
  } else {
    LOG(INFO) << "ComponentExtensionManager: Skipping download. No update needed.";
  }
}

void ComponentExtensionManager::DownloadUpdate(content::BrowserContext* context,
                                           const GURL& download_url,
                                           const std::string& version) {
  LOG(INFO) << "ComponentExtensionManager: Downloading update version "
            << version << " from " << download_url.spec();
  net::NetworkTrafficAnnotationTag traffic_annotation =
      net::DefineNetworkTrafficAnnotation("xenon_extension_download", R"(
        semantics {
          sender: "Xenon Overlay Extension Manager"
          description: "Downloads the Xenon Overlay Extension update."
          trigger: "Update check found a new version."
          data: "None."
          destination: OTHER
        }
        policy {
          cookies_allowed: NO
          setting: "This feature cannot be disabled."
          policy_exception_justification: "Essential for extension functionality."
        })");

  auto resource_request = std::make_unique<network::ResourceRequest>();
  resource_request->url = download_url;
  resource_request->method = "GET";
  resource_request->credentials_mode = network::mojom::CredentialsMode::kOmit;

  download_loader_ = network::SimpleURLLoader::Create(std::move(resource_request),
                                                      traffic_annotation);

  download_loader_->DownloadToTempFile(
      context->GetDefaultStoragePartition()->GetURLLoaderFactoryForBrowserProcess().get(),
      base::BindOnce(&ComponentExtensionManager::OnDownloadComplete,
                     base::Unretained(this), context, version));
}

void ComponentExtensionManager::OnDownloadComplete(content::BrowserContext* context,
                                               const std::string& version,
                                               base::FilePath response_path) {
  // Keep loader alive so temp file is not deleted during unzip
  // download_loader_.reset();
  LOG(INFO) << "ComponentExtensionManager: Download complete. Response path: " << response_path.value();
  if (response_path.empty()) {
    LOG(ERROR) << "ComponentExtensionManager: Download failed.";
    download_loader_.reset();
    return;
  }

  // SimpleURLLoader callbacks may execute on any thread, but we need to access
  // Profile which is a UI thread object. Post to UI thread to ensure thread safety.
  content::GetUIThreadTaskRunner({})->PostTask(
      FROM_HERE,
      base::BindOnce(&ComponentExtensionManager::OnDownloadCompleteOnUIThread,
                     base::Unretained(this), context, response_path));
}

void ComponentExtensionManager::OnDownloadCompleteOnUIThread(
    content::BrowserContext* context,
    base::FilePath response_path) {
  // This method runs on UI thread, safe to access Profile
  Profile* profile = Profile::FromBrowserContext(context);
  if (!profile) {
    LOG(ERROR) << "ComponentExtensionManager: Invalid profile for download completion.";
    download_loader_.reset();
    return;
  }
  
  // Get the parent directory because the zip package already contains the xenon_extension directory
  base::FilePath dest_dir = config_.get_user_update_path(context).DirName();
  
  LOG(INFO) << "ComponentExtensionManager: Extracting update to " << dest_dir.value();
  
  // unzip::Unzip will run on a background thread internally
  // The zip package contains the xenon_extension directory, so we extract to its parent
  unzip::Unzip(
      unzip::LaunchUnzipper(),
      response_path,
      dest_dir,
      unzip::mojom::UnzipOptions::New(),
      unzip::AllContents(),
      base::DoNothing(),
      base::BindOnce(&ComponentExtensionManager::OnUnzipComplete,
                     base::Unretained(this), context, config_.get_user_update_path(context)));
}

void ComponentExtensionManager::OnUnzipComplete(content::BrowserContext* context,
                                            const base::FilePath& unzip_dir,
                                            bool success) {
  // Now we can release the download loader (and its temp file)
  download_loader_.reset();
  if (success) {
    LOG(INFO) << "ComponentExtensionManager: Update installed successfully to " 
              << unzip_dir.value();
    
    // Reload the extension to apply the update immediately.
    content::GetUIThreadTaskRunner({})->PostTask(
        FROM_HERE,
        base::BindOnce(
            [](ComponentExtensionManager* manager, content::BrowserContext* ctx) {
              manager->LoadExtensionFromDefaultPath(ctx, base::NullCallback());
            },
            base::Unretained(this), context));
  } else {
    LOG(ERROR) << "ComponentExtensionManager: Failed to unzip update.";
  }
}

// XenonExtensionManager implementation (wrapper around ComponentExtensionManager)
namespace {

constexpr char kXenonExtensionName[] = "Xenon Overlay Extension";

base::FilePath GetXenonBuiltinPath() {
  base::FilePath extension_path;
  if (base::PathService::Get(base::DIR_MODULE, &extension_path)) {
    extension_path = extension_path.AppendASCII("resources")
                                   .AppendASCII("xenon_extension");
  }
  return extension_path;
}

base::FilePath GetXenonUserUpdatePath(content::BrowserContext* context) {
  Profile* profile = Profile::FromBrowserContext(context);
  if (!profile) {
    return base::FilePath();
  }
  return profile->GetOriginalProfile()->GetPath()
             .AppendASCII("xenon_extension");
}

ComponentExtensionConfig CreateXenonConfig() {
  ComponentExtensionConfig config;
  config.extension_name = kXenonExtensionName;
  config.expected_extension_id = "mkkhnfilihmphalmfjjbobdnaikhbeoi";
  config.get_builtin_path = GetXenonBuiltinPath;
  config.get_additional_builtin_paths = nullptr;  // No additional built-in paths for now
  config.get_user_update_path = GetXenonUserUpdatePath;
  config.user_data_subdir = "xenon_extension";
  config.extension_subdir = "";  // Not used when using get_user_update_path
  config.update_check_url = GURL("http://localhost:3000/download/update_manifest.json");
  return config;
}

}  // namespace

// static
XenonExtensionManager* XenonExtensionManager::GetInstance() {
  return base::Singleton<XenonExtensionManager>::get();
}

XenonExtensionManager::XenonExtensionManager()
    : manager_(std::make_unique<ComponentExtensionManager>(CreateXenonConfig())) {}

XenonExtensionManager::~XenonExtensionManager() = default;

// static
base::FilePath XenonExtensionManager::GetDefaultExtensionPath() {
  return GetXenonBuiltinPath();
}

// static
const char* XenonExtensionManager::GetExtensionName() {
  return kXenonExtensionName;
}

extensions::ExtensionId XenonExtensionManager::LoadExtension(
    content::BrowserContext* context,
    const base::FilePath& extension_path) {
  return manager_->LoadExtension(context, extension_path);
}

void XenonExtensionManager::LoadExtensionFromDefaultPath(
    content::BrowserContext* context,
    OnExtensionLoadedCallback callback) {
  manager_->LoadExtensionFromDefaultPath(context, std::move(callback));
}

const extensions::Extension* XenonExtensionManager::FindExtension(
    content::BrowserContext* context) {
  return manager_->FindExtension(context);
}

bool XenonExtensionManager::ShowExtension(content::BrowserContext* context) {
  return manager_->ShowExtension(context);
}

void XenonExtensionManager::CheckForUpdates(content::BrowserContext* context,
                                             const GURL& update_check_url) {
  manager_->CheckForUpdates(context, update_check_url);
}

}  // namespace xenon

