// Copyright 2026 The Xenon Authors. All rights reserved.

#include "xenon_overlay/chrome/browser/ui/webui/xenon_transfer_webui_handler.h"

#include <algorithm>
#include <utility>

#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/strings/string_util.h"
#include "base/task/thread_pool.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/select_file_policy/chrome_select_file_policy.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "ui/shell_dialogs/selected_file_info.h"
#include "xenon_overlay/chrome/browser/account/xenon_account_service.h"
#include "xenon_overlay/chrome/browser/account/xenon_transfer_service.h"

namespace xenon {
namespace {

bool ValidId(const std::string* value) {
  return value && value->size() == 32 &&
         std::ranges::all_of(*value, [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

}  // namespace

XenonTransferWebUIHandler::XenonTransferWebUIHandler() = default;
XenonTransferWebUIHandler::~XenonTransferWebUIHandler() {
  if (file_dialog_) {
    file_dialog_->ListenerDestroyed();
  }
}

void XenonTransferWebUIHandler::RegisterMessages() {
  web_ui()->RegisterMessageCallback(
      "ownTransferGetState",
      base::BindRepeating(&XenonTransferWebUIHandler::GetState,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "ownTransferRefresh",
      base::BindRepeating(&XenonTransferWebUIHandler::Refresh,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "ownTransferSetUseStun",
      base::BindRepeating(&XenonTransferWebUIHandler::SetUseStun,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "ownTransferChooseFile",
      base::BindRepeating(&XenonTransferWebUIHandler::ChooseFile,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "ownTransferSendText",
      base::BindRepeating(&XenonTransferWebUIHandler::SendText,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "ownTransferSendFile",
      base::BindRepeating(&XenonTransferWebUIHandler::SendFile,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "ownTransferRespond",
      base::BindRepeating(&XenonTransferWebUIHandler::Respond,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "ownTransferCancel",
      base::BindRepeating(&XenonTransferWebUIHandler::Cancel,
                          base::Unretained(this)));
}

void XenonTransferWebUIHandler::OnJavascriptDisallowed() {
  weak_factory_.InvalidateWeakPtrs();
  if (file_dialog_) {
    file_dialog_->ListenerDestroyed();
  }
  file_dialog_ = nullptr;
  file_callback_id_.clear();
  picker_account_id_.clear();
  selected_path_.clear();
  selected_account_id_.clear();
}

bool XenonTransferWebUIHandler::ValidateCallback(const base::ListValue& args,
                                                 size_t count) {
  if (args.empty() || !args[0].is_string() ||
      args[0].GetString().size() > 256) {
    return false;
  }
  AllowJavascript();
  if (args.size() != count) {
    RejectJavascriptCallback(args[0], base::Value("传输请求参数无效。"));
    return false;
  }
  return true;
}

void XenonTransferWebUIHandler::GetState(const base::ListValue& args) {
  if (!ValidateCallback(args, 1)) {
    return;
  }
  ResolveJavascriptCallback(
      args[0], XenonTransferService::GetForProfile(Profile::FromWebUI(web_ui()))
                   ->GetState());
}

void XenonTransferWebUIHandler::Refresh(const base::ListValue& args) {
  if (!ValidateCallback(args, 1)) {
    return;
  }
  XenonTransferService::GetForProfile(Profile::FromWebUI(web_ui()))
      ->Refresh(base::BindOnce(&XenonTransferWebUIHandler::Reply,
                               weak_factory_.GetWeakPtr(), args[0].GetString()),
                /*retry_signals=*/true);
}

void XenonTransferWebUIHandler::SetUseStun(const base::ListValue& args) {
  if (!ValidateCallback(args, 2)) {
    return;
  }
  const auto* input = args[1].GetIfDict();
  const std::optional<bool> enabled =
      input ? input->FindBool("enabled") : std::nullopt;
  if (!input || input->size() != 1 || !enabled.has_value()) {
    RejectJavascriptCallback(args[0], base::Value("无效的 STUN 设置。"));
    return;
  }
  XenonTransferService::GetForProfile(Profile::FromWebUI(web_ui()))
      ->SetUseStun(*enabled,
                   base::BindOnce(&XenonTransferWebUIHandler::Reply,
                                  weak_factory_.GetWeakPtr(),
                                  args[0].GetString()));
}

void XenonTransferWebUIHandler::ChooseFile(const base::ListValue& args) {
  if (!ValidateCallback(args, 1)) {
    return;
  }
  auto* account =
      XenonAccountService::GetForProfile(Profile::FromWebUI(web_ui()));
  if (account->GetDeviceId().empty() || !file_callback_id_.empty()) {
    RejectJavascriptCallback(args[0],
                             base::Value("请先登录，或完成当前文件选择。"));
    return;
  }
  picker_account_id_ = account->GetAccountId();
  file_callback_id_ = args[0].GetString();
  file_dialog_ = ui::SelectFileDialog::Create(
      this,
      std::make_unique<ChromeSelectFilePolicy>(web_ui()->GetWebContents()));
  file_dialog_->SelectFile(
      ui::SelectFileDialog::SELECT_OPEN_FILE, u"选择要发送的文件",
      base::FilePath(), nullptr, 0, base::FilePath::StringType(),
      web_ui()->GetWebContents()->GetTopLevelNativeWindow(), nullptr);
}

void XenonTransferWebUIHandler::FileSelected(const ui::SelectedFileInfo& file,
                                             int index) {
  file_dialog_ = nullptr;
  const auto path = file.path();
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(
          [](base::FilePath path) -> std::optional<int64_t> {
            base::File::Info info;
            if (!base::GetFileInfo(path, &info) || info.is_directory ||
                info.size < 0 || info.size > 8LL * 1024 * 1024 * 1024) {
              return std::nullopt;
            }
            return info.size;
          },
          path),
      base::BindOnce(&XenonTransferWebUIHandler::OnFileInspected,
                     weak_factory_.GetWeakPtr(), file_callback_id_,
                     picker_account_id_, path));
}

void XenonTransferWebUIHandler::OnFileInspected(std::string callback_id,
                                                std::string account_id,
                                                base::FilePath path,
                                                std::optional<int64_t> size) {
  file_callback_id_.clear();
  const auto current_account =
      XenonAccountService::GetForProfile(Profile::FromWebUI(web_ui()))
          ->GetAccountId();
  if (!size || account_id.empty() || account_id != current_account) {
    selected_path_.clear();
    RejectJavascriptCallback(
        base::Value(callback_id),
        base::Value("无法读取该文件，或文件超过 8 GiB，请重新选择。"));
    return;
  }
  selected_path_ = std::move(path);
  selected_account_id_ = std::move(account_id);
  Reply(std::move(callback_id),
        base::DictValue()
            .Set("name", selected_path_.BaseName().AsUTF8Unsafe())
            .Set("size", static_cast<double>(*size)));
}

void XenonTransferWebUIHandler::FileSelectionCanceled() {
  file_dialog_ = nullptr;
  const auto callback_id = std::exchange(file_callback_id_, {});
  if (!callback_id.empty()) {
    Reply(callback_id, base::DictValue().Set("cancelled", true));
  }
}

void XenonTransferWebUIHandler::SendText(const base::ListValue& args) {
  if (!ValidateCallback(args, 2)) {
    return;
  }
  const auto* input = args[1].GetIfDict();
  const auto* device = input ? input->FindString("deviceId") : nullptr;
  const auto* text = input ? input->FindString("text") : nullptr;
  if (!ValidId(device) || !text || text->empty() || text->size() > 16 * 1024 ||
      text->find('\0') != std::string::npos || !base::IsStringUTF8(*text)) {
    RejectJavascriptCallback(
        args[0], base::Value("请选择设备并输入不超过 16 KiB 的文本。"));
    return;
  }
  XenonTransferService::GetForProfile(Profile::FromWebUI(web_ui()))
      ->SendText(
          *device, *text,
          base::BindOnce(&XenonTransferWebUIHandler::Reply,
                         weak_factory_.GetWeakPtr(), args[0].GetString()));
}

void XenonTransferWebUIHandler::SendFile(const base::ListValue& args) {
  if (!ValidateCallback(args, 2)) {
    return;
  }
  const auto* input = args[1].GetIfDict();
  const auto* device = input ? input->FindString("deviceId") : nullptr;
  auto* profile = Profile::FromWebUI(web_ui());
  if (!ValidId(device) || selected_path_.empty() ||
      selected_account_id_ !=
          XenonAccountService::GetForProfile(profile)->GetAccountId()) {
    RejectJavascriptCallback(args[0],
                             base::Value("请先选择设备和要发送的文件。"));
    return;
  }
  XenonTransferService::GetForProfile(profile)->SendFile(
      *device, selected_path_,
      base::BindOnce(&XenonTransferWebUIHandler::Reply,
                     weak_factory_.GetWeakPtr(), args[0].GetString()));
}

void XenonTransferWebUIHandler::Respond(const base::ListValue& args) {
  if (!ValidateCallback(args, 2)) {
    return;
  }
  const auto* input = args[1].GetIfDict();
  const auto* id = input ? input->FindString("id") : nullptr;
  const auto* action = input ? input->FindString("action") : nullptr;
  if (!ValidId(id) || !action ||
      (*action != "accept" && *action != "reject" && *action != "cancel")) {
    RejectJavascriptCallback(args[0], base::Value("无效的接收操作。"));
    return;
  }
  XenonTransferService::GetForProfile(Profile::FromWebUI(web_ui()))
      ->Respond(
          *id, *action,
          base::BindOnce(&XenonTransferWebUIHandler::Reply,
                         weak_factory_.GetWeakPtr(), args[0].GetString()));
}

void XenonTransferWebUIHandler::Cancel(const base::ListValue& args) {
  if (!ValidateCallback(args, 1)) {
    return;
  }
  XenonTransferService::GetForProfile(Profile::FromWebUI(web_ui()))
      ->Cancel(base::BindOnce(&XenonTransferWebUIHandler::Reply,
                              weak_factory_.GetWeakPtr(), args[0].GetString()));
}

void XenonTransferWebUIHandler::Reply(std::string callback_id,
                                      base::DictValue state) {
  if (IsJavascriptAllowed()) {
    ResolveJavascriptCallback(base::Value(callback_id), state);
  }
}

}  // namespace xenon
