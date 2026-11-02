// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ui/webui/cross_transfer_test_ui.h"

#include <memory>
#include <string>
#include <utility>

#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/string_util.h"
#include "base/task/task_traits.h"
#include "base/task/thread_pool.h"
#include "base/values.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_data_source.h"
#include "content/public/browser/web_ui_message_handler.h"
#include "content/public/common/url_constants.h"
#include "cross_transfer/test_job.h"
#include "xenon_overlay/resources/grit/xenon_resources.h"

namespace xenon {
namespace {

constexpr char kHost[] = "cross-transfer-test";

base::DictValue ToValue(const cross_transfer::TestJobSnapshot& snapshot) {
  return base::DictValue()
      .Set("active", snapshot.active)
      .Set("state", snapshot.state)
      .Set("status", snapshot.status)
      .Set("error", snapshot.error)
      .Set("taskId", snapshot.task_id)
      .Set("savedPath", snapshot.saved_path)
      .Set("transferredBytes", static_cast<double>(snapshot.bytes))
      .Set("totalBytes", static_cast<double>(snapshot.total))
      .Set("percent", snapshot.percent)
      .Set("listeningPort", static_cast<int>(snapshot.listening_port));
}

bool ReadString(const base::DictValue& input,
                const char* key,
                std::string* output) {
  const base::Value* value = input.Find(key);
  if (!value) {
    return true;
  }
  if (!value->is_string() || value->GetString().size() > 4096 ||
      value->GetString().find('\0') != std::string::npos ||
      !base::IsStringUTF8(value->GetString())) {
    return false;
  }
  *output = value->GetString();
  return true;
}

bool IsAbsolutePathOrEmpty(const std::string& value) {
  return value.empty() || base::FilePath::FromUTF8Unsafe(value).IsAbsolute();
}

bool ParseOptions(const base::DictValue& input,
                  cross_transfer::TestJobOptions* options) {
  if (!ReadString(input, "role", &options->role) ||
      !ReadString(input, "transport", &options->transport) ||
      !ReadString(input, "sourcePath", &options->source_path) ||
      !ReadString(input, "outputDir", &options->output_dir) ||
      !ReadString(input, "host", &options->host) ||
      !ReadString(input, "caFile", &options->ca_file) ||
      !ReadString(input, "certificateFile", &options->certificate_file) ||
      !ReadString(input, "privateKeyFile", &options->private_key_file) ||
      !ReadString(input, "peerName", &options->peer_name) ||
      !ReadString(input, "roomId", &options->room_id) ||
      !ReadString(input, "connectionId", &options->connection_id) ||
      !ReadString(input, "bearerToken", &options->bearer_token) ||
      !ReadString(input, "taskId", &options->task_id)) {
    return false;
  }
  const auto port = input.FindInt("port");
  if (!port || *port < 0 || *port > 65535 ||
      (options->role != "send" && options->role != "receive") ||
      (options->transport != "direct" && options->transport != "relay")) {
    return false;
  }
  options->port = static_cast<uint16_t>(*port);
  if (const base::Value* overwrite = input.Find("allowOverwrite")) {
    if (!overwrite->is_bool()) {
      return false;
    }
    options->allow_overwrite = overwrite->GetBool();
  }
  return IsAbsolutePathOrEmpty(options->source_path) &&
         IsAbsolutePathOrEmpty(options->output_dir) &&
         IsAbsolutePathOrEmpty(options->ca_file) &&
         IsAbsolutePathOrEmpty(options->certificate_file) &&
         IsAbsolutePathOrEmpty(options->private_key_file);
}

class CrossTransferTestHandler final : public content::WebUIMessageHandler {
 public:
  CrossTransferTestHandler() = default;
  ~CrossTransferTestHandler() override {
    if (job_) {
      job_->Cancel();
    }
  }

  void RegisterMessages() override {
    web_ui()->RegisterMessageCallback(
        "crossTransferGetStatus",
        base::BindRepeating(&CrossTransferTestHandler::GetStatus,
                            base::Unretained(this)));
    web_ui()->RegisterMessageCallback(
        "crossTransferStart",
        base::BindRepeating(&CrossTransferTestHandler::Start,
                            base::Unretained(this)));
    web_ui()->RegisterMessageCallback(
        "crossTransferCancel",
        base::BindRepeating(&CrossTransferTestHandler::Cancel,
                            base::Unretained(this)));
  }

  void OnJavascriptDisallowed() override {
    if (job_) {
      job_->Cancel();
    }
  }

 private:
  bool ValidCallback(const base::ListValue& args) {
    if (args.empty() || !args[0].is_string()) {
      return false;
    }
    AllowJavascript();
    return true;
  }

  cross_transfer::TestJobSnapshot Snapshot() const {
    auto snapshot = job_ ? job_->snapshot() : cross_transfer::TestJobSnapshot();
    if (running_ && snapshot.state == "idle") {
      snapshot.active = true;
      snapshot.state = "starting";
      snapshot.status = "Waiting for the transfer worker";
    }
    return snapshot;
  }

  void GetStatus(const base::ListValue& args) {
    if (!ValidCallback(args)) {
      return;
    }
    ResolveJavascriptCallback(args[0], ToValue(Snapshot()));
  }

  void Start(const base::ListValue& args) {
    if (!ValidCallback(args)) {
      return;
    }
    if (running_) {
      RejectJavascriptCallback(args[0],
                               base::Value("请等待当前任务结束或取消完成。"));
      return;
    }
    cross_transfer::TestJobOptions options;
    if (args.size() != 2 || !args[1].is_dict() ||
        !ParseOptions(args[1].GetDict(), &options)) {
      RejectJavascriptCallback(
          args[0], base::Value("参数无效；请检查端口并使用本机绝对路径。"));
      return;
    }
    job_ =
        std::make_shared<cross_transfer::TestTransferJob>(std::move(options));
    running_ = true;
    // The worker owns the job independently of this page. It only publishes a
    // mutex-protected snapshot; no WebUI or WebContents is accessed
    // off-sequence.
    if (!base::ThreadPool::PostTaskAndReply(
            FROM_HERE,
            {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
             base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
            base::BindOnce(
                [](std::shared_ptr<cross_transfer::TestTransferJob> job) {
                  job->Run();
                },
                job_),
            base::BindOnce(&CrossTransferTestHandler::OnComplete,
                           weak_factory_.GetWeakPtr()))) {
      job_->Cancel();
      running_ = false;
      RejectJavascriptCallback(args[0], base::Value("无法启动传输工作线程。"));
      return;
    }
    ResolveJavascriptCallback(args[0], ToValue(Snapshot()));
  }

  void Cancel(const base::ListValue& args) {
    if (!ValidCallback(args)) {
      return;
    }
    if (job_) {
      job_->Cancel();
    }
    ResolveJavascriptCallback(args[0], ToValue(Snapshot()));
  }

  void OnComplete() { running_ = false; }

  std::shared_ptr<cross_transfer::TestTransferJob> job_;
  bool running_{false};
  base::WeakPtrFactory<CrossTransferTestHandler> weak_factory_{this};
};

}  // namespace

CrossTransferTestUI::CrossTransferTestUI(content::WebUI* web_ui)
    : content::WebUIController(web_ui) {
  auto* source = content::WebUIDataSource::CreateAndAdd(
      web_ui->GetWebContents()->GetBrowserContext(), kHost);
  source->SetDefaultResource(IDR_CROSS_TRANSFER_TEST_HTML);
  source->AddResourcePath("index.css", IDR_CROSS_TRANSFER_TEST_CSS);
  source->AddResourcePath("index.js", IDR_CROSS_TRANSFER_TEST_JS);
  web_ui->AddMessageHandler(std::make_unique<CrossTransferTestHandler>());
}

CrossTransferTestUI::~CrossTransferTestUI() = default;
CrossTransferTestUIConfig::CrossTransferTestUIConfig()
    : content::WebUIConfig(content::kChromeUIScheme, kHost) {}
CrossTransferTestUIConfig::~CrossTransferTestUIConfig() = default;
std::unique_ptr<content::WebUIController>
CrossTransferTestUIConfig::CreateWebUIController(content::WebUI* web_ui,
                                                 const GURL& url) {
  return std::make_unique<CrossTransferTestUI>(web_ui);
}

}  // namespace xenon
