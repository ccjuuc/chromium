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
#include "base/values.h"
#include "extensions/common/extension_id.h"
#include "url/gurl.h"

namespace content {
class BrowserContext;
}

namespace network {
class SimpleURLLoader;
}

namespace extensions {
class ComponentLoader;
class Extension;
class ExtensionRegistry;
}  // namespace extensions

namespace xenon {

class ComponentExtensionConfig {
 public:
  ComponentExtensionConfig();
  ComponentExtensionConfig(const ComponentExtensionConfig& other);
  ComponentExtensionConfig(ComponentExtensionConfig&& other) noexcept;
  ComponentExtensionConfig& operator=(const ComponentExtensionConfig& other);
  ComponentExtensionConfig& operator=(ComponentExtensionConfig&& other) noexcept;
  ~ComponentExtensionConfig();

  std::string extension_name;
  std::string expected_extension_id;
  std::string builtin_path;  // Relative to module directory, e.g., "resources/xenon_extension"
  std::vector<std::string> additional_builtin_paths;  // Additional paths to check
  std::string user_data_subdir;  // Subdirectory in user data, e.g., "xenon_extension"
  GURL update_check_url;
};

class ComponentExtensionConfigBuilder {
 public:
  ComponentExtensionConfigBuilder() = default;
  ~ComponentExtensionConfigBuilder() = default;

  ComponentExtensionConfigBuilder& SetExtensionName(const std::string& name);
  ComponentExtensionConfigBuilder& SetExpectedExtensionId(const std::string& id);
  ComponentExtensionConfigBuilder& SetBuiltinPath(const std::string& path);
  ComponentExtensionConfigBuilder& AddAdditionalBuiltinPath(const std::string& path);
  ComponentExtensionConfigBuilder& SetUserDataSubdir(const std::string& subdir);
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

  extensions::ExtensionId LoadExtension(
      content::BrowserContext* context,
      const std::string& extension_name,
      const base::FilePath& extension_path);

  using OnExtensionLoadedCallback = base::OnceCallback<void(const extensions::ExtensionId&)>;
  void LoadExtensionFromDefaultPath(
      content::BrowserContext* context,
      const std::string& extension_name,
      OnExtensionLoadedCallback callback = base::NullCallback());

  const extensions::Extension* FindExtension(
      content::BrowserContext* context,
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
      base::Value::Dict manifest,
      const base::FilePath& extension_path);

  void OnExtensionPathDetermined(content::BrowserContext* context,
                                 const std::string& extension_name,
                                 OnExtensionLoadedCallback callback,
                                 std::pair<base::FilePath, std::optional<base::Value::Dict>> result);

  void OnUpdateCheckComplete(content::BrowserContext* context,
                             const std::string& extension_name,
                             std::optional<std::string> response_body);
  void DownloadUpdate(content::BrowserContext* context,
                      const std::string& extension_name,
                      const GURL& download_url,
                      const std::string& version);
  void OnDownloadComplete(content::BrowserContext* context,
                          const std::string& extension_name,
                          const std::string& version,
                          base::FilePath response_path);
  void OnDownloadCompleteOnUIThread(content::BrowserContext* context,
                                     const std::string& extension_name,
                                     base::FilePath response_path);
  void OnUnzipComplete(content::BrowserContext* context,
                       const std::string& extension_name,
                       const base::FilePath& unzip_dir,
                       bool success);

  std::unique_ptr<network::SimpleURLLoader> data_loader_;
  std::unique_ptr<network::SimpleURLLoader> download_loader_;
};

class XenonExtensionManager {
 public:
  static XenonExtensionManager* GetInstance();

  extensions::ExtensionId LoadExtension(
      content::BrowserContext* context,
      const base::FilePath& extension_path);

  using OnExtensionLoadedCallback = base::OnceCallback<void(const extensions::ExtensionId&)>;
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
