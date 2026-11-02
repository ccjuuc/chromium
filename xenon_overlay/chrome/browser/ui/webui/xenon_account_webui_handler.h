// Copyright 2026 The Xenon Authors. All rights reserved.

#ifndef XENON_OVERLAY_CHROME_BROWSER_UI_WEBUI_XENON_ACCOUNT_WEBUI_HANDLER_H_
#define XENON_OVERLAY_CHROME_BROWSER_UI_WEBUI_XENON_ACCOUNT_WEBUI_HANDLER_H_

#include <string>

#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "content/public/browser/web_ui_message_handler.h"

namespace xenon {

// Exposes account state and operations, never session tokens or device keys.
class XenonAccountWebUIHandler final : public content::WebUIMessageHandler {
 public:
  explicit XenonAccountWebUIHandler(bool login_dialog = false);
  ~XenonAccountWebUIHandler() override;
  void RegisterMessages() override;
  void OnJavascriptDisallowed() override;

 private:
  bool ValidateCallback(const base::ListValue& args, size_t argument_count);
  void GetState(const base::ListValue& args);
  void Authenticate(bool registration, const base::ListValue& args);
  void Refresh(const base::ListValue& args);
  void Logout(const base::ListValue& args);
  void Continue(const base::ListValue& args);
  void Close(const base::ListValue& args);
  void Reply(std::string callback_id, base::DictValue state);

  const bool login_dialog_;
  base::WeakPtrFactory<XenonAccountWebUIHandler> weak_factory_{this};
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_UI_WEBUI_XENON_ACCOUNT_WEBUI_HANDLER_H_
