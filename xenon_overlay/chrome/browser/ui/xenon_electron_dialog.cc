// Copyright 2026 The Xenon Authors. All rights reserved.

#include "xenon_overlay/chrome/browser/ui/xenon_electron_dialog.h"

#include <map>
#include <memory>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/memory/ptr_util.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/scoped_observation.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "build/build_config.h"
#include "ui/base/accelerators/accelerator.h"
#include "ui/base/mojom/dialog_button.mojom.h"
#include "ui/base/mojom/ui_base_types.mojom-shared.h"
#include "ui/events/event_constants.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/shell_dialogs/select_file_dialog.h"
#include "ui/shell_dialogs/select_file_policy.h"
#include "ui/shell_dialogs/selected_file_info.h"
#include "ui/views/controls/button/md_text_button.h"
#include "ui/views/controls/message_box_view.h"
#include "ui/views/layout/box_layout_view.h"
#include "ui/views/widget/widget.h"
#include "ui/views/widget/widget_observer.h"
#include "ui/views/window/dialog_delegate.h"

namespace xenon {
namespace {

class DialogRequest;
using Requests = std::map<std::string, std::set<DialogRequest*>>;

Requests& PendingRequests() {
  static base::NoDestructor<Requests> requests;
  return *requests;
}

void Reply(ElectronDialogCallback callback,
           base::Value result,
           std::string error) {
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(std::move(callback), std::move(result), std::move(error)));
}

std::string Invalid(std::string_view field) {
  return "ERR_INVALID_ARG_VALUE: invalid dialog " + std::string(field);
}

std::string Unsupported(std::string_view field) {
  return "ERR_NOT_SUPPORTED: dialog " + std::string(field) +
         " is not supported";
}

bool ReadText(const base::DictValue& options,
              std::string_view name,
              std::string* out,
              size_t limit = 1024 * 1024) {
  const base::Value* value = options.Find(name);
  if (!value) {
    return true;
  }
  const std::string* text = value->GetIfString();
  if (!text || text->size() > limit || text->find('\0') != std::string::npos ||
      !base::IsStringUTF8(*text)) {
    return false;
  }
  *out = *text;
  return true;
}

bool ReadBool(const base::DictValue& options,
              std::string_view name,
              bool* out) {
  const base::Value* value = options.Find(name);
  if (!value) {
    return true;
  }
  if (!value->is_bool()) {
    return false;
  }
  *out = value->GetBool();
  return true;
}

// Owns itself until Finish() posts its deletion. This also permits native file
// pickers to complete synchronously inside SelectFile() without deleting their
// listener on the active call stack.
class DialogRequest : public views::WidgetObserver {
 public:
  DialogRequest(std::string owner_token,
                views::Widget* owner,
                ElectronDialogCallback callback)
      : owner_token_(std::move(owner_token)), callback_(std::move(callback)) {
    PendingRequests()[owner_token_].insert(this);
    if (owner) {
      owner_observation_.Observe(owner);
    }
  }

  ~DialogRequest() override = default;

  void Abort() {
    Finish(base::Value(), "ERR_ABORTED: dialog owner is no longer available");
  }

 protected:
  void OnWidgetDestroying(views::Widget* widget) override { Abort(); }

  virtual void Detach() {}

  void Finish(base::Value result, std::string error = {}) {
    if (!callback_) {
      return;
    }
    owner_observation_.Reset();
    auto it = PendingRequests().find(owner_token_);
    CHECK(it != PendingRequests().end());
    it->second.erase(this);
    if (it->second.empty()) {
      PendingRequests().erase(it);
    }
    auto callback = std::move(callback_);
    Detach();
    // Destroy UI and listener before delivering the reply, while keeping both
    // alive until any button/listener/owner-observer stack has unwound.
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(
            [](std::unique_ptr<DialogRequest> request,
               ElectronDialogCallback callback, base::Value result,
               std::string error) {
              request.reset();
              std::move(callback).Run(std::move(result), std::move(error));
            },
            base::WrapUnique(this), std::move(callback), std::move(result),
            std::move(error)));
  }

 private:
  const std::string owner_token_;
  ElectronDialogCallback callback_;
  base::ScopedObservation<views::Widget, views::WidgetObserver>
      owner_observation_{this};
};

struct MessageOptions {
  std::string title;
  std::string message;
  std::string detail;
  std::string checkbox_label;
  std::vector<std::string> buttons = {"OK"};
  int default_id = 0;
  int cancel_id = 0;
  bool checkbox_checked = false;
  bool normalize_access_keys = false;
};

std::string ParseMessageOptions(const base::DictValue& options,
                                MessageOptions* out) {
  if (!ReadText(options, "title", &out->title, 64 * 1024) ||
      !ReadText(options, "message", &out->message) ||
      !ReadText(options, "detail", &out->detail) ||
      !ReadText(options, "checkboxLabel", &out->checkbox_label, 64 * 1024) ||
      !ReadBool(options, "checkboxChecked", &out->checkbox_checked) ||
      !ReadBool(options, "normalizeAccessKeys", &out->normalize_access_keys)) {
    return Invalid("options");
  }
  bool no_link = false;
  if (!ReadBool(options, "noLink", &no_link)) {
    return Invalid("noLink");
  }
  std::string type = "none";
  if (!ReadText(options, "type", &type, 32) ||
      (type != "none" && type != "info" && type != "error" &&
       type != "question" && type != "warning")) {
    return Invalid("type");
  }
  if (options.contains("icon") || options.contains("textWidth")) {
    return Unsupported("icon/textWidth");
  }
  if (const auto* value = options.Find("buttons")) {
    const auto* buttons = value->GetIfList();
    if (!buttons || buttons->empty() || buttons->size() > 32) {
      return Invalid("buttons");
    }
    out->buttons.clear();
    for (const auto& button : *buttons) {
      if (!button.is_string() || button.GetString().size() > 4096 ||
          button.GetString().find('\0') != std::string::npos ||
          !base::IsStringUTF8(button.GetString())) {
        return Invalid("buttons");
      }
      out->buttons.push_back(button.GetString());
    }
  }
  for (auto [name, target] : {std::pair{"defaultId", &out->default_id},
                              std::pair{"cancelId", &out->cancel_id}}) {
    if (const auto* value = options.Find(name)) {
      const bool negative_one = value->is_int() && value->GetInt() == -1;
      const bool native_default =
          std::string_view(name) == "defaultId" && negative_one;
      if (!value->is_int() ||
          (!negative_one &&
           (value->GetInt() < 0 ||
            static_cast<size_t>(value->GetInt()) >= out->buttons.size()))) {
        return Invalid(name);
      }
      *target = native_default ? 0 : value->GetInt();
    }
  }
  return {};
}

// Convert Electron/Windows ampersand mnemonics into plain labels plus real
// keyboard accelerators. Escaped ampersands remain visible.
std::u16string ButtonText(const std::string& text,
                          bool normalize,
                          ui::KeyboardCode* mnemonic) {
  std::u16string label = base::UTF8ToUTF16(text);
#if BUILDFLAG(IS_MAC)
  if (!normalize) {
    return label;
  }
  constexpr char16_t marker = u'&';
#elif BUILDFLAG(IS_WIN)
  constexpr char16_t marker = u'&';
#else
  // GTK interprets underscores by default. Electron's normalization changes
  // ampersands into GTK mnemonics and escapes existing underscores; parsing the
  // original ampersands here gives the equivalent Views label/accelerator.
  const char16_t marker = normalize ? u'&' : u'_';
#endif
  std::u16string result;
  for (size_t i = 0; i < label.size(); ++i) {
    if (label[i] != marker) {
      result.push_back(label[i]);
      continue;
    }
    if (i + 1 == label.size()) {
      break;
    }
    const char16_t next = label[++i];
    if (next != marker && *mnemonic == ui::VKEY_UNKNOWN) {
      const char16_t upper = base::ToUpperASCII(next);
      if ((upper >= u'A' && upper <= u'Z') ||
          (upper >= u'0' && upper <= u'9')) {
        *mnemonic = static_cast<ui::KeyboardCode>(upper);
      }
    }
    result.push_back(next);
  }
  return result;
}

class MessageRequest final : public DialogRequest {
 public:
  MessageRequest(std::string owner_token,
                 views::Widget* owner,
                 ElectronDialogCallback callback)
      : DialogRequest(std::move(owner_token), owner, std::move(callback)) {}

  void Show(views::Widget* owner, const MessageOptions& options) {
    cancel_id_ = options.cancel_id;
    delegate_ = std::make_unique<views::DialogDelegate>();
    delegate_->SetTitle(base::UTF8ToUTF16(options.title));
    delegate_->SetButtons(static_cast<int>(ui::mojom::DialogButton::kNone));
    delegate_->SetDefaultButton(
        static_cast<int>(ui::mojom::DialogButton::kNone));
    delegate_->SetModalType(owner ? ui::mojom::ModalType::kWindow
                                  : ui::mojom::ModalType::kNone);
    delegate_->SetShowCloseButton(true);
    delegate_->SetOwnershipOfNewWidget(
        views::Widget::InitParams::CLIENT_OWNS_WIDGET);
#if BUILDFLAG(IS_MAC)
    // AppKit window-modal sheets require a native frame.
    delegate_->set_use_custom_frame(false);
#endif
    auto contents = std::make_unique<views::BoxLayoutView>();
    contents->SetOrientation(views::BoxLayout::Orientation::kVertical);
    contents->SetBetweenChildSpacing(20);
    contents->SetInsideBorderInsets(gfx::Insets(20));
    const std::string text = options.detail.empty()
                                 ? options.message
                                 : options.message + "\n\n" + options.detail;
    message_view_ = contents->AddChildView(
        std::make_unique<views::MessageBoxView>(base::UTF8ToUTF16(text)));
    message_view_->SetMessageWidth(480);
    if (!options.checkbox_label.empty()) {
      message_view_->SetCheckBoxLabel(
          base::UTF8ToUTF16(options.checkbox_label));
      message_view_->SetCheckBoxSelected(options.checkbox_checked);
    }
    auto* buttons =
        contents->AddChildView(std::make_unique<views::BoxLayoutView>());
    buttons->SetMainAxisAlignment(views::BoxLayout::MainAxisAlignment::kEnd);
    buttons->SetBetweenChildSpacing(8);
    for (size_t i = 0; i < options.buttons.size(); ++i) {
      ui::KeyboardCode mnemonic = ui::VKEY_UNKNOWN;
      const auto label = ButtonText(options.buttons[i],
                                    options.normalize_access_keys, &mnemonic);
      auto* button =
          buttons->AddChildView(std::make_unique<views::MdTextButton>(
              base::BindRepeating(&MessageRequest::Choose,
                                  weak_factory_.GetWeakPtr(),
                                  static_cast<int>(i)),
              label));
      if (static_cast<int>(i) == options.default_id) {
        button->SetIsDefault(true);
        delegate_->SetInitiallyFocusedView(button);
      }
#if !BUILDFLAG(IS_MAC)
      if (mnemonic != ui::VKEY_UNKNOWN) {
        button->AddAccelerator(ui::Accelerator(mnemonic, ui::EF_ALT_DOWN));
      }
#endif
    }
    delegate_->SetContentsView(std::move(contents));
    widget_.reset(views::DialogDelegate::CreateDialogWidget(
        delegate_.get(), owner ? owner->GetNativeWindow() : gfx::NativeWindow(),
        owner ? owner->GetNativeView() : gfx::NativeView()));
    widget_->MakeCloseSynchronous(
        base::BindOnce(&MessageRequest::OnClosed, weak_factory_.GetWeakPtr()));
    widget_->Show();
  }

 private:
  void Choose(int response) {
    Finish(base::Value(
        base::DictValue()
            .Set("response", response)
            .Set("checkboxChecked", message_view_->IsCheckBoxSelected())));
  }

  void OnClosed(views::Widget::ClosedReason reason) { Choose(cancel_id_); }

  void Detach() override {
    weak_factory_.InvalidateWeakPtrs();
    message_view_ = nullptr;
  }

  int cancel_id_ = 0;
  raw_ptr<views::MessageBoxView> message_view_ = nullptr;
  // Widget must be destroyed before its client-owned delegate.
  std::unique_ptr<views::DialogDelegate> delegate_;
  std::unique_ptr<views::Widget> widget_;
  base::WeakPtrFactory<MessageRequest> weak_factory_{this};
};

struct FileDialogOptions {
  std::string title;
  base::FilePath default_path;
  ui::SelectFileDialog::FileTypeInfo file_types;
  ui::SelectFileDialog::Type type = ui::SelectFileDialog::SELECT_SAVEAS_FILE;
};

std::string ParseFileDialogOptions(const base::DictValue& options,
                                   bool save,
                                   FileDialogOptions* out) {
  std::string default_path;
  if (!ReadText(options, "title", &out->title, 64 * 1024) ||
      !ReadText(options, "defaultPath", &default_path, 64 * 1024)) {
    return Invalid("options");
  }
  out->default_path = base::FilePath::FromUTF8Unsafe(default_path);
  for (const char* field : {"buttonLabel", "message", "nameFieldLabel"}) {
    std::string value;
    if (!ReadText(options, field, &value)) {
      return Invalid(field);
    }
    if (!value.empty()) {
      return Unsupported(field);
    }
  }
  for (const char* field : {"securityScopedBookmarks", "showsTagField"}) {
    bool enabled = false;
    if (!ReadBool(options, field, &enabled)) {
      return Invalid(field);
    }
    if (enabled) {
      return Unsupported(field);
    }
  }
  [[maybe_unused]] bool open_file = false;
  bool open_directory = false;
  bool multiple = false;
  bool create_directory = false;
  if (const auto* value = options.Find("properties")) {
    const auto* properties = value->GetIfList();
    if (!properties) {
      return Invalid("properties");
    }
    for (const auto& property : *properties) {
      if (!property.is_string()) {
        return Invalid("properties");
      }
      const auto& name = property.GetString();
      if (save) {
        // These are intrinsic features of the platform save picker.
        if (name != "createDirectory" && name != "showOverwriteConfirmation") {
          return Unsupported(name);
        }
      } else if (name == "openFile") {
        open_file = true;
      } else if (name == "openDirectory") {
        open_directory = true;
      } else if (name == "multiSelections") {
        multiple = true;
      } else if (name == "createDirectory") {
        create_directory = true;
      } else {
        return Unsupported(name);
      }
    }
  }
  if (!save) {
    if (open_directory && multiple) {
      return Unsupported("multiple directory selection");
    }
#if BUILDFLAG(IS_MAC)
    // Electron supports mixed file/directory selection on macOS, but the
    // Chromium picker abstraction cannot express that mode.
    if (open_file && open_directory) {
      return Unsupported("mixed file and directory selection");
    }
#endif
    // Windows and Linux follow Electron's directory precedence when both
    // selection modes are requested. With neither property, select a file.
    out->type =
        open_directory
            ? (create_directory ? ui::SelectFileDialog::SELECT_FOLDER
                                : ui::SelectFileDialog::SELECT_EXISTING_FOLDER)
            : (multiple ? ui::SelectFileDialog::SELECT_OPEN_MULTI_FILE
                        : ui::SelectFileDialog::SELECT_OPEN_FILE);
  }
  out->file_types.keep_extension_visible = true;
  if (const auto* value = options.Find("filters")) {
    const auto* filters = value->GetIfList();
    if (!filters || filters->size() > 128) {
      return Invalid("filters");
    }
    for (const auto& filter_value : *filters) {
      const auto* filter = filter_value.GetIfDict();
      std::string name;
      const auto* extensions =
          filter ? filter->FindList("extensions") : nullptr;
      if (!filter || !ReadText(*filter, "name", &name, 4096) || !extensions ||
          extensions->empty() || extensions->size() > 128) {
        return Invalid("filters");
      }
      ui::SelectFileDialog::FileTypeInfo::FileExtensionList group;
      for (const auto& extension : *extensions) {
        if (!extension.is_string() || extension.GetString().empty() ||
            extension.GetString().size() > 255) {
          return Invalid("filters.extensions");
        }
        const auto& text = extension.GetString();
        if (text == "*") {
          out->file_types.include_all_files = true;
          continue;
        }
        if (!base::IsStringUTF8(text) ||
            text.find_first_of("./\\;:*?\"<>|") != std::string::npos ||
            text.find('\0') != std::string::npos) {
          return Invalid("filters.extensions");
        }
        group.push_back(base::FilePath::FromUTF8Unsafe(text).value());
      }
      if (!group.empty()) {
        out->file_types.extensions.push_back(std::move(group));
        out->file_types.extension_description_overrides.push_back(
            base::UTF8ToUTF16(name));
      }
    }
  }
  if (out->file_types.extensions.empty()) {
    out->file_types.include_all_files = true;
  }
  return {};
}

class FileDialogRequest final : public DialogRequest,
                                public ui::SelectFileDialog::Listener {
 public:
  FileDialogRequest(std::string owner_token,
                    views::Widget* owner,
                    ElectronDialogCallback callback)
      : DialogRequest(std::move(owner_token), owner, std::move(callback)) {}

  void Show(views::Widget* owner, FileDialogOptions options) {
    options_ = std::move(options);
    dialog_ = ui::SelectFileDialog::Create(this, nullptr);
    if (!dialog_) {
      Finish(base::Value(), Unsupported("native file picker"));
      return;
    }
    // Keep a local reference: SelectFile may synchronously call the listener,
    // whose Finish detaches and releases dialog_.
    scoped_refptr<ui::SelectFileDialog> dialog = dialog_;
    const bool has_types = !options_.file_types.extensions.empty();
    dialog->SelectFile(
        options_.type, base::UTF8ToUTF16(options_.title), options_.default_path,
        &options_.file_types, has_types ? 1 : 0,
        has_types ? options_.file_types.extensions.front().front()
                  : base::FilePath::StringType(),
        owner ? owner->GetNativeWindow() : gfx::NativeWindow(), nullptr);
  }

 private:
  void FileSelected(const ui::SelectedFileInfo& file, int index) override {
    CompleteSelection({file});
  }

  void MultiFilesSelected(
      const std::vector<ui::SelectedFileInfo>& files) override {
    CompleteSelection(files);
  }

  void CompleteSelection(const std::vector<ui::SelectedFileInfo>& files) {
    if (files.empty() ||
        (options_.type != ui::SelectFileDialog::SELECT_OPEN_MULTI_FILE &&
         files.size() != 1)) {
      Finish(base::Value(),
             "ERR_FAILED: native file picker returned invalid selection");
      return;
    }
    base::ListValue paths;
    for (const auto& file : files) {
      if (file.path().empty()) {
        Finish(base::Value(),
               "ERR_FAILED: native file picker returned no path");
        return;
      }
      paths.Append(file.path().AsUTF8Unsafe());
    }
    base::DictValue result;
    result.Set("canceled", false);
    if (options_.type == ui::SelectFileDialog::SELECT_SAVEAS_FILE) {
      result.Set("filePath", paths[0].GetString());
    } else {
      result.Set("filePaths", std::move(paths));
    }
    Finish(base::Value(std::move(result)));
  }

  void FileSelectionCanceled() override {
    base::DictValue result;
    result.Set("canceled", true);
    if (options_.type == ui::SelectFileDialog::SELECT_SAVEAS_FILE) {
      result.Set("filePath", "");
    } else {
      result.Set("filePaths", base::ListValue());
    }
    Finish(base::Value(std::move(result)));
  }

  void Detach() override {
    if (dialog_) {
      dialog_->ListenerDestroyed();
      dialog_ = nullptr;
    }
  }

  FileDialogOptions options_;
  scoped_refptr<ui::SelectFileDialog> dialog_;
};

}  // namespace

void ShowElectronMessageBox(const std::string& owner_token,
                            views::Widget* owner,
                            const base::DictValue& options,
                            ElectronDialogCallback callback) {
  MessageOptions parsed;
  std::string error = ParseMessageOptions(options, &parsed);
  if (!error.empty()) {
    Reply(std::move(callback), base::Value(), std::move(error));
    return;
  }
  (new MessageRequest(owner_token, owner, std::move(callback)))
      ->Show(owner, parsed);
}

void ShowElectronSaveDialog(const std::string& owner_token,
                            views::Widget* owner,
                            const base::DictValue& options,
                            ElectronDialogCallback callback) {
  FileDialogOptions parsed;
  std::string error = ParseFileDialogOptions(options, true, &parsed);
  if (!error.empty()) {
    Reply(std::move(callback), base::Value(), std::move(error));
    return;
  }
  (new FileDialogRequest(owner_token, owner, std::move(callback)))
      ->Show(owner, std::move(parsed));
}

void ShowElectronOpenDialog(const std::string& owner_token,
                            views::Widget* owner,
                            const base::DictValue& options,
                            ElectronDialogCallback callback) {
  FileDialogOptions parsed;
  std::string error = ParseFileDialogOptions(options, false, &parsed);
  if (!error.empty()) {
    Reply(std::move(callback), base::Value(), std::move(error));
    return;
  }
  (new FileDialogRequest(owner_token, owner, std::move(callback)))
      ->Show(owner, std::move(parsed));
}

void CancelElectronDialogs(const std::string& owner_token) {
  // Abort removes itself from the registry before scheduling its reply.
  for (;;) {
    const auto it = PendingRequests().find(owner_token);
    if (it == PendingRequests().end()) {
      return;
    }
    (*it->second.begin())->Abort();
  }
}

}  // namespace xenon
