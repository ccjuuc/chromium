#ifndef XENON_OVERLAY_CHROME_BROWSER_XENON_EXTENSION_MANAGER_H_
#define XENON_OVERLAY_CHROME_BROWSER_XENON_EXTENSION_MANAGER_H_

#include "base/files/file_path.h"
#include "base/memory/singleton.h"
#include "extensions/common/extension_id.h"

namespace content {
class BrowserContext;
}

namespace extensions {
class ComponentLoader;
class Extension;
class ExtensionRegistry;
}  // namespace extensions

namespace xenon {

// Manager for the Xenon component extension.
// Handles extension loading, registration, and display.
class XenonExtensionManager {
 public:
  static XenonExtensionManager* GetInstance();

  // Loads the Xenon component extension from the specified path.
  // Returns the extension ID if successful, empty string otherwise.
  extensions::ExtensionId LoadExtension(
      content::BrowserContext* context,
      const base::FilePath& extension_path);

  // Loads the Xenon component extension from the default location.
  // Returns the extension ID if successful, empty string otherwise.
  extensions::ExtensionId LoadExtensionFromDefaultPath(
      content::BrowserContext* context);

  // Finds and returns the Xenon extension if it's loaded.
  const extensions::Extension* FindExtension(
      content::BrowserContext* context);

  // Shows the Xenon extension in a WebDialog.
  // Returns true if the extension was found and displayed.
  bool ShowExtension(content::BrowserContext* context);

  // Gets the default extension path relative to the module directory.
  static base::FilePath GetDefaultExtensionPath();

  // Gets the extension name used for identification.
  static const char* GetExtensionName();

 private:
  friend struct base::DefaultSingletonTraits<XenonExtensionManager>;
  XenonExtensionManager();
  ~XenonExtensionManager();

  // Helper to get the ComponentLoader for a context.
  extensions::ComponentLoader* GetComponentLoader(
      content::BrowserContext* context);

  // Helper to get the ExtensionRegistry for a context.
  extensions::ExtensionRegistry* GetExtensionRegistry(
      content::BrowserContext* context);
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_XENON_EXTENSION_MANAGER_H_
