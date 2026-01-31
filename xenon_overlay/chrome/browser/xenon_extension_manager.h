#ifndef XENON_OVERLAY_CHROME_BROWSER_XENON_EXTENSION_MANAGER_H_
#define XENON_OVERLAY_CHROME_BROWSER_XENON_EXTENSION_MANAGER_H_

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/functional/callback_helpers.h"
#include "url/gurl.h"
#include "base/memory/singleton.h"
#include "extensions/common/extension_id.h"
#include "url/gurl.h"
#include "base/values.h"

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

// Configuration for a component extension manager.
struct ComponentExtensionConfig {
  // Extension name for identification and logging.
  const char* extension_name;
  
  // Expected extension ID (for validation, can be empty if not known).
  // If empty, the ID will be computed from the manifest's key field.
  std::string expected_extension_id;
  
  // Function to get the built-in extension path(s).
  // This is typically a path relative to the module directory.
  // For multiple built-in paths, return the primary one here.
  base::FilePath (*get_builtin_path)();
  
  // Optional: Function to get additional built-in extension paths.
  // Returns a list of alternative built-in paths to check.
  // If null, only get_builtin_path() will be used.
  std::vector<base::FilePath> (*get_additional_builtin_paths)() = nullptr;
  
  // Function to get the user update path for the extension.
  // This is typically in the user data directory.
  base::FilePath (*get_user_update_path)(content::BrowserContext* context);
  
  // Subdirectory name in user data directory (e.g., "xenon_overlay").
  const char* user_data_subdir;
  
  // Subdirectory name for extension within user data (e.g., "extension").
  const char* extension_subdir;
  
  // Update check URL. If empty, update checking will be disabled.
  GURL update_check_url;
  
  // Default constructor (out-of-line)
  ComponentExtensionConfig();
  
  // Copy constructor (out-of-line)
  ComponentExtensionConfig(const ComponentExtensionConfig& other);
  
  // Move constructor (out-of-line)
  ComponentExtensionConfig(ComponentExtensionConfig&& other) noexcept;
  
  // Copy assignment operator (out-of-line)
  ComponentExtensionConfig& operator=(const ComponentExtensionConfig& other);
  
  // Move assignment operator (out-of-line)
  ComponentExtensionConfig& operator=(ComponentExtensionConfig&& other) noexcept;
};

// Generic manager for component extensions.
// Handles extension loading, registration, and display with configurable paths and IDs.
class ComponentExtensionManager {
 public:
  // Creates a manager with the given configuration.
  explicit ComponentExtensionManager(const ComponentExtensionConfig& config);
  ~ComponentExtensionManager();

  // Loads the component extension from the specified path.
  // Returns the extension ID if successful, empty string otherwise.
  extensions::ExtensionId LoadExtension(
      content::BrowserContext* context,
      const base::FilePath& extension_path);

  using OnExtensionLoadedCallback = base::OnceCallback<void(const extensions::ExtensionId&)>;
  // Loads the component extension from the default location.
  // Asynchronously determines the best path (built-in vs user updated) to avoid UI thread blocking.
  void LoadExtensionFromDefaultPath(
      content::BrowserContext* context,
      OnExtensionLoadedCallback callback = base::NullCallback());

  // Finds and returns the extension if it's loaded.
  const extensions::Extension* FindExtension(
      content::BrowserContext* context);

  // Shows the extension in a WebDialog.
  // Returns true if the extension was found and displayed.
  bool ShowExtension(content::BrowserContext* context);

  // Gets the extension name used for identification.
  const char* GetExtensionName() const { return config_.extension_name; }

  // Gets the built-in extension path.
  base::FilePath GetBuiltinPath() const { return config_.get_builtin_path(); }

  // Gets the user update path for the extension.
  base::FilePath GetUserUpdatePath(content::BrowserContext* context) const {
    return config_.get_user_update_path(context);
  }

 private:
  ComponentExtensionConfig config_;

  // Helper to get the ComponentLoader for a context.
  extensions::ComponentLoader* GetComponentLoader(
      content::BrowserContext* context);

  // Helper to get the ExtensionRegistry for a context.
  extensions::ExtensionRegistry* GetExtensionRegistry(
      content::BrowserContext* context);

  // Helper to add extension with parsed manifest to ComponentLoader.
  // This is the common logic used by LoadExtension and OnExtensionPathDetermined.
  extensions::ExtensionId AddExtensionWithManifest(
      content::BrowserContext* context,
      base::Value::Dict manifest,
      const base::FilePath& extension_path);

  // Callback for when the extension path has been determined on a background thread.
  void OnExtensionPathDetermined(content::BrowserContext* context,
                                 OnExtensionLoadedCallback callback,
                                 std::pair<base::FilePath, std::optional<base::Value::Dict>> result);

 public:
  // Update-related methods (can be made optional via config if needed)
  void CheckForUpdates(content::BrowserContext* context,
                       const GURL& update_check_url);

 private:
  void OnUpdateCheckComplete(content::BrowserContext* context,
                             std::optional<std::string> response_body);
  void DownloadUpdate(content::BrowserContext* context,
                      const GURL& download_url,
                      const std::string& version);
  void OnDownloadComplete(content::BrowserContext* context,
                          const std::string& version,
                          base::FilePath response_path);
  void OnDownloadCompleteOnUIThread(content::BrowserContext* context,
                                     base::FilePath response_path);
  void OnUnzipComplete(content::BrowserContext* context,
                       const base::FilePath& unzip_dir,
                       bool success);

  std::unique_ptr<network::SimpleURLLoader> data_loader_;
  std::unique_ptr<network::SimpleURLLoader> download_loader_;
};

// Manager for the Xenon component extension.
// This is a convenience wrapper around ComponentExtensionManager with Xenon-specific configuration.
class XenonExtensionManager {
 public:
  static XenonExtensionManager* GetInstance();

  // Loads the Xenon component extension from the specified path.
  extensions::ExtensionId LoadExtension(
      content::BrowserContext* context,
      const base::FilePath& extension_path);

  using OnExtensionLoadedCallback = base::OnceCallback<void(const extensions::ExtensionId&)>;
  void LoadExtensionFromDefaultPath(
      content::BrowserContext* context,
      OnExtensionLoadedCallback callback = base::NullCallback());

  const extensions::Extension* FindExtension(content::BrowserContext* context);
  bool ShowExtension(content::BrowserContext* context);

  // Checks for updates from the update server.
  void CheckForUpdates(content::BrowserContext* context,
                       const GURL& update_check_url);

  static base::FilePath GetDefaultExtensionPath();
  static const char* GetExtensionName();

 private:
  friend struct base::DefaultSingletonTraits<XenonExtensionManager>;
  XenonExtensionManager();
  ~XenonExtensionManager();

  std::unique_ptr<ComponentExtensionManager> manager_;
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_XENON_EXTENSION_MANAGER_H_
