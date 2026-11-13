// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_crash_reporter_bridge.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "components/crash/core/app/crashpad.h"

namespace xenon::ipc {
namespace {

mojom::IpcResultPtr ReadHostCrashState(std::string operation) {
  auto result = mojom::IpcResult::New();
  result->success = true;
  if (operation == "crashReporter.getUploadToServer") {
    result->value = base::Value(crash_reporter::IsCrashUploadEnabled());
  } else {
    std::vector<crash_reporter::Report> reports;
    crash_reporter::GetReports(&reports);
    result->value = base::Value(UploadedCrashReportsToValue(reports));
  }
  return result;
}

}  // namespace

base::ListValue UploadedCrashReportsToValue(
    base::span<const crash_reporter::Report> reports) {
  std::vector<const crash_reporter::Report*> uploaded;
  for (const auto& report : reports) {
    if (report.state == crash_reporter::ReportUploadState::Uploaded) {
      uploaded.push_back(&report);
    }
  }
  std::stable_sort(uploaded.begin(), uploaded.end(),
                   [](const auto* a, const auto* b) {
                     return a->capture_time > b->capture_time;
                   });
  base::ListValue result;
  for (const auto* report : uploaded) {
    result.Append(base::DictValue()
                      .Set("id", report->remote_id)
                      .Set("date", base::Time::FromTimeT(report->capture_time)
                                       .InMillisecondsFSinceUnixEpoch()));
  }
  return result;
}

void CallCrashReporterApi(const std::string& operation,
                          ElectronApiCallback callback) {
  if (operation != "crashReporter.getUploadToServer" &&
      operation != "crashReporter.getUploadedReports") {
    auto result = mojom::IpcResult::New();
    result->success = false;
    result->error =
        "ERR_NOT_SUPPORTED: crashReporter configuration is host-managed";
    std::move(callback).Run(std::move(result));
    return;
  }
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&ReadHostCrashState, operation), std::move(callback));
}

}  // namespace xenon::ipc
