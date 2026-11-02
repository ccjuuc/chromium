// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/gm_key/test/key_service_test_client.h"

#include "base/command_line.h"
#include "base/process/launch.h"
#include "base/time/time.h"
#include "mojo/public/cpp/platform/platform_channel.h"
#include "mojo/public/cpp/system/invitation.h"

namespace gm_key::test {
ServiceProcess::ServiceProcess() = default;
ServiceProcess::~ServiceProcess() {
  service_.reset();
  control_.reset();
  if (process_.IsValid()) {
    int exit_code = 0;
    if (!process_.WaitForExitWithTimeout(base::Seconds(5), &exit_code)) {
      process_.Terminate(74, true);
    }
  }
}
bool ServiceProcess::Launch(const base::FilePath& executable) {
  if (process_.IsValid()) {
    return false;
  }
  base::CommandLine command(executable);
  base::LaunchOptions options;
  options.start_hidden = true;
  mojo::PlatformChannel channel;
  channel.PrepareToPassRemoteEndpoint(&options, &command);
  mojo::OutgoingInvitation invitation;
  auto service_pipe = invitation.AttachMessagePipe(0);
  auto control_pipe = invitation.AttachMessagePipe(1);
  process_ = base::LaunchProcess(command, options);
  channel.RemoteProcessLaunchAttempted();
  if (!process_.IsValid()) {
    return false;
  }
  mojo::OutgoingInvitation::Send(std::move(invitation), process_.Handle(),
                                 channel.TakeLocalEndpoint());
  service_.Bind(mojo::PendingRemote<gm_key::mojom::KeyService>(
      std::move(service_pipe), 0));
  control_.Bind(mojo::PendingRemote<gm_key::test::mojom::KeyTestControl>(
      std::move(control_pipe), 0));
  return true;
}
}  // namespace gm_key::test
