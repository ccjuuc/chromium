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

namespace xenon {

namespace {

constexpr char kExtensionName[] = "Xenon Overlay Extension";

}  // namespace

// static
XenonExtensionManager* XenonExtensionManager::GetInstance() {
  return base::Singleton<XenonExtensionManager>::get();
}

XenonExtensionManager::XenonExtensionManager() = default;

XenonExtensionManager::~XenonExtensionManager() = default;

// static
base::FilePath XenonExtensionManager::GetDefaultExtensionPath() {
  base::FilePath extension_path;
  if (base::PathService::Get(base::DIR_MODULE, &extension_path)) {
    extension_path = extension_path.AppendASCII("resources")
                                   .AppendASCII("xenon_overlay")
                                   .AppendASCII("extension");
  }
  return extension_path;
}

// static
const char* XenonExtensionManager::GetExtensionName() {
  return kExtensionName;
}

extensions::ComponentLoader* XenonExtensionManager::GetComponentLoader(
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

extensions::ExtensionRegistry* XenonExtensionManager::GetExtensionRegistry(
    content::BrowserContext* context) {
  if (!context) {
    return nullptr;
  }
  return extensions::ExtensionRegistry::Get(context);
}

extensions::ExtensionId XenonExtensionManager::LoadExtension(
    content::BrowserContext* context,
    const base::FilePath& extension_path) {
  if (extension_path.empty()) {
    LOG(ERROR) << "XenonExtensionManager: Extension path is empty";
    return std::string();
  }

  extensions::ComponentLoader* loader = GetComponentLoader(context);
  if (!loader) {
    LOG(ERROR) << "XenonExtensionManager: Failed to get ComponentLoader";
    return std::string();
  }

  extensions::ExtensionId extension_id = loader->AddOrReplace(extension_path);
  if (extension_id.empty()) {
    LOG(ERROR) << "XenonExtensionManager: Failed to load extension from "
               << extension_path.value();
  } else {
    LOG(INFO) << "XenonExtensionManager: Successfully loaded extension with ID: "
              << extension_id << " from " << extension_path.value();
  }

  return extension_id;
}

extensions::ExtensionId XenonExtensionManager::LoadExtensionFromDefaultPath(
    content::BrowserContext* context) {
  base::FilePath extension_path = GetDefaultExtensionPath();
  if (extension_path.empty()) {
    LOG(ERROR) << "XenonExtensionManager: Failed to get default extension path";
    return std::string();
  }
  return LoadExtension(context, extension_path);
}

const extensions::Extension* XenonExtensionManager::FindExtension(
    content::BrowserContext* context) {
  if (!context) {
    return nullptr;
  }

  extensions::ExtensionRegistry* registry = GetExtensionRegistry(context);
  if (!registry) {
    return nullptr;
  }

  for (const auto& extension : registry->enabled_extensions()) {
    if (extension->name() == kExtensionName) {
      return extension.get();
    }
  }

  return nullptr;
}

bool XenonExtensionManager::ShowExtension(content::BrowserContext* context) {
  if (!context) {
    LOG(WARNING) << "XenonExtensionManager: Invalid context for ShowExtension";
    return false;
  }

  const extensions::Extension* target_extension = FindExtension(context);
  if (!target_extension) {
    LOG(WARNING) << "XenonExtensionManager: Extension '" << kExtensionName
                 << "' not found in enabled extensions registry.";
    return false;
  }

  GURL extension_url = target_extension->GetResourceURL("index.html");
  LOG(INFO) << "XenonExtensionManager: Found extension with ID: "
            << target_extension->id();
  LOG(INFO) << "XenonExtensionManager: Opening URL: " << extension_url.spec();

  XenonWebDialog::Show(context, extension_url, 400, 300,
                       u"Xenon Overlay Extension");
  return true;
}

}  // namespace xenon
