// Copyright 2020 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/service_host/utility_sandbox_delegate.h"

#include <optional>

#include "base/check.h"
#include "build/build_config.h"
#include "content/browser/sandboxed_process_launcher_delegate.h"
#include "content/common/features.h"
#include "content/public/common/zygote/zygote_buildflags.h"
#include "media/gpu/buildflags.h"
#include "sandbox/policy/mojom/sandbox.mojom.h"

#if BUILDFLAG(ENABLE_XENON_SERVICE)
#include "base/logging.h"
#include "base/process/launch.h"
#include "content/public/common/content_switches.h"
#include "xenon_overlay/public/xenon_ipc_switches.h"
#if BUILDFLAG(IS_WIN)
#include <windows.h>
#endif
#endif

#if BUILDFLAG(IS_WIN)
#include "content/public/browser/content_browser_client.h"
#include "content/public/common/content_client.h"
#endif  // BUILDFLAG(IS_WIN)

#if BUILDFLAG(USE_ZYGOTE)
#include "content/common/zygote/zygote_handle_impl_linux.h"
#include "sandbox/policy/sandbox_type.h"
#endif

#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_CHROMEOS)
#include "media/gpu/buildflags.h"
#include "media/media_buildflags.h"
#endif  // BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_CHROMEOS)

#if BUILDFLAG(IS_MAC)
#include "base/mac/process_requirement.h"
#endif  // BUILDFLAG(IS_MAC)

namespace content {

UtilitySandboxedProcessLauncherDelegate::
    UtilitySandboxedProcessLauncherDelegate(
        sandbox::mojom::Sandbox sandbox_type,
        const base::EnvironmentMap& env,
        const base::CommandLine& cmd_line)
    :
#if BUILDFLAG(IS_POSIX)
      env_(env),
#endif
      sandbox_type_(sandbox_type),
#if BUILDFLAG(IS_WIN)
      app_container_disabled_(
          GetContentClient()->browser()->IsAppContainerDisabled(sandbox_type)),
#endif
      cmd_line_(cmd_line) {
#if DCHECK_IS_ON()
  bool supported_sandbox_type =
      sandbox_type_ == sandbox::mojom::Sandbox::kNoSandbox ||
#if BUILDFLAG(IS_WIN)
      sandbox_type_ ==
          sandbox::mojom::Sandbox::kNoSandboxAndElevatedPrivileges ||
      sandbox_type_ == sandbox::mojom::Sandbox::kXrCompositing ||
      sandbox_type_ == sandbox::mojom::Sandbox::kPdfConversion ||
      sandbox_type_ == sandbox::mojom::Sandbox::kIconReader ||
      sandbox_type_ == sandbox::mojom::Sandbox::kMediaFoundationCdm ||
      sandbox_type_ == sandbox::mojom::Sandbox::kWebNNModelCompilation ||
#endif
#if BUILDFLAG(IS_MAC)
      sandbox_type_ == sandbox::mojom::Sandbox::kMirroring ||
#endif
#if BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC)
      sandbox_type_ == sandbox::mojom::Sandbox::kProxyResolver ||
#endif
      sandbox_type_ == sandbox::mojom::Sandbox::kUtility ||
      sandbox_type_ == sandbox::mojom::Sandbox::kService ||
      sandbox_type_ == sandbox::mojom::Sandbox::kServiceWithJit ||
      sandbox_type_ == sandbox::mojom::Sandbox::kNetwork ||
      sandbox_type_ == sandbox::mojom::Sandbox::kOnDeviceModelExecution ||
      sandbox_type_ == sandbox::mojom::Sandbox::kCdm ||
      sandbox_type_ == sandbox::mojom::Sandbox::kPrintCompositor ||
#if BUILDFLAG(IS_FUCHSIA)
      sandbox_type_ == sandbox::mojom::Sandbox::kVideoCapture ||
#endif
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_CHROMEOS)
      sandbox_type_ == sandbox::mojom::Sandbox::kShapeDetection ||
#if BUILDFLAG(ALLOW_OOP_VIDEO_DECODER)
      sandbox_type_ == sandbox::mojom::Sandbox::kHardwareVideoDecoding ||
#endif  // BUILDFLAG(ALLOW_OOP_VIDEO_DECODER)
#if BUILDFLAG(USE_LINUX_VIDEO_ACCELERATION)
      sandbox_type_ == sandbox::mojom::Sandbox::kHardwareVideoEncoding ||
#endif  // BUILDFLAG(USE_LINUX_VIDEO_ACCELERATION)
#endif  // BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_CHROMEOS)
#if BUILDFLAG(IS_CHROMEOS)
      sandbox_type_ == sandbox::mojom::Sandbox::kIme ||
      sandbox_type_ == sandbox::mojom::Sandbox::kTts ||
      sandbox_type_ == sandbox::mojom::Sandbox::kNearby ||
#endif  // BUILDFLAG(IS_CHROMEOS)
#if BUILDFLAG(IS_CHROMEOS) || BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_MAC) || \
    BUILDFLAG(IS_WIN)
      sandbox_type_ == sandbox::mojom::Sandbox::kScreenAI ||
      sandbox_type_ == sandbox::mojom::Sandbox::kPrintBackend ||
#endif
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_MAC) || BUILDFLAG(IS_CHROMEOS)
      sandbox_type_ == sandbox::mojom::Sandbox::kOnDeviceTranslation ||
#endif
      sandbox_type_ == sandbox::mojom::Sandbox::kAudio ||
      sandbox_type_ == sandbox::mojom::Sandbox::kSpeechRecognition;
  DCHECK(supported_sandbox_type);
#endif  // DCHECK_IS_ON()
}

UtilitySandboxedProcessLauncherDelegate::
    ~UtilitySandboxedProcessLauncherDelegate() = default;

sandbox::mojom::Sandbox
UtilitySandboxedProcessLauncherDelegate::GetSandboxType() {
  return sandbox_type_;
}

#if BUILDFLAG(ENABLE_XENON_SERVICE)
bool UtilitySandboxedProcessLauncherDelegate::SetXenonUtilityProcessOptions(
    std::unique_ptr<xenon::UtilityProcessLaunchOptions> options) {
  std::string error;
  if (!options || !options->Validate(&error) ||
      sandbox_type_ != sandbox::mojom::Sandbox::kNoSandbox ||
      cmd_line_.GetSwitchValueASCII(switches::kProcessType) !=
          switches::kUtilityProcess ||
      !cmd_line_.HasSwitch(xenon::ipc::switches::kUtilityProcess) ||
      cmd_line_.GetSwitchValueASCII(switches::kUtilitySubType) !=
          "xenon.ipc.mojom.UtilityProcessService") {
    LOG(ERROR) << "Invalid dedicated utilityProcess launch options: " << error;
    return false;
  }
  xenon_utility_options_ = std::move(options);
  return true;
}

bool UtilitySandboxedProcessLauncherDelegate::ApplyXenonUtilityProcessOptions(
    base::LaunchOptions* options) {
  if (!xenon_utility_options_) {
    return true;
  }
  CHECK_EQ(sandbox_type_, sandbox::mojom::Sandbox::kNoSandbox);
  options->environment = xenon_utility_options_->environment;
  options->clear_environment = true;
  options->preserve_empty_environment_values = true;
  options->current_directory = xenon_utility_options_->working_directory;
#if BUILDFLAG(IS_WIN)
  options->start_hidden = true;
  for (const auto& file : xenon_utility_options_->stdio) {
    HANDLE handle = file.GetPlatformFile();
    if (!::SetHandleInformation(handle, HANDLE_FLAG_INHERIT,
                                HANDLE_FLAG_INHERIT)) {
      return false;
    }
    options->handles_to_inherit.push_back(handle);
  }
  options->stdin_handle = xenon_utility_options_->stdio[0].GetPlatformFile();
  options->stdout_handle = xenon_utility_options_->stdio[1].GetPlatformFile();
  options->stderr_handle = xenon_utility_options_->stdio[2].GetPlatformFile();
#elif BUILDFLAG(IS_POSIX)
  for (size_t index = 0; index < xenon_utility_options_->stdio.size();
       ++index) {
    options->fds_to_remap.emplace_back(
        xenon_utility_options_->stdio[index].GetPlatformFile(),
        static_cast<int>(index));
  }
#else
  return false;
#endif
  return true;
}
#endif

#if BUILDFLAG(IS_POSIX)
base::EnvironmentMap UtilitySandboxedProcessLauncherDelegate::GetEnvironment() {
  return env_;
}
#endif  // BUILDFLAG(IS_POSIX)

#if BUILDFLAG(USE_ZYGOTE)
ZygoteCommunication* UtilitySandboxedProcessLauncherDelegate::GetZygote() {
  if (zygote_.has_value()) {
    return zygote_.value();
  }

  // If the sandbox has been disabled for a given type, don't use a zygote.
  if (sandbox::policy::IsUnsandboxedSandboxType(sandbox_type_)) {
    return nullptr;
  }

  // TODO(crbug.com/40261714): remove this special case and fork from the
  // zygote. For now, browser tests fail when forking the network service from
  // the unsandboxed zygote, as the forked process only creates the
  // NetworkServiceTestHelper if the process is exec'd.
  if (sandbox_type_ == sandbox::mojom::Sandbox::kNetwork) {
    return nullptr;
  }

  // Utility processes which need specialized sandboxes fork from the
  // unsandboxed zygote and then apply their actual sandboxes in the forked
  // process upon startup.
  if (sandbox_type_ == sandbox::mojom::Sandbox::kNetwork ||
      sandbox_type_ == sandbox::mojom::Sandbox::kOnDeviceModelExecution ||
#if BUILDFLAG(USE_LINUX_VIDEO_ACCELERATION)
      sandbox_type_ == sandbox::mojom::Sandbox::kHardwareVideoDecoding ||
      sandbox_type_ == sandbox::mojom::Sandbox::kHardwareVideoEncoding ||
#endif
#if BUILDFLAG(IS_CHROMEOS)
      sandbox_type_ == sandbox::mojom::Sandbox::kIme ||
      sandbox_type_ == sandbox::mojom::Sandbox::kTts ||
      sandbox_type_ == sandbox::mojom::Sandbox::kNearby ||
#endif  // BUILDFLAG(IS_CHROMEOS)
      sandbox_type_ == sandbox::mojom::Sandbox::kAudio ||
#if BUILDFLAG(IS_CHROMEOS) || BUILDFLAG(IS_LINUX)
      sandbox_type_ == sandbox::mojom::Sandbox::kShapeDetection ||
#endif
#if BUILDFLAG(IS_CHROMEOS) || BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_MAC) || \
    BUILDFLAG(IS_WIN)
      sandbox_type_ == sandbox::mojom::Sandbox::kPrintBackend ||
      sandbox_type_ == sandbox::mojom::Sandbox::kScreenAI ||
#endif
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_CHROMEOS)
      sandbox_type_ == sandbox::mojom::Sandbox::kOnDeviceTranslation ||
#endif  // BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_CHROMEOS)
      sandbox_type_ == sandbox::mojom::Sandbox::kSpeechRecognition) {
    return GetUnsandboxedZygote();
  }

  // All other types use the pre-sandboxed zygote.
  return GetGenericZygote();
}

void UtilitySandboxedProcessLauncherDelegate::SetZygote(
    ZygoteCommunication* handle) {
  zygote_ = handle;
}
#endif  // BUILDFLAG(USE_ZYGOTE)

#if BUILDFLAG(IS_MAC)
std::optional<base::mac::ProcessRequirement>
UtilitySandboxedProcessLauncherDelegate::GetProcessRequirement() {
  if (sandbox_type_ == sandbox::mojom::Sandbox::kNetwork &&
      base::FeatureList::IsEnabled(
          features::kValidateNetworkServiceProcessIdentity)) {
    return base::mac::ProcessRequirement::Builder()
        .SignedWithSameIdentity()
        .Build();
  }

  return std::nullopt;
}
#endif  // BUILDFLAG(IS_MAC)

}  // namespace content
