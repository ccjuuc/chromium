#include "xenon_overlay/chrome/browser/xenon_extension_manager.h"

#include <memory>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/json/json_file_value_serializer.h"
#include "base/json/json_reader.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/strings/string_split.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/thread_pool.h"
#include "base/values.h"
#include "base/version.h"
#include "chrome/browser/extensions/component_loader.h"
#include "chrome/browser/profiles/profile.h"
#include "components/services/unzip/content/unzip_service.h"
#include "components/services/unzip/public/cpp/unzip.h"
#include "components/services/unzip/public/mojom/unzipper.mojom.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/storage_partition.h"
#include "extensions/browser/extension_registry.h"
#include "extensions/common/extension.h"
#include "extensions/common/extension_id.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "url/gurl.h"
#include "xenon_overlay/chrome/browser/ui/xenon_web_dialog.h"


namespace xenon {

namespace {

using ExtensionLoadData = std::pair<base::FilePath, std::optional<base::Value::Dict>>;

base::Version GetExtensionVersionFromPath(const base::FilePath& path) {
  base::FilePath manifest_path = path.AppendASCII("manifest.json");
  if (!base::PathExists(manifest_path))
    return base::Version();
  
  std::string error;
  JSONFileValueDeserializer deserializer(manifest_path);
  std::unique_ptr<base::Value> root = deserializer.Deserialize(nullptr, &error);
  if (!root || !root->is_dict())
    return base::Version();
  
  const std::string* ver = root->GetDict().FindString("version");
  return ver ? base::Version(*ver) : base::Version();
}

base::FilePath FindBestBuiltinPath(const std::vector<base::FilePath>& builtin_paths) {
  base::FilePath best_path;
  base::Version best_version;
  
  for (const auto& path : builtin_paths) {
    if (path.empty())
      continue;
    
    base::Version version = GetExtensionVersionFromPath(path);
    if (version.IsValid() && (!best_version.IsValid() || version > best_version)) {
      best_path = path;
      best_version = version;
    } else if (best_path.empty() && base::PathExists(path.AppendASCII("manifest.json"))) {
      best_path = path;
    }
  }
  
  return best_path;
}

ExtensionLoadData DetermineBestExtensionPath(
    std::vector<base::FilePath> builtin_paths,
    base::FilePath user_update_path) {
  ExtensionLoadData result;
  
  base::FilePath best_builtin_path = FindBestBuiltinPath(builtin_paths);
  base::Version user_version = GetExtensionVersionFromPath(user_update_path);
  base::Version builtin_version = GetExtensionVersionFromPath(best_builtin_path);

  base::FilePath path_to_load = best_builtin_path;
  if (user_version.IsValid() && 
      (!builtin_version.IsValid() || user_version > builtin_version)) {
    path_to_load = user_update_path;
  }

  if (path_to_load.empty()) {
    LOG(ERROR) << "ComponentExtensionManager: No valid extension path found";
    return result;
  }

  base::FilePath manifest_path = path_to_load.AppendASCII("manifest.json");
  if (!base::PathExists(manifest_path)) {
    LOG(ERROR) << "ComponentExtensionManager: Manifest not found at " 
               << manifest_path.value();
    return result;
  }

  std::string error;
  JSONFileValueDeserializer deserializer(manifest_path);
  std::unique_ptr<base::Value> root = deserializer.Deserialize(nullptr, &error);
  
  if (!root || !root->is_dict()) {
    LOG(ERROR) << "ComponentExtensionManager: Failed to parse manifest: " << error;
    return result;
  }

  result.first = path_to_load;
  result.second = std::move(root->GetDict());
  return result;
}

}  // namespace

namespace {
base::FilePath BuildPathFromString(const std::string& path_str) {
  if (path_str.empty())
    return base::FilePath();
  
  base::FilePath module_path;
  if (!base::PathService::Get(base::DIR_MODULE, &module_path))
    return base::FilePath();
  
  base::FilePath result = module_path;
  for (const std::string& component : base::SplitString(
           path_str, "/", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    result = result.AppendASCII(component);
  }
  return result;
}
}  // namespace

ComponentExtensionConfig::ComponentExtensionConfig() = default;

ComponentExtensionConfig::ComponentExtensionConfig(const ComponentExtensionConfig& other)
    : extension_name(other.extension_name),
      expected_extension_id(other.expected_extension_id),
      builtin_path(other.builtin_path),
      additional_builtin_paths(other.additional_builtin_paths),
      user_data_subdir(other.user_data_subdir),
      update_check_url(other.update_check_url) {}

ComponentExtensionConfig::ComponentExtensionConfig(ComponentExtensionConfig&& other) noexcept
    : extension_name(std::move(other.extension_name)),
      expected_extension_id(std::move(other.expected_extension_id)),
      builtin_path(std::move(other.builtin_path)),
      additional_builtin_paths(std::move(other.additional_builtin_paths)),
      user_data_subdir(std::move(other.user_data_subdir)),
      update_check_url(std::move(other.update_check_url)) {}

ComponentExtensionConfig& ComponentExtensionConfig::operator=(const ComponentExtensionConfig& other) {
  if (this != &other) {
    extension_name = other.extension_name;
    expected_extension_id = other.expected_extension_id;
    builtin_path = other.builtin_path;
    additional_builtin_paths = other.additional_builtin_paths;
    user_data_subdir = other.user_data_subdir;
    update_check_url = other.update_check_url;
  }
  return *this;
}

ComponentExtensionConfig& ComponentExtensionConfig::operator=(ComponentExtensionConfig&& other) noexcept {
  if (this != &other) {
    extension_name = std::move(other.extension_name);
    expected_extension_id = std::move(other.expected_extension_id);
    builtin_path = std::move(other.builtin_path);
    additional_builtin_paths = std::move(other.additional_builtin_paths);
    user_data_subdir = std::move(other.user_data_subdir);
    update_check_url = std::move(other.update_check_url);
  }
  return *this;
}

ComponentExtensionConfig::~ComponentExtensionConfig() = default;

ComponentExtensionConfigBuilder& ComponentExtensionConfigBuilder::SetExtensionName(const std::string& name) {
  config_.extension_name = name;
  return *this;
}

ComponentExtensionConfigBuilder& ComponentExtensionConfigBuilder::SetExpectedExtensionId(const std::string& id) {
  config_.expected_extension_id = id;
  return *this;
}

ComponentExtensionConfigBuilder& ComponentExtensionConfigBuilder::SetBuiltinPath(const std::string& path) {
  config_.builtin_path = path;
  return *this;
}

ComponentExtensionConfigBuilder& ComponentExtensionConfigBuilder::AddAdditionalBuiltinPath(const std::string& path) {
  config_.additional_builtin_paths.push_back(path);
  return *this;
}

ComponentExtensionConfigBuilder& ComponentExtensionConfigBuilder::SetUserDataSubdir(const std::string& subdir) {
  config_.user_data_subdir = subdir;
  return *this;
}

ComponentExtensionConfigBuilder& ComponentExtensionConfigBuilder::SetUpdateCheckUrl(const GURL& url) {
  config_.update_check_url = url;
  return *this;
}

ComponentExtensionConfig ComponentExtensionConfigBuilder::Build() {
  return std::move(config_);
}

ComponentExtensionManager::ComponentExtensionManager() = default;

ComponentExtensionManager::~ComponentExtensionManager() = default;

void ComponentExtensionManager::RegisterExtension(
    const std::string& extension_name,
    const ComponentExtensionConfig& config) {
  configs_[extension_name] = config;
}

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
    const std::string& extension_name,
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
  }

  return extension_id;
}

extensions::ExtensionId ComponentExtensionManager::LoadExtension(
    content::BrowserContext* context,
    const std::string& extension_name,
    const base::FilePath& extension_path) {
  if (extension_path.empty()) {
    LOG(ERROR) << "ComponentExtensionManager: Extension path is empty";
    return std::string();
  }

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

  return AddExtensionWithManifest(context, extension_name, std::move(root->GetDict()), extension_path);
}

void ComponentExtensionManager::LoadExtensionFromDefaultPath(
    content::BrowserContext* context,
    const std::string& extension_name,
    OnExtensionLoadedCallback callback) {
  auto it = configs_.find(extension_name);
  if (it == configs_.end()) {
    LOG(ERROR) << "ComponentExtensionManager: Extension '" << extension_name << "' not registered";
    if (callback)
      std::move(callback).Run(std::string());
    return;
  }

  const ComponentExtensionConfig& config = it->second;
  std::vector<base::FilePath> builtin_paths;
  
  if (!config.builtin_path.empty()) {
    base::FilePath primary_builtin = BuildPathFromString(config.builtin_path);
    if (!primary_builtin.empty())
      builtin_paths.push_back(primary_builtin);
  }
  
  for (const std::string& path_str : config.additional_builtin_paths) {
    base::FilePath additional_path = BuildPathFromString(path_str);
    if (!additional_path.empty())
      builtin_paths.push_back(additional_path);
  }

  base::FilePath user_update_path = GetUserUpdatePath(context, extension_name);
  
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&DetermineBestExtensionPath, std::move(builtin_paths), user_update_path),
      base::BindOnce(&ComponentExtensionManager::OnExtensionPathDetermined,
                     base::Unretained(this), context, extension_name, std::move(callback)));
}

void ComponentExtensionManager::OnExtensionPathDetermined(
    content::BrowserContext* context,
    const std::string& extension_name,
    OnExtensionLoadedCallback callback,
    std::pair<base::FilePath, std::optional<base::Value::Dict>> result) {
  base::FilePath path_to_load = result.first;
  std::optional<base::Value::Dict> manifest = std::move(result.second);

  if (path_to_load.empty() || !manifest.has_value()) {
    LOG(ERROR) << "ComponentExtensionManager: Failed to determine extension path or parse manifest";
    if (callback)
      std::move(callback).Run(std::string());
    return;
  }
  
  extensions::ComponentLoader* loader = GetComponentLoader(context);
  if (!loader) {
    LOG(ERROR) << "ComponentExtensionManager: Failed to get ComponentLoader";
    if (callback)
      std::move(callback).Run(std::string());
    return;
  }

  loader->Remove(path_to_load);
  const extensions::Extension* existing = FindExtension(context, extension_name);
  if (existing)
    loader->Remove(existing->id());

  extensions::ExtensionId id = AddExtensionWithManifest(context, extension_name, std::move(*manifest), path_to_load);
  if (callback)
    std::move(callback).Run(id);
}

const extensions::Extension* ComponentExtensionManager::FindExtension(
    content::BrowserContext* context,
    const std::string& extension_name) {
  if (!context) {
    return nullptr;
  }

  auto it = configs_.find(extension_name);
  if (it == configs_.end()) {
    return nullptr;
  }

  extensions::ExtensionRegistry* registry = GetExtensionRegistry(context);
  if (!registry) {
    return nullptr;
  }

  for (const auto& extension : registry->enabled_extensions()) {
    if (extension->name() == it->second.extension_name) {
      return extension.get();
    }
  }

  return nullptr;
}

bool ComponentExtensionManager::ShowExtension(content::BrowserContext* context,
                                              const std::string& extension_name) {
  if (!context) {
    LOG(WARNING) << "ComponentExtensionManager: Invalid context for ShowExtension";
    return false;
  }

  auto it = configs_.find(extension_name);
  if (it == configs_.end()) {
    LOG(WARNING) << "ComponentExtensionManager: Extension '" << extension_name << "' not registered";
    return false;
  }

  const extensions::Extension* target_extension = FindExtension(context, extension_name);
  if (!target_extension) {
    LOG(WARNING) << "ComponentExtensionManager: Extension '" << it->second.extension_name
                 << "' not found in enabled extensions registry.";
    return false;
  }

  XenonWebDialog::Show(context, target_extension->GetResourceURL("index.html"), 400, 300,
                       base::UTF8ToUTF16(it->second.extension_name));

  if (it->second.update_check_url.is_valid())
    CheckForUpdates(context, extension_name, it->second.update_check_url);

  return true;
}

void ComponentExtensionManager::CheckForUpdates(content::BrowserContext* context,
                                                 const std::string& extension_name,
                                                 const GURL& update_check_url) {
  if (!context)
    return;
  
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
                     base::Unretained(this), context, extension_name),
      1024 * 1024);
}

void ComponentExtensionManager::LoadAllExtensions(content::BrowserContext* context) {
  for (const auto& [name, config] : configs_) {
    LoadExtensionFromDefaultPath(context, name);
  }
}

base::FilePath ComponentExtensionManager::GetBuiltinPath(const std::string& extension_name) const {
  auto it = configs_.find(extension_name);
  if (it == configs_.end() || it->second.builtin_path.empty())
    return base::FilePath();
  return BuildPathFromString(it->second.builtin_path);
}

base::FilePath ComponentExtensionManager::GetUserUpdatePath(
    content::BrowserContext* context,
    const std::string& extension_name) const {
  auto it = configs_.find(extension_name);
  if (it == configs_.end() || it->second.user_data_subdir.empty())
    return base::FilePath();
  
  Profile* profile = Profile::FromBrowserContext(context);
  if (!profile)
    return base::FilePath();
  
  return profile->GetOriginalProfile()->GetPath()
             .AppendASCII(it->second.user_data_subdir);
}

void ComponentExtensionManager::OnUpdateCheckComplete(
    content::BrowserContext* context,
    const std::string& extension_name,
    std::optional<std::string> response_body) {
  data_loader_.reset();

  auto it = configs_.find(extension_name);
  if (it == configs_.end()) {
    LOG(ERROR) << "ComponentExtensionManager: Extension '" << extension_name << "' not registered";
    return;
  }

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

  base::Version local_version;
  const extensions::Extension* loaded_extension = FindExtension(context, extension_name);
  if (loaded_extension && loaded_extension->version().IsValid()) {
    local_version = loaded_extension->version();
  } else {
    base::FilePath user_update_path = GetUserUpdatePath(context, extension_name);
    base::FilePath builtin_path = GetBuiltinPath(extension_name);
    base::Version user_version = GetExtensionVersionFromPath(user_update_path);
    base::Version builtin_version = GetExtensionVersionFromPath(builtin_path);
    if (user_version.IsValid() && builtin_version.IsValid()) {
      local_version = (user_version > builtin_version) ? user_version : builtin_version;
    } else if (user_version.IsValid()) {
      local_version = user_version;
    } else if (builtin_version.IsValid()) {
      local_version = builtin_version;
    }
  }

  bool should_download = !local_version.IsValid() || remote_version > local_version;
  if (should_download) {
    GURL download_url(*url_str);
    if (!download_url.is_valid()) {
      LOG(ERROR) << "ComponentExtensionManager: Invalid download URL.";
      return;
    }
    DownloadUpdate(context, extension_name, download_url, *version_str);
  }
}

void ComponentExtensionManager::DownloadUpdate(content::BrowserContext* context,
                                                const std::string& extension_name,
                                                const GURL& download_url,
                                                const std::string& version) {
  net::NetworkTrafficAnnotationTag traffic_annotation =
      net::DefineNetworkTrafficAnnotation("component_extension_download", R"(
        semantics {
          sender: "Component Extension Manager"
          description: "Downloads component extension update."
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
                     base::Unretained(this), context, extension_name, version));
}

void ComponentExtensionManager::OnDownloadComplete(content::BrowserContext* context,
                                                    const std::string& extension_name,
                                                    const std::string& version,
                                                    base::FilePath response_path) {
  if (response_path.empty()) {
    LOG(ERROR) << "ComponentExtensionManager: Download failed.";
    download_loader_.reset();
    return;
  }

  content::GetUIThreadTaskRunner({})->PostTask(
      FROM_HERE,
      base::BindOnce(&ComponentExtensionManager::OnDownloadCompleteOnUIThread,
                     base::Unretained(this), context, extension_name, response_path));
}

void ComponentExtensionManager::OnDownloadCompleteOnUIThread(
    content::BrowserContext* context,
    const std::string& extension_name,
    base::FilePath response_path) {
  Profile* profile = Profile::FromBrowserContext(context);
  if (!profile) {
    LOG(ERROR) << "ComponentExtensionManager: Invalid profile for download completion.";
    download_loader_.reset();
    return;
  }
  
  auto it = configs_.find(extension_name);
  if (it == configs_.end()) {
    LOG(ERROR) << "ComponentExtensionManager: Extension '" << extension_name << "' not registered";
    download_loader_.reset();
    return;
  }

  base::FilePath user_update_path = GetUserUpdatePath(context, extension_name);
  base::FilePath dest_dir = user_update_path.DirName();
  unzip::Unzip(
      unzip::LaunchUnzipper(),
      response_path,
      dest_dir,
      unzip::mojom::UnzipOptions::New(),
      unzip::AllContents(),
      base::DoNothing(),
      base::BindOnce(&ComponentExtensionManager::OnUnzipComplete,
                     base::Unretained(this), context, extension_name,
                     user_update_path));
}

void ComponentExtensionManager::OnUnzipComplete(content::BrowserContext* context,
                                                 const std::string& extension_name,
                                                 const base::FilePath& unzip_dir,
                                                 bool success) {
  download_loader_.reset();
  if (!success) {
    LOG(ERROR) << "ComponentExtensionManager: Failed to unzip update.";
    return;
  }
  
  content::GetUIThreadTaskRunner({})->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](ComponentExtensionManager* manager, content::BrowserContext* ctx, const std::string& name) {
            manager->LoadExtensionFromDefaultPath(ctx, name, base::NullCallback());
          },
          base::Unretained(this), context, extension_name));
}

namespace {

constexpr char kXenonExtensionName[] = "Xenon Overlay Extension";

ComponentExtensionConfig CreateXenonConfig() {
  return ComponentExtensionConfigBuilder()
      .SetExtensionName(kXenonExtensionName)
      .SetExpectedExtensionId("mkkhnfilihmphalmfjjbobdnaikhbeoi")
      .SetBuiltinPath("resources/xenon_extension")
      .SetUserDataSubdir("xenon_extension")
      .SetUpdateCheckUrl(GURL("http://localhost:3000/download/update_manifest.json"))
      .Build();
}

// Example: To register multiple extensions, use ComponentExtensionManager directly:
//
// ComponentExtensionConfig CreateSecondExtensionConfig() {
//   return ComponentExtensionConfigBuilder()
//       .SetExtensionName("Second Extension")
//       .SetExpectedExtensionId("another_extension_id_here")
//       .SetBuiltinPath("resources/second_extension")
//       .AddAdditionalBuiltinPath("resources/second_extension_v2")  // Optional: add fallback paths
//       .SetUserDataSubdir("second_extension")
//       .SetUpdateCheckUrl(GURL("http://localhost:3000/download/second_update_manifest.json"))
//       .Build();
// }
//
// Then register it (e.g., in browser initialization code):
// auto* manager = ComponentExtensionManager::GetInstance();  // or use a shared instance
// manager->RegisterExtension("Second Extension", CreateSecondExtensionConfig());
// manager->LoadAllExtensions(context);

}  // namespace

XenonExtensionManager* XenonExtensionManager::GetInstance() {
  return base::Singleton<XenonExtensionManager>::get();
}

XenonExtensionManager::XenonExtensionManager()
    : xenon_extension_name_(kXenonExtensionName) {
  manager_.RegisterExtension(kXenonExtensionName, CreateXenonConfig());
}

XenonExtensionManager::~XenonExtensionManager() = default;

// static
base::FilePath XenonExtensionManager::GetDefaultExtensionPath() {
  XenonExtensionManager* instance = GetInstance();
  if (!instance)
    return base::FilePath();
  return instance->manager_.GetBuiltinPath(kXenonExtensionName);
}

const std::string& XenonExtensionManager::GetExtensionName() {
  static base::NoDestructor<std::string> kName(kXenonExtensionName);
  return *kName;
}

extensions::ExtensionId XenonExtensionManager::LoadExtension(
    content::BrowserContext* context,
    const base::FilePath& extension_path) {
  return manager_.LoadExtension(context, xenon_extension_name_, extension_path);
}

void XenonExtensionManager::LoadExtensionFromDefaultPath(
    content::BrowserContext* context,
    OnExtensionLoadedCallback callback) {
  manager_.LoadExtensionFromDefaultPath(context, xenon_extension_name_, std::move(callback));
}

const extensions::Extension* XenonExtensionManager::FindExtension(
    content::BrowserContext* context) {
  return manager_.FindExtension(context, xenon_extension_name_);
}

bool XenonExtensionManager::ShowExtension(content::BrowserContext* context) {
  return manager_.ShowExtension(context, xenon_extension_name_);
}

void XenonExtensionManager::CheckForUpdates(content::BrowserContext* context,
                                             const GURL& update_check_url) {
  manager_.CheckForUpdates(context, xenon_extension_name_, update_check_url);
}

}  // namespace xenon

