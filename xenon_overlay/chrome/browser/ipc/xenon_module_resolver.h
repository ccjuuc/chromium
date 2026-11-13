// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_MODULE_RESOLVER_H_
#define XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_MODULE_RESOLVER_H_

#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr_exclusion.h"
#include "base/values.h"

namespace xenon::ipc {

// Node package/path resolution over the host's canonical filesystem view.
// Package maps are evaluated by the shared JS implementation so condition
// ordering is retained (base::DictValue does not preserve JSON member order).
class XenonModuleResolver {
 public:
  enum class Mode { kRequire, kImport, kMetaResolve };
  enum class Format { kCommonJs, kModule, kJson, kBuiltin };
  struct Resolution {
    base::FilePath filename;
    std::string url;
    Format format = Format::kCommonJs;
  };
  using Normalize = base::RepeatingCallback<
      bool(const base::FilePath&, base::FilePath*, bool*)>;
  using Read =
      base::RepeatingCallback<bool(const base::FilePath&, std::string*)>;
  using Builtin = base::RepeatingCallback<bool(const std::string&)>;
  using PackageMap = base::RepeatingCallback<bool(const std::string&,
                                                  const std::string&,
                                                  bool,
                                                  bool,
                                                  std::string*,
                                                  std::string*)>;
  using LookupPaths = base::RepeatingCallback<bool(const std::string&,
                                                   const base::FilePath&,
                                                   std::vector<base::FilePath>*,
                                                   std::string*)>;

  XenonModuleResolver(base::FilePath root,
                      base::FilePath executable,
                      Normalize normalize,
                      Read read,
                      Builtin builtin,
                      PackageMap package_map,
                      LookupPaths lookup_paths = {});
  ~XenonModuleResolver();

  bool Resolve(const std::string& request,
               const base::FilePath& parent,
               Mode mode,
               Resolution* result,
               std::string* error,
               const std::vector<std::string>* conditions = nullptr,
               const std::vector<base::FilePath>* lookup_paths = nullptr);
  const std::vector<std::string>* active_conditions() const {
    return active_conditions_;
  }
  bool Describe(const base::FilePath& filename,
                Resolution* result,
                std::string* error,
                Mode mode = Mode::kRequire);
  std::optional<base::FilePath> ResolveLegacyPath(const base::FilePath& path,
                                                  std::string* error,
                                                  int depth = 0);

 private:
  struct Package {
    base::FilePath directory;
    std::string text;
    base::DictValue config;
  };
  bool ReadPackage(const base::FilePath& directory,
                   Package* package,
                   std::string* error);
  bool FindScope(const base::FilePath& parent,
                 Package* package,
                 std::string* error);
  bool ResolveInternal(const std::string& request,
                       const base::FilePath& parent,
                       Mode mode,
                       Resolution* result,
                       std::string* error,
                       int depth,
                       bool package_import_target = false);
  bool ResolveMap(const Package& package,
                  const std::string& key,
                  bool imports,
                  Mode mode,
                  Resolution* result,
                  std::string* error,
                  int depth);
  bool ResolveFileUrl(const std::string& url,
                      Mode mode,
                      Resolution* result,
                      std::string* error);
  bool InRoot(const base::FilePath& path) const;
  bool AllowedFile(const base::FilePath& path) const;

  const base::FilePath root_;
  const base::FilePath executable_;
  const Normalize normalize_;
  const Read read_;
  const Builtin builtin_;
  const PackageMap package_map_;
  const LookupPaths lookup_paths_;
  RAW_PTR_EXCLUSION const std::vector<std::string>* active_conditions_ =
      nullptr;
  RAW_PTR_EXCLUSION const std::vector<base::FilePath>* active_lookup_paths_ =
      nullptr;
};

}  // namespace xenon::ipc

#endif  // XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_MODULE_RESOLVER_H_
