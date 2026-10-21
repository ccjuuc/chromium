// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/updater/app/app_patch_worker.h"

#include <memory>
#include <utility>

#include "base/command_line.h"
#include "base/process/process.h"
#include "build/build_config.h"

#if BUILDFLAG(IS_MAC)
#include "base/process/port_provider_mac.h"
#endif
#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/task/bind_post_task.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/task_traits.h"
#include "base/task/thread_pool.h"
#include "chrome/updater/app/app.h"
#include "chrome/updater/constants.h"
#include "components/services/patch/file_patcher_impl.h"
#include "components/services/patch/public/mojom/file_patcher.mojom.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "mojo/public/cpp/platform/platform_channel.h"
#include "mojo/public/cpp/system/invitation.h"
#include "mojo/public/cpp/system/message_pipe.h"

namespace updater {

namespace {

class AppPatchWorker : public App {
 public:
  AppPatchWorker();

 private:
  ~AppPatchWorker() override;
  int Initialize() override;
  void FirstTaskRun() override;
};

class FilePatcherImplWithShutdown : public patch::FilePatcherImpl {
 public:
  explicit FilePatcherImplWithShutdown(base::OnceClosure shutdown_callback)
      : shutdown_callback_(std::move(shutdown_callback)) {}

  ~FilePatcherImplWithShutdown() override {
    std::move(shutdown_callback_).Run();
  }

 private:
  base::OnceClosure shutdown_callback_;
};

}  // namespace

AppPatchWorker::AppPatchWorker() = default;

AppPatchWorker::~AppPatchWorker() = default;

int AppPatchWorker::Initialize() {
#if BUILDFLAG(IS_MAC)
  if (base::CommandLine::ForCurrentProcess()->HasSwitch(
          ::updater::kPatchWorkerBackgroundPrioritySwitch)) {
    base::SelfPortProvider self_port_provider;
    base::Process::Current().SetPriority(&self_port_provider,
                                         base::Process::Priority::kBestEffort);
  }
#endif
  return kErrorOk;
}

void AppPatchWorker::FirstTaskRun() {
  // This process must be started with the command line switch
  // `--mojo-platform-channel-handle=N`. In other words, the command line
  // must be prepared by
  // `mojo::PlatformChannel::PrepareToPassRemoteEndpoint()`.
  mojo::PlatformChannelEndpoint endpoint =
      mojo::PlatformChannel::RecoverPassedEndpointFromCommandLine(
          *base::CommandLine::ForCurrentProcess());
  if (!endpoint.is_valid()) {
    Shutdown(kErrorMojoConnectionFailure);
    return;
  }

  mojo::ScopedMessagePipeHandle pipe =
      mojo::IncomingInvitation::AcceptIsolated(std::move(endpoint));
  if (!pipe->is_valid()) {
    Shutdown(kErrorMojoConnectionFailure);
    return;
  }

  base::ThreadPool::CreateSequencedTaskRunner({base::MayBlock()})
      ->PostTask(
          FROM_HERE,
          base::BindOnce(
              [](mojo::PendingReceiver<patch::mojom::FilePatcher> receiver,
                 base::OnceClosure shutdown_callback) {
                mojo::MakeSelfOwnedReceiver(
                    std::make_unique<FilePatcherImplWithShutdown>(
                        std::move(shutdown_callback)),
                    std::move(receiver));
              },
              mojo::PendingReceiver<patch::mojom::FilePatcher>(std::move(pipe)),
              base::BindPostTaskToCurrentDefault(
                  base::BindOnce(&AppPatchWorker::Shutdown, this, kErrorOk))));
}

scoped_refptr<App> MakeAppPatchWorker() {
  return base::MakeRefCounted<AppPatchWorker>();
}

}  // namespace updater
