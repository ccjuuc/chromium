// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/xenon_ai/xenon_ai_service.h"

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/memory/scoped_refptr.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/browser_thread.h"
#include "xenon_overlay/chrome/browser/xenon_ai/xenon_ai_inference_client.h"

namespace xenon {

namespace {

std::string XenonBuildStamp() {
  return "xenon-ai-1.0-alpha";
}

}  // namespace

XenonAiService::XenonAiService(Profile* profile)
    : profile_(profile),
      build_stamp_(XenonBuildStamp()) {}

XenonAiService::~XenonAiService() = default;

void XenonAiService::AddObserver(Observer* observer) {
  observers_.AddObserver(observer);
}

void XenonAiService::RemoveObserver(Observer* observer) {
  observers_.RemoveObserver(observer);
}

std::string XenonAiService::GetBuildStamp() const {
  return build_stamp_;
}

void XenonAiService::SetPendingPrompt(const std::string& prompt) {
  pending_prompt_ = prompt;
  for (auto& observer : observers_) {
    observer.OnPromptReceived(prompt);
  }
}

std::string XenonAiService::GetPendingPrompt() const {
  return pending_prompt_;
}

void XenonAiService::RunInferencerRewriteAsync(
    const std::string& instruction_utf8,
    const std::string& selection_utf8,
    base::OnceCallback<void(bool success, const std::string& text)> callback) {
  scoped_refptr<base::SequencedTaskRunner> ui_runner =
      content::GetUIThreadTaskRunner({});
  VLOG(1) << "[xenon_ai] RunInferencerRewriteAsync selection_len="
           << selection_utf8.size();

  const bool posted = base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(
          [](const std::string& instruction,
             const std::string& selection) -> std::pair<bool, std::string> {
            std::string out;
            std::string err;
            if (!XenonAiInferenceClient::RunRewrite(instruction, selection, &out,
                                                    &err)) {
              VLOG(1) << "[xenon_ai] worker RunRewrite failed err_len="
                      << err.size();
              return {false, std::move(err)};
            }
            VLOG(1) << "[xenon_ai] worker RunRewrite ok out_len=" << out.size();
            return {true, std::move(out)};
          },
          instruction_utf8, selection_utf8),
      base::BindOnce(
          [](scoped_refptr<base::SequencedTaskRunner> ui_runner,
             base::OnceCallback<void(bool, const std::string&)> user_cb,
             std::pair<bool, std::string> result) {
            ui_runner->PostTask(
                FROM_HERE,
                base::BindOnce(
                    [](base::OnceCallback<void(bool, const std::string&)> cb,
                       std::pair<bool, std::string> res) {
                      VLOG(1) << "[xenon_ai] UI callback success=" << res.first
                              << " text_len=" << res.second.size();
                      std::move(cb).Run(res.first, res.second);
                    },
                    std::move(user_cb), std::move(result)));
          },
          std::move(ui_runner), std::move(callback)));

  if (!posted) {
    LOG(WARNING)
        << "[xenon_ai] PostTaskAndReplyWithResult failed (browser shutting "
           "down?); invoking callback with error.";
    ui_runner->PostTask(
        FROM_HERE,
        base::BindOnce(
            [](base::OnceCallback<void(bool, const std::string&)> cb) {
              std::move(cb).Run(
                  false, "inferencer task was not scheduled (ThreadPool)");
            },
            std::move(callback)));
  }
}

}  // namespace xenon
