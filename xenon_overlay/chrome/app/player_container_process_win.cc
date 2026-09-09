// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/app/player_container_process_win.h"

#include <cstdlib>

#include "base/base_paths.h"
#include "base/command_line.h"
#include "base/environment.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/logging.h"
#include "base/path_service.h"
#include "base/scoped_native_library.h"

namespace xenon {

namespace {

constexpr char kServerIdSwitch[] = "server-id";
constexpr char kClientIdSwitch[] = "client-id";
constexpr char kProcessIdSwitch[] = "process-id";
constexpr base::FilePath::CharType kPlayerDirectoryName[] =
    FILE_PATH_LITERAL("player");
constexpr base::FilePath::CharType kContainerLibraryName[] =
    FILE_PATH_LITERAL("containor.dll");
constexpr char kInitializeContainerFunction[] = "InitContainor";
constexpr char kHostedAppDirectoryEnvironmentVariable[] =
    "XENON_HOSTED_APP_DIR";

using InitializeContainerFunction = void (*)();

bool IsPlayerContainerProcess(const base::CommandLine& command_line) {
  return !command_line.GetSwitchValueASCII(kServerIdSwitch).empty() &&
         !command_line.GetSwitchValueASCII(kClientIdSwitch).empty() &&
         !command_line.GetSwitchValueASCII(kProcessIdSwitch).empty();
}

}  // namespace

std::optional<int> MaybeRunPlayerContainerProcess(
    const base::CommandLine& command_line) {
  if (!IsPlayerContainerProcess(command_line)) {
    return std::nullopt;
  }

  base::FilePath xenon_executable;
  if (!base::PathService::Get(base::FILE_EXE, &xenon_executable)) {
    LOG(ERROR) << "Failed to resolve Xenon executable path";
    return EXIT_FAILURE;
  }

  base::FilePath player_directory = xenon_executable.DirName().Append(
      kPlayerDirectoryName);
  if (std::unique_ptr<base::Environment> environment =
          base::Environment::Create()) {
    if (std::optional<std::string> hosted_directory =
            environment->GetVar(kHostedAppDirectoryEnvironmentVariable)) {
      const base::FilePath candidate =
          base::FilePath::FromUTF8Unsafe(*hosted_directory).Append(
              kPlayerDirectoryName);
      if (base::DirectoryExists(candidate)) {
        player_directory = candidate;
      }
    }
  }

  const base::FilePath container_library =
      player_directory.Append(kContainerLibraryName);
  if (!base::PathExists(container_library)) {
    LOG(ERROR) << "Player container library is missing: "
               << container_library.AsUTF8Unsafe();
    return EXIT_FAILURE;
  }

  base::ScopedNativeLibrary library(container_library);
  if (!library.is_valid()) {
    const base::NativeLibraryLoadError* error = library.GetError();
    LOG(ERROR) << "Failed to load player container library: "
               << container_library.AsUTF8Unsafe()
               << ", error=" << (error ? error->ToString() : "unknown error");
    return EXIT_FAILURE;
  }

  auto initialize_container = reinterpret_cast<InitializeContainerFunction>(
      library.GetFunctionPointer(kInitializeContainerFunction));
  if (!initialize_container) {
    LOG(ERROR) << "Player container does not export "
               << kInitializeContainerFunction;
    return EXIT_FAILURE;
  }

  initialize_container();
  return EXIT_SUCCESS;
}

}  // namespace xenon
