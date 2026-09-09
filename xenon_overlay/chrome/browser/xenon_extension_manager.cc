#include "xenon_overlay/chrome/browser/xenon_extension_manager.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
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
#include "chrome/browser/extensions/chrome_component_extension_resource_manager.h"
#include "chrome/browser/extensions/component_loader.h"
#include "chrome/browser/profiles/profile.h"
#include "components/crx_file/id_util.h"
#include "components/services/unzip/content/unzip_service.h"
#include "components/services/unzip/public/cpp/unzip.h"
#include "components/services/unzip/public/mojom/unzipper.mojom.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/storage_partition.h"
#include "extensions/browser/extension_registry.h"
#include "extensions/common/extension.h"
#include "extensions/common/extension_id.h"
#include "extensions/common/manifest.h"
#include "extensions/common/manifest_constants.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "ui/base/resource/resource_bundle.h"
#include "url/gurl.h"
#include "xenon_overlay/buildflags/buildflags.h"
#include "xenon_overlay/chrome/browser/ui/xenon_web_dialog.h"
#include "xenon_overlay/chrome/browser/xenon_encrypted_extension_package.h"

namespace xenon {

namespace {

using ExtensionLoadData =
    std::pair<base::FilePath, std::optional<base::DictValue>>;

constexpr int64_t kMaxUpdateDownloadBytes = 256 * 1024 * 1024;

bool UsesEncryptedZip(const ComponentExtensionConfig& config) {
  return config.source == ComponentExtensionSource::kEncryptedZip;
}

struct CachedEncryptedExtension {
  base::DictValue manifest;
  std::map<base::FilePath, int> resource_ids;
};

void AppendLittleEndian16(std::vector<uint8_t>* output, uint16_t value) {
  output->push_back(static_cast<uint8_t>(value));
  output->push_back(static_cast<uint8_t>(value >> 8));
}

void AppendLittleEndian32(std::vector<uint8_t>* output, uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    output->push_back(static_cast<uint8_t>(value >> shift));
  }
}

std::vector<std::vector<uint8_t>>& GetEncryptedExtensionDataPacks() {
  static base::NoDestructor<std::vector<std::vector<uint8_t>>> packs;
  return *packs;
}

std::map<std::string, CachedEncryptedExtension>&
GetCachedEncryptedExtensions() {
  static base::NoDestructor<std::map<std::string, CachedEncryptedExtension>>
      extensions;
  return *extensions;
}

bool ManifestMatchesExpectedExtensionId(
    const base::DictValue& manifest,
    const std::string& expected_extension_id) {
  if (expected_extension_id.empty()) {
    return true;
  }
  const std::string* raw_key =
      manifest.FindString(extensions::manifest_keys::kPublicKey);
  std::string public_key_bytes;
  return raw_key &&
         extensions::Extension::ParsePEMKeyBytes(*raw_key, &public_key_bytes) &&
         crx_file::id_util::GenerateId(public_key_bytes) ==
             expected_extension_id;
}

struct DirectoryExtensionCandidate {
  base::FilePath path;
  base::DictValue manifest;
  base::Version version;
};

std::optional<DirectoryExtensionCandidate> ReadDirectoryExtensionCandidate(
    const base::FilePath& path,
    const std::string& expected_extension_id) {
  base::FilePath manifest_path = path.AppendASCII("manifest.json");
  if (!base::PathExists(manifest_path)) {
    return std::nullopt;
  }

  std::string error;
  JSONFileValueDeserializer deserializer(manifest_path);
  std::unique_ptr<base::Value> root = deserializer.Deserialize(nullptr, &error);
  if (!root || !root->is_dict() ||
      !ManifestMatchesExpectedExtensionId(root->GetDict(),
                                          expected_extension_id)) {
    return std::nullopt;
  }

  const std::string* version_string = root->GetDict().FindString("version");
  base::Version version(version_string ? *version_string : std::string());
  if (!version.IsValid()) {
    return std::nullopt;
  }

  return DirectoryExtensionCandidate{path, std::move(root->GetDict()),
                                     std::move(version)};
}

std::optional<DirectoryExtensionCandidate> FindBestBuiltinExtension(
    const std::vector<base::FilePath>& builtin_paths,
    const std::string& expected_extension_id) {
  std::optional<DirectoryExtensionCandidate> best;
  for (const auto& path : builtin_paths) {
    if (path.empty()) {
      continue;
    }
    std::optional<DirectoryExtensionCandidate> candidate =
        ReadDirectoryExtensionCandidate(path, expected_extension_id);
    if (candidate && (!best || candidate->version > best->version)) {
      best = std::move(candidate);
    }
  }
  return best;
}

ExtensionLoadData DetermineBestExtensionPath(
    std::vector<base::FilePath> builtin_paths,
    base::FilePath user_update_path,
    std::string expected_extension_id) {
  ExtensionLoadData result;

  std::optional<DirectoryExtensionCandidate> best =
      FindBestBuiltinExtension(builtin_paths, expected_extension_id);
  std::optional<DirectoryExtensionCandidate> user =
      ReadDirectoryExtensionCandidate(user_update_path, expected_extension_id);
  if (user && (!best || user->version > best->version)) {
    best = std::move(user);
  }
  if (!best) {
    LOG(ERROR) << "ComponentExtensionManager: No valid extension path found";
    return result;
  }

  result.first = std::move(best->path);
  result.second = std::move(best->manifest);
  return result;
}

}  // namespace

namespace {
base::FilePath BuildPathFromString(const std::string& path_str) {
  if (path_str.empty()) {
    return base::FilePath();
  }

  base::FilePath module_path;
  if (!base::PathService::Get(base::DIR_MODULE, &module_path)) {
    return base::FilePath();
  }

  base::FilePath result = module_path;
  for (const std::string& component : base::SplitString(
           path_str, "/", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    result = result.AppendASCII(component);
  }
  return result;
}
}  // namespace

ComponentExtensionConfig::ComponentExtensionConfig() = default;

ComponentExtensionConfig::ComponentExtensionConfig(
    const ComponentExtensionConfig& other)
    : extension_name(other.extension_name),
      source(other.source),
      expected_extension_id(other.expected_extension_id),
      builtin_path(other.builtin_path),
      additional_builtin_paths(other.additional_builtin_paths),
      user_data_subdir(other.user_data_subdir),
      encrypted_zip_path(other.encrypted_zip_path),
      zip_password(other.zip_password),
      virtual_root_subdir(other.virtual_root_subdir),
      update_check_url(other.update_check_url) {}

ComponentExtensionConfig::ComponentExtensionConfig(
    ComponentExtensionConfig&& other) noexcept
    : extension_name(std::move(other.extension_name)),
      source(other.source),
      expected_extension_id(std::move(other.expected_extension_id)),
      builtin_path(std::move(other.builtin_path)),
      additional_builtin_paths(std::move(other.additional_builtin_paths)),
      user_data_subdir(std::move(other.user_data_subdir)),
      encrypted_zip_path(std::move(other.encrypted_zip_path)),
      zip_password(std::move(other.zip_password)),
      virtual_root_subdir(std::move(other.virtual_root_subdir)),
      update_check_url(std::move(other.update_check_url)) {}

ComponentExtensionConfig& ComponentExtensionConfig::operator=(
    const ComponentExtensionConfig& other) {
  if (this != &other) {
    extension_name = other.extension_name;
    source = other.source;
    expected_extension_id = other.expected_extension_id;
    builtin_path = other.builtin_path;
    additional_builtin_paths = other.additional_builtin_paths;
    user_data_subdir = other.user_data_subdir;
    encrypted_zip_path = other.encrypted_zip_path;
    zip_password = other.zip_password;
    virtual_root_subdir = other.virtual_root_subdir;
    update_check_url = other.update_check_url;
  }
  return *this;
}

ComponentExtensionConfig& ComponentExtensionConfig::operator=(
    ComponentExtensionConfig&& other) noexcept {
  if (this != &other) {
    extension_name = std::move(other.extension_name);
    source = other.source;
    expected_extension_id = std::move(other.expected_extension_id);
    builtin_path = std::move(other.builtin_path);
    additional_builtin_paths = std::move(other.additional_builtin_paths);
    user_data_subdir = std::move(other.user_data_subdir);
    encrypted_zip_path = std::move(other.encrypted_zip_path);
    zip_password = std::move(other.zip_password);
    virtual_root_subdir = std::move(other.virtual_root_subdir);
    update_check_url = std::move(other.update_check_url);
  }
  return *this;
}

ComponentExtensionConfig::~ComponentExtensionConfig() = default;

ComponentExtensionConfigBuilder&
ComponentExtensionConfigBuilder::SetExtensionName(const std::string& name) {
  config_.extension_name = name;
  return *this;
}

ComponentExtensionConfigBuilder&
ComponentExtensionConfigBuilder::SetExpectedExtensionId(const std::string& id) {
  config_.expected_extension_id = id;
  return *this;
}

ComponentExtensionConfigBuilder&
ComponentExtensionConfigBuilder::SetBuiltinPath(const std::string& path) {
  config_.builtin_path = path;
  return *this;
}

ComponentExtensionConfigBuilder&
ComponentExtensionConfigBuilder::AddAdditionalBuiltinPath(
    const std::string& path) {
  config_.additional_builtin_paths.push_back(path);
  return *this;
}

ComponentExtensionConfigBuilder&
ComponentExtensionConfigBuilder::SetUserDataSubdir(const std::string& subdir) {
  config_.user_data_subdir = subdir;
  return *this;
}

ComponentExtensionConfigBuilder&
ComponentExtensionConfigBuilder::SetEncryptedZipPath(const std::string& path) {
  config_.encrypted_zip_path = path;
  config_.source = ComponentExtensionSource::kEncryptedZip;
  return *this;
}

ComponentExtensionConfigBuilder& ComponentExtensionConfigBuilder::SetSource(
    ComponentExtensionSource source) {
  config_.source = source;
  return *this;
}

ComponentExtensionConfigBuilder&
ComponentExtensionConfigBuilder::SetZipPassword(const std::string& password) {
  config_.zip_password = password;
  return *this;
}

ComponentExtensionConfigBuilder&
ComponentExtensionConfigBuilder::SetVirtualRootSubdir(
    const std::string& subdir) {
  config_.virtual_root_subdir = subdir;
  return *this;
}

ComponentExtensionConfigBuilder&
ComponentExtensionConfigBuilder::SetUpdateCheckUrl(const GURL& url) {
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
    base::DictValue manifest,
    const base::FilePath& extension_path) {
  extensions::ComponentLoader* loader = GetComponentLoader(context);
  if (!loader) {
    LOG(ERROR) << "ComponentExtensionManager: Failed to get ComponentLoader";
    return std::string();
  }

  extensions::ExtensionId extension_id =
      loader->Add(std::move(manifest), extension_path);
  if (extension_id.empty()) {
    LOG(ERROR) << "ComponentExtensionManager: Failed to load extension from "
               << extension_path.value();
  }

  return extension_id;
}

extensions::ExtensionId ComponentExtensionManager::InstallEncryptedExtension(
    content::BrowserContext* context,
    const ComponentExtensionConfig& config,
    internal::EncryptedExtensionPackage package) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  Profile* profile = Profile::FromBrowserContext(context);
  if (!profile || config.extension_name.empty() ||
      config.expected_extension_id.empty() ||
      config.virtual_root_subdir.empty()) {
    return std::string();
  }

  const base::FilePath extension_root =
      profile->GetOriginalProfile()->GetPath().AppendASCII(
          config.virtual_root_subdir);

  auto cached = GetCachedEncryptedExtensions().find(package.archive_sha256);
  if (cached != GetCachedEncryptedExtensions().end()) {
    extensions::ChromeComponentExtensionResourceManager::
        RegisterMemoryExtensionResources(extension_root,
                                         cached->second.resource_ids);
    return AddExtensionWithManifest(context, config.extension_name,
                                    cached->second.manifest.Clone(),
                                    extension_root);
  }

  ui::ResourceBundle& resource_bundle = ui::ResourceBundle::GetSharedInstance();
  std::map<base::FilePath, int> resource_ids;
  std::vector<std::tuple<uint16_t, base::FilePath, std::string>> entries;
  uint16_t candidate = std::numeric_limits<uint16_t>::max();
  for (auto& [path, contents] : package.resources) {
    while (candidate && resource_bundle.HasDataResource(candidate)) {
      --candidate;
    }
    if (!candidate) {
      LOG(ERROR) << "No data resource ids remain for encrypted extension";
      return std::string();
    }
    resource_ids[path] = candidate;
    entries.emplace_back(candidate--, std::move(path), std::move(contents));
  }
  std::ranges::sort(entries, {},
                    [](const auto& entry) { return std::get<0>(entry); });

  constexpr size_t kHeaderSize = 12;
  constexpr size_t kEntrySize = 6;
  uint32_t data_offset =
      static_cast<uint32_t>(kHeaderSize + (entries.size() + 1) * kEntrySize);
  std::vector<uint8_t> data_pack;
  AppendLittleEndian32(&data_pack, 5);
  data_pack.insert(data_pack.end(), {0, 0, 0, 0});
  AppendLittleEndian16(&data_pack, static_cast<uint16_t>(entries.size()));
  AppendLittleEndian16(&data_pack, 0);
  for (const auto& entry : entries) {
    AppendLittleEndian16(&data_pack, std::get<0>(entry));
    AppendLittleEndian32(&data_pack, data_offset);
    data_offset += static_cast<uint32_t>(std::get<2>(entry).size());
  }
  AppendLittleEndian16(&data_pack, 0);
  AppendLittleEndian32(&data_pack, data_offset);
  for (const auto& entry : entries) {
    const std::string& contents = std::get<2>(entry);
    data_pack.insert(data_pack.end(), contents.begin(), contents.end());
  }

  GetEncryptedExtensionDataPacks().push_back(std::move(data_pack));
  resource_bundle.AddDataPackFromBuffer(GetEncryptedExtensionDataPacks().back(),
                                        ui::kScaleFactorNone);
  GetCachedEncryptedExtensions().emplace(
      package.archive_sha256,
      CachedEncryptedExtension{package.manifest.Clone(), resource_ids});
  extensions::ChromeComponentExtensionResourceManager::
      RegisterMemoryExtensionResources(extension_root, std::move(resource_ids));

  return AddExtensionWithManifest(context, config.extension_name,
                                  std::move(package.manifest), extension_root);
}

void ComponentExtensionManager::OnEncryptedExtensionSelected(
    base::WeakPtr<Profile> profile,
    const std::string& extension_name,
    OnExtensionLoadedCallback callback,
    internal::EncryptedExtensionSelection selection) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!selection.error.empty()) {
    LOG(WARNING) << "ComponentExtensionManager: " << selection.error;
  }
  if (!profile || !selection.package) {
    if (callback) {
      std::move(callback).Run(std::string());
    }
    return;
  }

  auto config_it = configs_.find(extension_name);
  if (config_it == configs_.end()) {
    if (callback) {
      std::move(callback).Run(std::string());
    }
    return;
  }

  extensions::ComponentLoader* loader = GetComponentLoader(profile.get());
  if (loader && !config_it->second.expected_extension_id.empty()) {
    loader->Remove(config_it->second.expected_extension_id);
  }
  extensions::ExtensionId id = InstallEncryptedExtension(
      profile.get(), config_it->second, std::move(*selection.package));
  if (callback) {
    std::move(callback).Run(id);
  }
}

extensions::ExtensionId ComponentExtensionManager::LoadExtension(
    content::BrowserContext* context,
    const std::string& extension_name,
    const base::FilePath& extension_path) {
  auto config = configs_.find(extension_name);
  if (config != configs_.end() && UsesEncryptedZip(config->second)) {
    LOG(ERROR) << "Plaintext path loading is disabled for encrypted component "
                  "extension: "
               << extension_name;
    return std::string();
  }
  if (extension_path.empty()) {
    LOG(ERROR) << "ComponentExtensionManager: Extension path is empty";
    return std::string();
  }

  base::FilePath manifest_path = extension_path.AppendASCII("manifest.json");
  if (!base::PathExists(manifest_path)) {
    LOG(ERROR) << "ComponentExtensionManager: Manifest not found at "
               << manifest_path.value();
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
  if (!ManifestMatchesExpectedExtensionId(
          root->GetDict(), config == configs_.end()
                               ? std::string()
                               : config->second.expected_extension_id)) {
    LOG(ERROR) << "ComponentExtensionManager: Unexpected extension id for "
               << extension_name;
    return std::string();
  }

  return AddExtensionWithManifest(context, extension_name,
                                  std::move(root->GetDict()), extension_path);
}

void ComponentExtensionManager::LoadExtensionFromDefaultPath(
    content::BrowserContext* context,
    const std::string& extension_name,
    OnExtensionLoadedCallback callback) {
  auto it = configs_.find(extension_name);
  if (it == configs_.end()) {
    LOG(ERROR) << "ComponentExtensionManager: Extension '" << extension_name
               << "' not registered";
    if (callback) {
      std::move(callback).Run(std::string());
    }
    return;
  }

  const ComponentExtensionConfig& config = it->second;
  Profile* profile = Profile::FromBrowserContext(context);
  if (!profile) {
    if (callback) {
      std::move(callback).Run(std::string());
    }
    return;
  }

  if (UsesEncryptedZip(config)) {
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce(&internal::LoadBestEncryptedExtensionPackage,
                       BuildPathFromString(config.encrypted_zip_path),
                       GetUserUpdatePath(context, extension_name),
                       config.zip_password, config.expected_extension_id),
        base::BindOnce(&ComponentExtensionManager::OnEncryptedExtensionSelected,
                       weak_factory_.GetWeakPtr(), profile->GetWeakPtr(),
                       extension_name, std::move(callback)));
    return;
  }

  std::vector<base::FilePath> builtin_paths;

  if (!config.builtin_path.empty()) {
    base::FilePath primary_builtin = BuildPathFromString(config.builtin_path);
    if (!primary_builtin.empty()) {
      builtin_paths.push_back(primary_builtin);
    }
  }

  for (const std::string& path_str : config.additional_builtin_paths) {
    base::FilePath additional_path = BuildPathFromString(path_str);
    if (!additional_path.empty()) {
      builtin_paths.push_back(additional_path);
    }
  }

  base::FilePath user_update_path = GetUserUpdatePath(context, extension_name);

  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&DetermineBestExtensionPath, std::move(builtin_paths),
                     user_update_path, config.expected_extension_id),
      base::BindOnce(&ComponentExtensionManager::OnExtensionPathDetermined,
                     weak_factory_.GetWeakPtr(), profile->GetWeakPtr(),
                     extension_name, std::move(callback)));
}

void ComponentExtensionManager::OnExtensionPathDetermined(
    base::WeakPtr<Profile> profile,
    const std::string& extension_name,
    OnExtensionLoadedCallback callback,
    std::pair<base::FilePath, std::optional<base::DictValue>> result) {
  base::FilePath path_to_load = result.first;
  std::optional<base::DictValue> manifest = std::move(result.second);

  if (!profile || path_to_load.empty() || !manifest.has_value()) {
    LOG(ERROR) << "ComponentExtensionManager: Failed to determine extension "
                  "path or parse manifest";
    if (callback) {
      std::move(callback).Run(std::string());
    }
    return;
  }

  extensions::ComponentLoader* loader = GetComponentLoader(profile.get());
  if (!loader) {
    LOG(ERROR) << "ComponentExtensionManager: Failed to get ComponentLoader";
    if (callback) {
      std::move(callback).Run(std::string());
    }
    return;
  }

  loader->Remove(path_to_load);
  const extensions::Extension* existing =
      FindExtension(profile.get(), extension_name);
  if (existing) {
    loader->Remove(existing->id());
  }

  extensions::ExtensionId id = AddExtensionWithManifest(
      profile.get(), extension_name, std::move(*manifest), path_to_load);
  if (callback) {
    std::move(callback).Run(id);
  }
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

  if (!it->second.expected_extension_id.empty()) {
    return registry->enabled_extensions().GetByID(
        it->second.expected_extension_id);
  }

  for (const auto& extension : registry->enabled_extensions()) {
    if (extension->name() == it->second.extension_name) {
      return extension.get();
    }
  }

  return nullptr;
}

bool ComponentExtensionManager::ShowExtension(
    content::BrowserContext* context,
    const std::string& extension_name) {
  if (!context) {
    LOG(WARNING)
        << "ComponentExtensionManager: Invalid context for ShowExtension";
    return false;
  }

  auto it = configs_.find(extension_name);
  if (it == configs_.end()) {
    LOG(WARNING) << "ComponentExtensionManager: Extension '" << extension_name
                 << "' not registered";
    return false;
  }

  const extensions::Extension* target_extension =
      FindExtension(context, extension_name);
  if (!target_extension) {
    LOG(WARNING) << "ComponentExtensionManager: Extension '"
                 << it->second.extension_name
                 << "' not found in enabled extensions registry.";
    return false;
  }

  XenonWebDialog::Show(context, target_extension->GetResourceURL("index.html"),
                       400, 300, base::UTF8ToUTF16(it->second.extension_name));

  if (it->second.update_check_url.is_valid()) {
    CheckForUpdates(context, extension_name, it->second.update_check_url);
  }

  return true;
}

void ComponentExtensionManager::CheckForUpdates(
    content::BrowserContext* context,
    const std::string& extension_name,
    const GURL& update_check_url) {
  if (!context) {
    return;
  }
  Profile* profile = Profile::FromBrowserContext(context);
  if (!profile) {
    return;
  }
  net::NetworkTrafficAnnotationTag traffic_annotation =
      net::DefineNetworkTrafficAnnotation("component_extension_update_check",
                                          R"(
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
      context->GetDefaultStoragePartition()
          ->GetURLLoaderFactoryForBrowserProcess()
          .get(),
      base::BindOnce(&ComponentExtensionManager::OnUpdateCheckComplete,
                     weak_factory_.GetWeakPtr(), profile->GetWeakPtr(),
                     extension_name),
      1024 * 1024);
}

void ComponentExtensionManager::LoadAllExtensions(
    content::BrowserContext* context) {
  for (const auto& [name, config] : configs_) {
    LoadExtensionFromDefaultPath(
        context, name,
        base::BindOnce(
            [](const std::string& name, const extensions::ExtensionId& id) {
              if (!id.empty()) {
                LOG(INFO) << "Component extension loaded: " << name << " ("
                          << id << ")";
              }
            },
            name));
  }
}

base::FilePath ComponentExtensionManager::GetBuiltinPath(
    const std::string& extension_name) const {
  auto it = configs_.find(extension_name);
  if (it == configs_.end()) {
    return base::FilePath();
  }
  if (UsesEncryptedZip(it->second)) {
    return BuildPathFromString(it->second.encrypted_zip_path);
  }
  if (it->second.builtin_path.empty()) {
    return base::FilePath();
  }
  return BuildPathFromString(it->second.builtin_path);
}

base::FilePath ComponentExtensionManager::GetUserUpdatePath(
    content::BrowserContext* context,
    const std::string& extension_name) const {
  auto it = configs_.find(extension_name);
  if (it == configs_.end() || it->second.user_data_subdir.empty()) {
    return base::FilePath();
  }

  Profile* profile = Profile::FromBrowserContext(context);
  if (!profile) {
    return base::FilePath();
  }

  return profile->GetOriginalProfile()->GetPath().AppendASCII(
      it->second.user_data_subdir);
}

void ComponentExtensionManager::OnUpdateCheckComplete(
    base::WeakPtr<Profile> profile,
    const std::string& extension_name,
    std::optional<std::string> response_body) {
  data_loader_.reset();
  if (!profile) {
    return;
  }

  auto it = configs_.find(extension_name);
  if (it == configs_.end()) {
    LOG(ERROR) << "ComponentExtensionManager: Extension '" << extension_name
               << "' not registered";
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
  const std::string* sha256 = result->GetDict().FindString("sha256");
  const std::string* signature = result->GetDict().FindString("signature");

  if (!version_str || !url_str || !sha256 || !signature) {
    LOG(ERROR) << "ComponentExtensionManager: Update manifest must contain "
                  "version, url, sha256, and signature.";
    return;
  }

  base::Version remote_version(*version_str);
  if (!remote_version.IsValid()) {
    LOG(ERROR) << "ComponentExtensionManager: Invalid remote version.";
    return;
  }

  const extensions::Extension* loaded_extension =
      FindExtension(profile.get(), extension_name);
  const std::string* public_key =
      loaded_extension ? loaded_extension->manifest()->FindStringPath(
                             extensions::manifest_keys::kPublicKey)
                       : nullptr;
  if (!loaded_extension || !loaded_extension->version().IsValid() ||
      !public_key) {
    LOG(ERROR) << "ComponentExtensionManager: Cannot verify an update before "
                  "the trusted component extension is loaded.";
    return;
  }

  GURL download_url(*url_str);
  internal::EncryptedExtensionUpdateMetadata metadata;
  metadata.version = *version_str;
  metadata.url = download_url.spec();
  metadata.sha256 = *sha256;
  metadata.signature = *signature;
  if (!download_url.is_valid() ||
      !internal::VerifyUpdateMetadataSignature(metadata, *public_key)) {
    LOG(ERROR) << "ComponentExtensionManager: Update manifest signature is "
                  "invalid.";
    return;
  }

  if (remote_version > loaded_extension->version()) {
    DownloadUpdate(profile, extension_name, download_url, *version_str, *sha256,
                   *signature);
  }
}

void ComponentExtensionManager::DownloadUpdate(
    base::WeakPtr<Profile> profile,
    const std::string& extension_name,
    const GURL& download_url,
    const std::string& version,
    const std::string& sha256,
    const std::string& signature) {
  if (!profile) {
    return;
  }
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

  download_loader_ = network::SimpleURLLoader::Create(
      std::move(resource_request), traffic_annotation);
  download_loader_->DownloadToTempFile(
      profile->GetDefaultStoragePartition()
          ->GetURLLoaderFactoryForBrowserProcess()
          .get(),
      base::BindOnce(&ComponentExtensionManager::OnDownloadComplete,
                     weak_factory_.GetWeakPtr(), profile, extension_name,
                     version, download_url.spec(), sha256, signature),
      kMaxUpdateDownloadBytes);
}

void ComponentExtensionManager::OnDownloadComplete(
    base::WeakPtr<Profile> profile,
    const std::string& extension_name,
    const std::string& version,
    const std::string& download_url,
    const std::string& sha256,
    const std::string& signature,
    base::FilePath response_path) {
  download_loader_.reset();
  if (response_path.empty()) {
    LOG(ERROR) << "ComponentExtensionManager: Download failed.";
    return;
  }

  auto config_it = configs_.find(extension_name);
  const extensions::Extension* loaded_extension =
      profile ? FindExtension(profile.get(), extension_name) : nullptr;
  const std::string* trusted_public_key =
      loaded_extension ? loaded_extension->manifest()->FindStringPath(
                             extensions::manifest_keys::kPublicKey)
                       : nullptr;
  if (!profile || config_it == configs_.end() || !trusted_public_key) {
    base::ThreadPool::PostTask(
        FROM_HERE, {base::MayBlock()},
        base::BindOnce(base::IgnoreResult(&base::DeleteFile), response_path));
    return;
  }

  if (UsesEncryptedZip(config_it->second)) {
    const ComponentExtensionConfig& config = config_it->second;
    internal::EncryptedExtensionUpdateMetadata metadata;
    metadata.version = version;
    metadata.url = download_url;
    metadata.sha256 = sha256;
    metadata.signature = signature;
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce(
            [](base::FilePath downloaded_path, base::FilePath update_path,
               std::string password, std::string expected_extension_id,
               std::string public_key,
               internal::EncryptedExtensionUpdateMetadata metadata) {
              std::string error;
              if (internal::PrepareEncryptedExtensionUpdate(
                      downloaded_path, update_path, password,
                      expected_extension_id, public_key, std::move(metadata),
                      &error)) {
                return std::string();
              }
              return error;
            },
            response_path, GetUserUpdatePath(profile.get(), extension_name),
            config.zip_password, config.expected_extension_id,
            *trusted_public_key, std::move(metadata)),
        base::BindOnce(&ComponentExtensionManager::OnEncryptedUpdatePrepared,
                       weak_factory_.GetWeakPtr(), profile, extension_name));
    return;
  }

  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&internal::FileMatchesSha256, response_path, sha256),
      base::BindOnce(&ComponentExtensionManager::OnDirectoryUpdateVerified,
                     weak_factory_.GetWeakPtr(), profile, extension_name,
                     response_path));
}

void ComponentExtensionManager::OnEncryptedUpdatePrepared(
    base::WeakPtr<Profile> profile,
    const std::string& extension_name,
    std::string error) {
  if (!error.empty()) {
    LOG(ERROR) << "ComponentExtensionManager: " << error;
    return;
  }
  if (!profile) {
    return;
  }
  LoadExtensionFromDefaultPath(profile.get(), extension_name,
                               base::NullCallback());
}

void ComponentExtensionManager::OnDirectoryUpdateVerified(
    base::WeakPtr<Profile> profile,
    const std::string& extension_name,
    base::FilePath response_path,
    bool verified) {
  if (!profile || !verified) {
    LOG(ERROR) << "ComponentExtensionManager: Downloaded update hash does not "
                  "match the signed update manifest.";
    base::ThreadPool::PostTask(
        FROM_HERE, {base::MayBlock()},
        base::BindOnce(base::IgnoreResult(&base::DeleteFile), response_path));
    return;
  }

  const base::FilePath user_update_path =
      GetUserUpdatePath(profile.get(), extension_name);
  const base::FilePath dest_dir = user_update_path.DirName();
  unzip::Unzip(unzip::LaunchUnzipper(), response_path, dest_dir,
               unzip::mojom::UnzipOptions::New(), unzip::AllContents(),
               base::DoNothing(),
               base::BindOnce(&ComponentExtensionManager::OnUnzipComplete,
                              weak_factory_.GetWeakPtr(), profile,
                              extension_name, response_path, user_update_path));
}

void ComponentExtensionManager::OnUnzipComplete(
    base::WeakPtr<Profile> profile,
    const std::string& extension_name,
    base::FilePath response_path,
    const base::FilePath& unzip_dir,
    bool success) {
  base::ThreadPool::PostTask(
      FROM_HERE, {base::MayBlock()},
      base::BindOnce(base::IgnoreResult(&base::DeleteFile),
                     std::move(response_path)));
  if (!success) {
    LOG(ERROR) << "ComponentExtensionManager: Failed to unzip update.";
    return;
  }
  if (profile) {
    LoadExtensionFromDefaultPath(profile.get(), extension_name,
                                 base::NullCallback());
  }
}

namespace {

constexpr char kXenonExtensionName[] = "Xenon Overlay Extension";
constexpr char kVideoControlsExtensionName[] = "XL Video Controls Bridge";

ComponentExtensionConfig CreateXenonConfig() {
  auto builder =
      ComponentExtensionConfigBuilder()
          .SetExtensionName(kXenonExtensionName)
          .SetExpectedExtensionId("mkkhnfilihmphalmfjjbobdnaikhbeoi")
          .SetUserDataSubdir("xenon_extension")
          .SetUpdateCheckUrl(GURL(
              "http://localhost:3000/download/xenon_update_manifest.json"));
#if BUILDFLAG(XENON_EXTENSION_SOURCE_ZIP)
  builder.SetEncryptedZipPath("resources/xenon_extension.zip")
      .SetZipPassword("xunlei@!@#$")
      .SetVirtualRootSubdir("XenonExtensionMemory");
#else
  builder.SetBuiltinPath("resources/xenon_extension");
#endif
  return builder.Build();
}

ComponentExtensionConfig CreateVideoControlsConfig() {
  return ComponentExtensionConfigBuilder()
      .SetExtensionName(kVideoControlsExtensionName)
      .SetExpectedExtensionId("ofmfjminmdmfdekchkolhdfidomocgac")
      .SetBuiltinPath("resources/video_controls_extension")
      .SetUserDataSubdir("video_controls_extension")
      .Build();
}

}  // namespace

XenonExtensionManager* XenonExtensionManager::GetInstance() {
  return base::Singleton<XenonExtensionManager>::get();
}

XenonExtensionManager::XenonExtensionManager()
    : xenon_extension_name_(kXenonExtensionName) {
  manager_.RegisterExtension(kXenonExtensionName, CreateXenonConfig());
  manager_.RegisterExtension(kVideoControlsExtensionName,
                            CreateVideoControlsConfig());
}

XenonExtensionManager::~XenonExtensionManager() = default;

void XenonExtensionManager::LoadAllExtensions(content::BrowserContext* context) {
  manager_.LoadAllExtensions(context);
}

// static
base::FilePath XenonExtensionManager::GetDefaultExtensionPath() {
  XenonExtensionManager* instance = GetInstance();
  if (!instance) {
    return base::FilePath();
  }
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
  manager_.LoadExtensionFromDefaultPath(context, xenon_extension_name_,
                                        std::move(callback));
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
