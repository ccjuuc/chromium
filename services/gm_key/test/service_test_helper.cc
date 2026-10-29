// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/at_exit.h"
#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/memory/raw_ref.h"
#include "base/run_loop.h"
#include "base/task/single_thread_task_executor.h"
#include "base/threading/thread.h"
#include "mojo/core/embedder/embedder.h"
#include "mojo/core/embedder/scoped_ipc_support.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/platform/platform_channel.h"
#include "mojo/public/cpp/system/invitation.h"
#include "services/gm_key/gm_key_service.h"
#include "services/gm_key/test/gm_key_test_control.test-mojom.h"

namespace {
class TestControl : public gm_key::test::mojom::KeyTestControl {
 public:
  TestControl(
      gm_key::GmKeyService& service,
      mojo::PendingReceiver<gm_key::test::mojom::KeyTestControl> receiver,
      base::OnceClosure disconnected)
      : service_(service), receiver_(this, std::move(receiver)) {
    receiver_.set_disconnect_handler(std::move(disconnected));
  }
  void Control(uint32_t operation,
               uint32_t value,
               ControlCallback callback) override {
    service_->ControlForTesting(operation, value, std::move(callback));
  }

 private:
  const raw_ref<gm_key::GmKeyService> service_;
  mojo::Receiver<gm_key::test::mojom::KeyTestControl> receiver_;
};
}  // namespace

int main(int argc, char** argv) {
  base::AtExitManager at_exit;
  base::CommandLine::Init(argc, argv);
  base::SingleThreadTaskExecutor main_executor(base::MessagePumpType::IO);
  mojo::core::Init();
  base::Thread ipc_thread("GmKeyTestIpc");
  base::Thread::Options options(base::MessagePumpType::IO, 0);
  CHECK(ipc_thread.StartWithOptions(std::move(options)));
  mojo::core::ScopedIPCSupport ipc(
      ipc_thread.task_runner(),
      mojo::core::ScopedIPCSupport::ShutdownPolicy::CLEAN);
  auto invitation = mojo::IncomingInvitation::Accept(
      mojo::PlatformChannel::RecoverPassedEndpointFromCommandLine(
          *base::CommandLine::ForCurrentProcess()));
  CHECK(invitation.is_valid());
  base::RunLoop loop;
  auto service = gm_key::GmKeyService::CreateForTesting(
      mojo::PendingReceiver<gm_key::mojom::KeyService>(
          invitation.ExtractMessagePipe(0)),
      base::Seconds(3));
  TestControl control(
      *service,
      mojo::PendingReceiver<gm_key::test::mojom::KeyTestControl>(
          invitation.ExtractMessagePipe(1)),
      loop.QuitClosure());
  loop.Run();
  return 0;
}
