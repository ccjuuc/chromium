// Copyright 2026 The Xenon Authors. All rights reserved.

#ifndef XENON_OVERLAY_CHROME_BROWSER_UI_WEBUI_XENON_TRANSFER_WEBUI_HANDLER_H_
#define XENON_OVERLAY_CHROME_BROWSER_UI_WEBUI_XENON_TRANSFER_WEBUI_HANDLER_H_

#include <optional>
#include <string>

#include "base/files/file_path.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "content/public/browser/web_ui_message_handler.h"
#include "ui/shell_dialogs/select_file_dialog.h"

namespace xenon {

class XenonTransferWebUIHandler final : public content::WebUIMessageHandler,
                                        public ui::SelectFileDialog::Listener {
 public:
  XenonTransferWebUIHandler();
  ~XenonTransferWebUIHandler() override;
  void RegisterMessages() override;
  void OnJavascriptDisallowed() override;
  void FileSelected(const ui::SelectedFileInfo& file, int index) override;
  void FileSelectionCanceled() override;

 private:
  bool ValidateCallback(const base::ListValue& args, size_t count);
  void GetState(const base::ListValue& args);
  void Refresh(const base::ListValue& args);
  void SetUseStun(const base::ListValue& args);
  void ChooseFile(const base::ListValue& args);
  void OnFileInspected(std::string callback_id,
                       std::string account_id,
                       base::FilePath path,
                       std::optional<int64_t> size);
  void SendText(const base::ListValue& args);
  void SendFile(const base::ListValue& args);
  void Respond(const base::ListValue& args);
  void Cancel(const base::ListValue& args);
  void Reply(std::string callback_id, base::DictValue state);

  scoped_refptr<ui::SelectFileDialog> file_dialog_;
  std::string file_callback_id_;
  std::string picker_account_id_;
  std::string selected_account_id_;
  base::FilePath selected_path_;
  base::WeakPtrFactory<XenonTransferWebUIHandler> weak_factory_{this};
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_UI_WEBUI_XENON_TRANSFER_WEBUI_HANDLER_H_
