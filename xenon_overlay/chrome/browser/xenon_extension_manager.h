#ifndef XENON_OVERLAY_CHROME_BROWSER_XENON_EXTENSION_MANAGER_H_
#define XENON_OVERLAY_CHROME_BROWSER_XENON_EXTENSION_MANAGER_H_

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/functional/callback_helpers.h"
#include "base/memory/singleton.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "extensions/common/extension_id.h"
#include "url/gurl.h"

namespace content {
class BrowserContext;
}

class Profile;

namespace network {
class SimpleURLLoader;
}

namespace extensions {
class ComponentLoader;
class Extension;
class ExtensionRegistry;
}  // namespace extensions

namespace xenon {

namespace internal {
struct EncryptedExtensionPackage;
struct EncryptedExtensionSelection;
}  // namespace internal

enum class ComponentExtensionSource {
  kDirectory,
  kEncryptedZip,
};

class ComponentExtensionConfig {
 public:
  ComponentExtensionConfig();
  ComponentExtensionConfig(const ComponentExtensionConfig& other);
  ComponentExtensionConfig(ComponentExtensionConfig&& other) noexcept;
  ComponentExtensionConfig& operator=(const ComponentExtensionConfig& other);
  ComponentExtensionConfig& operator=(
      ComponentExtensionConfig&& other) noexcept;
  ~ComponentExtensionConfig();

  std::string extension_name;
  ComponentExtensionSource source = ComponentExtensionSource::kDirectory;
  std::string expected_extension_id;
  // Relative to the module directory, e.g. "resources/xenon_extension".
  std::string builtin_path;
  std::vector<std::string> additional_builtin_paths;
  // Profile subdirectory used for downloaded updates.
  std::string user_data_subdir;
  std::string encrypted_zip_path;
  std::string zip_password;
  std::string virtual_root_subdir;
  GURL update_check_url;
};

class ComponentExtensionConfigBuilder {
 public:
  ComponentExtensionConfigBuilder() = default;
  ~ComponentExtensionConfigBuilder() = default;

  ComponentExtensionConfigBuilder& SetExtensionName(const std::string& name);
  ComponentExtensionConfigBuilder& SetExpectedExtensionId(
      const std::string& id);
  ComponentExtensionConfigBuilder& SetBuiltinPath(const std::string& path);
  ComponentExtensionConfigBuilder& AddAdditionalBuiltinPath(
      const std::string& path);
  ComponentExtensionConfigBuilder& SetUserDataSubdir(const std::string& subdir);
  ComponentExtensionConfigBuilder& SetEncryptedZipPath(const std::string& path);
  ComponentExtensionConfigBuilder& SetSource(ComponentExtensionSource source);
  ComponentExtensionConfigBuilder& SetZipPassword(const std::string& password);
  ComponentExtensionConfigBuilder& SetVirtualRootSubdir(
      const std::string& subdir);
  ComponentExtensionConfigBuilder& SetUpdateCheckUrl(const GURL& url);

  ComponentExtensionConfig Build();

 private:
  ComponentExtensionConfig config_;
};

class ComponentExtensionManager {
 public:
  ComponentExtensionManager();
  ~ComponentExtensionManager();

  void RegisterExtension(const std::string& extension_name,
                         const ComponentExtensionConfig& config);

  extensions::ExtensionId LoadExtension(content::BrowserContext* context,
                                        const std::string& extension_name,
                                        const base::FilePath& extension_path);

  using OnExtensionLoadedCallback =
      base::OnceCallback<void(const extensions::ExtensionId&)>;
  void LoadExtensionFromDefaultPath(
      content::BrowserContext* context,
      const std::string& extension_name,
      OnExtensionLoadedCallback callback = base::NullCallback());

  const extensions::Extension* FindExtension(content::BrowserContext* context,
                                             const std::string& extension_name);

  bool ShowExtension(content::BrowserContext* context,
                     const std::string& extension_name);

  void CheckForUpdates(content::BrowserContext* context,
                       const std::string& extension_name,
                       const GURL& update_check_url);

  void LoadAllExtensions(content::BrowserContext* context);

  base::FilePath GetBuiltinPath(const std::string& extension_name) const;
  base::FilePath GetUserUpdatePath(content::BrowserContext* context,
                                   const std::string& extension_name) const;

 private:
  std::map<std::string, ComponentExtensionConfig> configs_;

  extensions::ComponentLoader* GetComponentLoader(
      content::BrowserContext* context);

  extensions::ExtensionRegistry* GetExtensionRegistry(
      content::BrowserContext* context);

  extensions::ExtensionId AddExtensionWithManifest(
      content::BrowserContext* context,
      const std::string& extension_name,
      base::DictValue manifest,
      const base::FilePath& extension_path);

  extensions::ExtensionId InstallEncryptedExtension(
      content::BrowserContext* context,
      const ComponentExtensionConfig& config,
      internal::EncryptedExtensionPackage package);
  void OnEncryptedExtensionSelected(
      base::WeakPtr<Profile> profile,
      const std::string& extension_name,
      OnExtensionLoadedCallback callback,
      internal::EncryptedExtensionSelection selection);

  void OnExtensionPathDetermined(
      base::WeakPtr<Profile> profile,
      const std::string& extension_name,
      OnExtensionLoadedCallback callback,
      std::pair<base::FilePath, std::optional<base::DictValue>> result);

  void OnUpdateCheckComplete(base::WeakPtr<Profile> profile,
                             const std::string& extension_name,
                             std::optional<std::string> response_body);
  void DownloadUpdate(base::WeakPtr<Profile> profile,
                      const std::string& extension_name,
                      const GURL& download_url,
                      const std::string& version,
                      const std::string& sha256,
                      const std::string& signature);
  void OnDownloadComplete(base::WeakPtr<Profile> profile,
                          const std::string& extension_name,
                          const std::string& version,
                          const std::string& download_url,
                          const std::string& sha256,
                          const std::string& signature,
                          base::FilePath response_path);
  void OnEncryptedUpdatePrepared(base::WeakPtr<Profile> profile,
                                 const std::string& extension_name,
                                 std::string error);
  void OnDirectoryUpdateVerified(base::WeakPtr<Profile> profile,
                                 const std::string& extension_name,
                                 base::FilePath response_path,
                                 bool verified);
  void OnUnzipComplete(base::WeakPtr<Profile> profile,
                       const std::string& extension_name,
                       base::FilePath response_path,
                       const base::FilePath& unzip_dir,
                       bool success);

  std::unique_ptr<network::SimpleURLLoader> data_loader_;
  std::unique_ptr<network::SimpleURLLoader> download_loader_;
  base::WeakPtrFactory<ComponentExtensionManager> weak_factory_{this};
};

class XenonExtensionManager {
 public:
  static XenonExtensionManager* GetInstance();

  void LoadAllExtensions(content::BrowserContext* context);

  extensions::ExtensionId LoadExtension(content::BrowserContext* context,
                                        const base::FilePath& extension_path);

  using OnExtensionLoadedCallback =
      base::OnceCallback<void(const extensions::ExtensionId&)>;
  void LoadExtensionFromDefaultPath(
      content::BrowserContext* context,
      OnExtensionLoadedCallback callback = base::NullCallback());

  const extensions::Extension* FindExtension(content::BrowserContext* context);
  bool ShowExtension(content::BrowserContext* context);

  void CheckForUpdates(content::BrowserContext* context,
                       const GURL& update_check_url);

  static base::FilePath GetDefaultExtensionPath();
  static const std::string& GetExtensionName();

 private:
  friend struct base::DefaultSingletonTraits<XenonExtensionManager>;
  XenonExtensionManager();
  ~XenonExtensionManager();

  ComponentExtensionManager manager_;
  std::string xenon_extension_name_;
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_XENON_EXTENSION_MANAGER_H_
