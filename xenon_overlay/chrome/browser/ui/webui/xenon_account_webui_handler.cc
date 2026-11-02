// Copyright 2026 The Xenon Authors. All rights reserved.

#include "xenon_overlay/chrome/browser/ui/webui/xenon_account_webui_handler.h"

#include <algorithm>
#include <utility>

#include "base/functional/bind.h"
#include "base/strings/string_util.h"
#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_delegate.h"
#include "content/public/browser/web_ui.h"
#include "xenon_overlay/chrome/browser/account/xenon_account_service.h"
#include "xenon_overlay/chrome/browser/xenon_login_controller.h"

namespace xenon {

XenonAccountWebUIHandler::XenonAccountWebUIHandler(bool login_dialog)
    : login_dialog_(login_dialog) {}
XenonAccountWebUIHandler::~XenonAccountWebUIHandler() = default;

void XenonAccountWebUIHandler::RegisterMessages() {
  web_ui()->RegisterMessageCallback(
      "xenonAccountGetState",
      base::BindRepeating(&XenonAccountWebUIHandler::GetState,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "xenonAccountLogin",
      base::BindRepeating(&XenonAccountWebUIHandler::Authenticate,
                          base::Unretained(this), false));
  web_ui()->RegisterMessageCallback(
      "xenonAccountRegister",
      base::BindRepeating(&XenonAccountWebUIHandler::Authenticate,
                          base::Unretained(this), true));
  web_ui()->RegisterMessageCallback(
      "xenonAccountRefresh",
      base::BindRepeating(&XenonAccountWebUIHandler::Refresh,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "xenonAccountLogout",
      base::BindRepeating(&XenonAccountWebUIHandler::Logout,
                          base::Unretained(this)));
  if (login_dialog_) {
    web_ui()->RegisterMessageCallback(
        "xenonAccountContinue",
        base::BindRepeating(&XenonAccountWebUIHandler::Continue,
                            base::Unretained(this)));
    web_ui()->RegisterMessageCallback(
        "xenonLoginClose", base::BindRepeating(&XenonAccountWebUIHandler::Close,
                                               base::Unretained(this)));
  }
}

void XenonAccountWebUIHandler::OnJavascriptDisallowed() {
  weak_factory_.InvalidateWeakPtrs();
}

bool XenonAccountWebUIHandler::ValidateCallback(const base::ListValue& args,
                                                size_t argument_count) {
  if (args.empty() || !args[0].is_string() ||
      args[0].GetString().size() > 256) {
    return false;
  }
  AllowJavascript();
  if (args.size() != argument_count) {
    RejectJavascriptCallback(args[0], base::Value("账号请求参数无效。"));
    return false;
  }
  return true;
}

void XenonAccountWebUIHandler::GetState(const base::ListValue& args) {
  if (!ValidateCallback(args, 1)) {
    return;
  }
  ResolveJavascriptCallback(
      args[0], XenonAccountService::GetForProfile(Profile::FromWebUI(web_ui()))
                   ->GetState());
}

void XenonAccountWebUIHandler::Authenticate(bool registration,
                                            const base::ListValue& args) {
  if (!ValidateCallback(args, 2)) {
    return;
  }
  const auto* input = args[1].GetIfDict();
  const auto* username = input ? input->FindString("username") : nullptr;
  const auto* password = input ? input->FindString("password") : nullptr;
  if (!username || username->size() < 3 || username->size() > 64 ||
      !std::ranges::all_of(*username,
                           [](char c) {
                             return base::IsAsciiAlpha(c) ||
                                    base::IsAsciiDigit(c) || c == '_' ||
                                    c == '.' || c == '-';
                           }) ||
      !password || password->empty() || password->size() > 1024 ||
      password->find('\0') != std::string::npos ||
      !base::IsStringUTF8(*password)) {
    RejectJavascriptCallback(args[0], base::Value("请输入有效的账号和密码。"));
    return;
  }
  XenonAccountService::GetForProfile(Profile::FromWebUI(web_ui()))
      ->Authenticate(
          registration, *username, *password,
          base::BindOnce(&XenonAccountWebUIHandler::Reply,
                         weak_factory_.GetWeakPtr(), args[0].GetString()));
}

void XenonAccountWebUIHandler::Refresh(const base::ListValue& args) {
  if (!ValidateCallback(args, 1)) {
    return;
  }
  XenonAccountService::GetForProfile(Profile::FromWebUI(web_ui()))
      ->Refresh(base::BindOnce(&XenonAccountWebUIHandler::Reply,
                               weak_factory_.GetWeakPtr(),
                               args[0].GetString()));
}

void XenonAccountWebUIHandler::Logout(const base::ListValue& args) {
  if (!ValidateCallback(args, 1)) {
    return;
  }
  XenonAccountService::GetForProfile(Profile::FromWebUI(web_ui()))
      ->Logout(base::BindOnce(&XenonAccountWebUIHandler::Reply,
                              weak_factory_.GetWeakPtr(), args[0].GetString()));
}

void XenonAccountWebUIHandler::Continue(const base::ListValue& args) {
  if (!args.empty()) {
    return;
  }
  // The controller checks the native session and the pending launch profile.
  XenonLoginController::GetInstance()->SetAppSessionLoggedIn(
      Profile::FromWebUI(web_ui()), true);
}

void XenonAccountWebUIHandler::Close(const base::ListValue& args) {
  if (!args.empty()) {
    return;
  }
  auto* contents = web_ui()->GetWebContents();
  if (auto* delegate = contents->GetDelegate()) {
    delegate->CloseContents(contents);
  }
}

void XenonAccountWebUIHandler::Reply(std::string callback_id,
                                     base::DictValue state) {
  if (IsJavascriptAllowed()) {
    ResolveJavascriptCallback(base::Value(callback_id), state);
  }
}

}  // namespace xenon
