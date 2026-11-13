// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_module_resolver.h"

#include <utility>

#include "base/auto_reset.h"
#include "base/json/json_reader.h"
#include "base/strings/string_util.h"
#include "build/build_config.h"
#include "net/base/filename_util.h"
#include "url/gurl.h"

namespace xenon::ipc {
namespace {

bool HasExports(const base::DictValue& config) {
  const auto* value = config.Find("exports");
  return value && (value->is_string() || value->is_dict() || value->is_list());
}

bool IsRelative(const std::string& request) {
  if (request == "." || request == ".." || request.starts_with("./") ||
      request.starts_with("../")) {
    return true;
  }
#if BUILDFLAG(IS_WIN)
  return request.starts_with(".\\") || request.starts_with("..\\");
#else
  return false;
#endif
}

}  // namespace

XenonModuleResolver::XenonModuleResolver(base::FilePath root,
                                         base::FilePath executable,
                                         Normalize normalize,
                                         Read read,
                                         Builtin builtin,
                                         PackageMap package_map,
                                         LookupPaths lookup_paths)
    : root_(std::move(root)),
      executable_(std::move(executable)),
      normalize_(std::move(normalize)),
      read_(std::move(read)),
      builtin_(std::move(builtin)),
      package_map_(std::move(package_map)),
      lookup_paths_(std::move(lookup_paths)) {}

XenonModuleResolver::~XenonModuleResolver() = default;

bool XenonModuleResolver::InRoot(const base::FilePath& path) const {
  return path == root_ || root_.IsParent(path);
}

bool XenonModuleResolver::AllowedFile(const base::FilePath& path) const {
  // Preserve the existing explicit executable-adjacent native library rule.
  return InRoot(path) || (path.MatchesExtension(FILE_PATH_LITERAL(".node")) &&
                          path.DirName() == executable_.DirName());
}

bool XenonModuleResolver::ReadPackage(const base::FilePath& directory,
                                      Package* package,
                                      std::string* error) {
  if (!InRoot(directory)) {
    return false;
  }
  const auto filename = directory.AppendASCII("package.json");
  base::FilePath canonical;
  bool is_directory = false;
  if (!normalize_.Run(filename, &canonical, &is_directory) || is_directory) {
    return false;
  }
  if (!InRoot(canonical)) {
    *error = "MODULE_NOT_FOUND: Package config is outside the application";
    return false;
  }
  std::string text;
  if (!read_.Run(canonical, &text)) {
    return false;
  }
  auto parsed = base::JSONReader::Read(text, base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict()) {
    *error = "ERR_INVALID_PACKAGE_CONFIG: Invalid package config " +
             filename.AsUTF8Unsafe();
    return false;
  }
  *package = {canonical.DirName(), std::move(text),
              std::move(parsed->GetDict())};
  return true;
}

bool XenonModuleResolver::FindScope(const base::FilePath& parent,
                                    Package* package,
                                    std::string* error) {
  for (auto directory = parent.DirName(); InRoot(directory);
       directory = directory.DirName()) {
    if (directory.BaseName().value() == FILE_PATH_LITERAL("node_modules")) {
      break;
    }
    if (ReadPackage(directory, package, error)) {
      return true;
    }
    if (!error->empty() || directory == root_ ||
        directory == directory.DirName()) {
      break;
    }
  }
  return false;
}

std::optional<base::FilePath> XenonModuleResolver::ResolveLegacyPath(
    const base::FilePath& path,
    std::string* error,
    int depth) {
  if (depth > 32) {
    *error = "ERR_INVALID_PACKAGE_CONFIG: Package main resolution is too deep";
    return std::nullopt;
  }
  auto file =
      [&](const base::FilePath& candidate) -> std::optional<base::FilePath> {
    base::FilePath normalized;
    bool directory = false;
    if (normalize_.Run(candidate, &normalized, &directory) && !directory &&
        AllowedFile(normalized)) {
      return normalized;
    }
    return std::nullopt;
  };
  if (auto exact = file(path)) {
    return exact;
  }
  for (const auto* extension :
       {FILE_PATH_LITERAL("js"), FILE_PATH_LITERAL("json"),
        FILE_PATH_LITERAL("node")}) {
    if (auto extended = file(path.AddExtension(extension))) {
      return extended;
    }
  }
  base::FilePath directory;
  bool is_directory = false;
  if (!normalize_.Run(path, &directory, &is_directory) || !is_directory ||
      !InRoot(directory)) {
    return std::nullopt;
  }
  Package package;
  if (ReadPackage(directory, &package, error)) {
    const auto* main = package.config.FindString("main");
    if (main && !main->empty()) {
      const auto relative = base::FilePath::FromUTF8Unsafe(*main);
      if (!relative.IsAbsolute()) {
        const auto candidate = directory.Append(relative);
        base::FilePath normalized;
        bool ignored = false;
        const bool same = normalize_.Run(candidate, &normalized, &ignored) &&
                          normalized == directory;
        if (!same) {
          if (auto result = ResolveLegacyPath(candidate, error, depth + 1)) {
            return result;
          }
        }
      }
    }
  }
  if (!error->empty()) {
    return std::nullopt;
  }
  for (const char* index : {"index.js", "index.json", "index.node"}) {
    if (auto result = file(directory.AppendASCII(index))) {
      return result;
    }
  }
  return std::nullopt;
}

bool XenonModuleResolver::Describe(const base::FilePath& filename,
                                   Resolution* result,
                                   std::string* error,
                                   Mode mode) {
  result->filename = filename;
  result->url = net::FilePathToFileURL(filename).spec();
  result->format = Format::kCommonJs;
  const auto extension = filename.FinalExtension();
  if (extension == FILE_PATH_LITERAL(".mjs")) {
    result->format = Format::kModule;
  } else if (extension == FILE_PATH_LITERAL(".json")) {
    result->format = Format::kJson;
  } else if (extension == FILE_PATH_LITERAL(".js") ||
             (extension.empty() && mode != Mode::kRequire)) {
    Package package;
    if (FindScope(filename, &package, error)) {
      if (const auto* type = package.config.FindString("type");
          type && *type == "module") {
        result->format = Format::kModule;
      }
    }
  }
  return error->empty();
}

bool XenonModuleResolver::ResolveFileUrl(const std::string& url,
                                         Mode mode,
                                         Resolution* result,
                                         std::string* error) {
  const GURL parsed(url);
  const auto lower_path = base::ToLowerASCII(parsed.path());
  if (lower_path.find("%2f") != std::string::npos ||
      lower_path.find("%5c") != std::string::npos) {
    *error = "ERR_INVALID_MODULE_SPECIFIER: Encoded path separator in " + url;
    return false;
  }
  base::FilePath filename;
  if (!parsed.SchemeIsFile() || !net::FileURLToFilePath(parsed, &filename)) {
    *error = "ERR_INVALID_MODULE_SPECIFIER: Invalid file URL " + url;
    return false;
  }
  base::FilePath normalized;
  bool directory = false;
  if (!normalize_.Run(filename, &normalized, &directory)) {
    if (mode != Mode::kMetaResolve || !AllowedFile(filename)) {
      *error = std::string(mode == Mode::kRequire ? "MODULE_NOT_FOUND: "
                                                  : "ERR_MODULE_NOT_FOUND: ") +
               "Cannot find module '" + url + "'";
      return false;
    }
    normalized = filename;
  } else if (directory && mode != Mode::kMetaResolve) {
    *error =
        std::string(mode == Mode::kRequire ? "MODULE_NOT_FOUND: "
                                           : "ERR_UNSUPPORTED_DIR_IMPORT: ") +
        "Directory import " + url;
    return false;
  }
  if (!AllowedFile(normalized)) {
    *error = "ERR_MODULE_NOT_FOUND: Module is outside the application: " + url;
    return false;
  }
  if (!Describe(normalized, result, error, mode)) {
    return false;
  }
  // URL identity includes query/fragment even though the file read does not.
  if (mode != Mode::kRequire) {
    GURL::Replacements replacements;
    if (parsed.has_query()) {
      replacements.SetQueryStr(parsed.query());
    }
    if (parsed.has_ref()) {
      replacements.SetRefStr(parsed.ref());
    }
    result->url = GURL(result->url).ReplaceComponents(replacements).spec();
  }
  return true;
}

bool XenonModuleResolver::ResolveMap(const Package& package,
                                     const std::string& key,
                                     bool imports,
                                     Mode mode,
                                     Resolution* result,
                                     std::string* error,
                                     int depth) {
  std::string target;
  if (!package_map_.Run(package.text, key, mode != Mode::kRequire, imports,
                        &target, error)) {
    return false;
  }
  if (target.starts_with("./")) {
    const GURL base =
        net::FilePathToFileURL(package.directory.AppendASCII("package.json"));
    return ResolveFileUrl(base.Resolve(target).spec(), mode, result, error);
  }
  if (target.starts_with('#')) {
    *error = std::string(mode == Mode::kRequire ? "MODULE_NOT_FOUND: "
                                                : "ERR_MODULE_NOT_FOUND: ") +
             "Cannot find package '" + target + "'";
    return false;
  }
  return imports &&
         ResolveInternal(target, package.directory.AppendASCII("package.json"),
                         mode, result, error, depth + 1, true);
}

bool XenonModuleResolver::Resolve(
    const std::string& request,
    const base::FilePath& parent,
    Mode mode,
    Resolution* result,
    std::string* error,
    const std::vector<std::string>* conditions,
    const std::vector<base::FilePath>* lookup_paths) {
  base::AutoReset<const std::vector<std::string>*> active_conditions(
      &active_conditions_, conditions);
  base::AutoReset<const std::vector<base::FilePath>*> active_lookup_paths(
      &active_lookup_paths_, lookup_paths);
  error->clear();
  return ResolveInternal(request, parent, mode, result, error, 0);
}

bool XenonModuleResolver::ResolveInternal(const std::string& request,
                                          const base::FilePath& parent,
                                          Mode mode,
                                          Resolution* result,
                                          std::string* error,
                                          int depth,
                                          bool package_import_target) {
  if (depth > 32) {
    *error = "ERR_INVALID_PACKAGE_CONFIG: Recursive package imports";
    return false;
  }
  if (request.empty() || request.find('\0') != std::string::npos) {
    *error = "ERR_INVALID_ARG_VALUE: Invalid module specifier";
    return false;
  }
  if (builtin_.Run(request)) {
    result->filename.clear();
    result->format = Format::kBuiltin;
    // Electron's extra builtins are not Node builtins. Canonicalize aliases
    // only when the node: spelling is itself registered.
    const std::string node_name = "node:" + request;
    result->url =
        request.find(':') == std::string::npos && builtin_.Run(node_name)
            ? node_name
            : request;
    return true;
  }
  if (request.starts_with("node:")) {
    *error = "ERR_UNKNOWN_BUILTIN_MODULE: No such built-in module: " + request;
    return false;
  }
  const auto requested_path = base::FilePath::FromUTF8Unsafe(request);
  if (package_import_target &&
      (IsRelative(request) || requested_path.IsAbsolute() ||
       request.find('\\') != std::string::npos)) {
    *error = "ERR_INVALID_MODULE_SPECIFIER: Invalid package target " + request;
    return false;
  }
  if (mode != Mode::kRequire && request.starts_with("file:")) {
    return ResolveFileUrl(request, mode, result, error);
  }
  if (mode != Mode::kRequire && GURL(request).has_scheme()) {
    if (mode == Mode::kMetaResolve) {
      result->filename.clear();
      result->url = GURL(request).spec();
      result->format = Format::kModule;
      return true;
    }
    *error =
        "ERR_UNSUPPORTED_ESM_URL_SCHEME: Unsupported module URL " + request;
    return false;
  }
  if (requested_path.IsAbsolute() || IsRelative(request)) {
    if (mode != Mode::kRequire) {
      // Relative ESM specifiers are URL-relative, including ?query and #hash.
      const auto url = net::FilePathToFileURL(parent).Resolve(request);
      return ResolveFileUrl(url.spec(), mode, result, error);
    }
    const auto filename = requested_path.IsAbsolute()
                              ? requested_path
                              : parent.DirName().Append(requested_path);
    if (auto found = ResolveLegacyPath(filename, error)) {
      return Describe(*found, result, error);
    }
  } else if (request.starts_with('#')) {
    Package package;
    if (FindScope(parent, &package, error)) {
      return ResolveMap(package, request, true, mode, result, error, depth);
    }
    if (error->empty()) {
      *error = "ERR_PACKAGE_IMPORT_NOT_DEFINED: Package import " + request;
    }
    return false;
  } else {
    const auto first_slash = request.find('/');
    size_t end = first_slash;
    if (request.starts_with('@')) {
      if (first_slash == std::string::npos ||
          first_slash == request.size() - 1) {
        *error =
            "ERR_INVALID_MODULE_SPECIFIER: Invalid package name " + request;
        return false;
      }
      end = request.find('/', first_slash + 1);
    }
    const auto name = request.substr(0, end);
    if (name.find_first_of("%\\:") != std::string::npos ||
        name.starts_with('.')) {
      *error = "ERR_INVALID_MODULE_SPECIFIER: Invalid package name " + name;
      return false;
    }
    const auto key = end == std::string::npos ? "." : "." + request.substr(end);
    Package scope;
    if (FindScope(parent, &scope, error)) {
      const auto* own_name = scope.config.FindString("name");
      if (own_name && *own_name == name && HasExports(scope.config)) {
        return ResolveMap(scope, key, false, mode, result, error, depth);
      }
    }
    if (!error->empty()) {
      return false;
    }
    std::vector<base::FilePath> lookup_paths;
    if (mode == Mode::kRequire && !package_import_target && lookup_paths_) {
      if (active_lookup_paths_) {
        lookup_paths = *active_lookup_paths_;
      } else if (!lookup_paths_.Run(request, parent, &lookup_paths, error)) {
        return false;
      }
    } else {
      for (auto directory = parent.DirName(); InRoot(directory);
           directory = directory.DirName()) {
        if (directory.BaseName().value() != FILE_PATH_LITERAL("node_modules")) {
          lookup_paths.push_back(directory.AppendASCII("node_modules"));
        }
        if (directory == root_ || directory == directory.DirName()) {
          break;
        }
      }
    }
    for (const auto& modules : lookup_paths) {
      // Public lookup overrides choose search order, not filesystem authority.
      // Normalization below also rejects symlinks escaping the application.
      if (InRoot(modules)) {
        const auto package_root =
            modules.Append(base::FilePath::FromUTF8Unsafe(name));
        Package package;
        if (ReadPackage(package_root, &package, error) &&
            HasExports(package.config)) {
          return ResolveMap(package, key, false, mode, result, error, depth);
        }
        if (!error->empty()) {
          return false;
        }
        const auto candidate = modules.Append(requested_path);
        if ((mode == Mode::kRequire && !package_import_target) || key == ".") {
          if (auto found = ResolveLegacyPath(candidate, error)) {
            return Describe(*found, result, error, mode);
          }
          base::FilePath ignored;
          bool is_directory = false;
          if (mode != Mode::kRequire &&
              normalize_.Run(package_root, &ignored, &is_directory) &&
              is_directory && error->empty()) {
            *error = "ERR_MODULE_NOT_FOUND: Cannot find package entry " +
                     package_root.AsUTF8Unsafe();
          }
        } else {
          base::FilePath ignored;
          bool is_directory = false;
          if (normalize_.Run(package_root, &ignored, &is_directory) &&
              is_directory) {
            const GURL package_url = net::FilePathToFileURL(
                package_root.AppendASCII("package.json"));
            return ResolveFileUrl(package_url.Resolve(key).spec(), mode, result,
                                  error);
          }
        }
        if (!error->empty()) {
          return false;
        }
      }
    }
  }
  if (error->empty()) {
    *error = std::string(mode == Mode::kRequire ? "MODULE_NOT_FOUND: "
                                                : "ERR_MODULE_NOT_FOUND: ") +
             "Cannot find module '" + request + "'";
  }
  return false;
}

}  // namespace xenon::ipc
