// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ui/xenon_mac_player_adapter.h"

#include <algorithm>

#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "xenon_overlay/chrome/browser/ui/adapters/xunlei_mac_player_adapter.h"

namespace xenon {
namespace {

constexpr char kDeclarationName[] = "xenon-native-player.json";
constexpr size_t kMaxDeclarationBytes = 4 * 1024;

bool IsFileInside(const base::FilePath& directory,
                  const base::FilePath& path,
                  base::FilePath* canonical_path) {
  base::File::Info info;
  return base::NormalizeFilePath(path, canonical_path) &&
         directory.IsParent(*canonical_path) &&
         base::GetFileInfo(*canonical_path, &info) && !info.is_directory;
}

}  // namespace

bool MacPlayerAdapter::AllowsExport(std::string_view name) const {
  return std::ranges::contains(allowed_exports, name);
}

bool MacPlayerAdapter::IsProviderRegistration(std::string_view name) const {
  return std::ranges::any_of(callbacks, [name](const auto& callback) {
    return callback.method == name;
  });
}

std::string_view MacPlayerAdapter::DetachMethod(std::string_view method) const {
  for (const auto& subscription : subscriptions) {
    if (subscription.attach == method) {
      return subscription.detach;
    }
  }
  return {};
}

bool MacPlayerAdapter::ExcludesInstanceMethod(std::string_view method) const {
  return std::ranges::contains(excluded_instance_methods, method);
}

const MacPlayerAdapter* FindMacPlayerAdapter(std::string_view id, int version) {
  // A declaration can select a registered contract, never provide native code
  // paths or extend the Browser process's export/method allowlist.
  const auto& adapter = XunleiMacPlayerAdapterV1();
  return adapter.id == id && adapter.version == version ? &adapter : nullptr;
}

ResolvedMacPlayerAdapter ResolveBundledMacPlayerAdapter(
    const base::FilePath& outer_bundle,
    const base::FilePath& resources) {
  if (outer_bundle.empty() || resources.empty() || !outer_bundle.IsAbsolute() ||
      !resources.IsAbsolute() || outer_bundle.ReferencesParent() ||
      resources.ReferencesParent()) {
    return {};
  }

  const auto bundle = outer_bundle.StripTrailingSeparators();
  const auto resource_path = resources.StripTrailingSeparators();
  const auto apps = bundle.AppendASCII("Contents")
                        .AppendASCII("Resources")
                        .AppendASCII("xenon-apps");
  const auto app = resource_path.DirName().DirName();
  if (resource_path.BaseName().value() != FILE_PATH_LITERAL("Resources") ||
      resource_path.DirName().BaseName().value() !=
          FILE_PATH_LITERAL("Contents") ||
      !app.MatchesExtension(FILE_PATH_LITERAL(".app"))) {
    return {};
  }

  base::FilePath real_bundle;
  base::FilePath real_apps;
  base::FilePath real_app;
  base::FilePath real_resources;
  if (!base::NormalizeFilePath(bundle, &real_bundle) ||
      !base::NormalizeFilePath(apps, &real_apps) ||
      !base::NormalizeFilePath(app, &real_app) ||
      !base::NormalizeFilePath(resource_path, &real_resources)) {
    return {};
  }
  const auto expected_apps = real_bundle.AppendASCII("Contents")
                                 .AppendASCII("Resources")
                                 .AppendASCII("xenon-apps");
  // The fixed trust root must not redirect outside the outer app, and the
  // selected app must be an immediate child rather than an arbitrary nested
  // directory or a symlink to an external application. Compare canonical paths:
  // NSBundle and application metadata can spell the same root /var or
  // /private/var.
  if (real_apps != expected_apps || real_app.DirName() != real_apps ||
      !real_app.MatchesExtension(FILE_PATH_LITERAL(".app")) ||
      real_resources !=
          real_app.AppendASCII("Contents").AppendASCII("Resources") ||
      !base::DirectoryExists(real_resources)) {
    return {};
  }

  base::FilePath declaration_path;
  std::string text;
  if (!IsFileInside(real_resources, resource_path.AppendASCII(kDeclarationName),
                    &declaration_path) ||
      !base::ReadFileToStringWithMaxSize(declaration_path, &text,
                                         kMaxDeclarationBytes)) {
    return {};
  }
  auto declaration = base::JSONReader::Read(text, base::JSON_PARSE_RFC);
  if (!declaration || !declaration->is_dict()) {
    return {};
  }
  const auto& root = declaration->GetDict();
  const auto* selection = root.FindDict("adapter");
  if (root.size() != 2 || root.FindInt("schemaVersion") != 1 || !selection ||
      selection->size() != 2) {
    return {};
  }
  const auto* id = selection->FindString("id");
  const auto version = selection->FindInt("version");
  const auto* adapter =
      id && version ? FindMacPlayerAdapter(*id, *version) : nullptr;
  if (!adapter) {
    return {};
  }

  const auto module_relative =
      base::FilePath::FromUTF8Unsafe(adapter->module_relative_path);
  base::FilePath module;
  if (module_relative.empty() || module_relative.IsAbsolute() ||
      module_relative.ReferencesParent() ||
      !IsFileInside(real_resources, resource_path.Append(module_relative),
                    &module)) {
    return {};
  }
  return {adapter, module.AsUTF8Unsafe()};
}

}  // namespace xenon
