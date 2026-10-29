// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/gm/gm_pin_dialog.h"

#include <algorithm>

#include "base/check_is_test.h"
#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/grit/generated_resources.h"
#include "components/constrained_window/constrained_window_views.h"
#include "components/web_modal/web_contents_modal_dialog_manager.h"
#include "components/web_modal/web_contents_modal_dialog_manager_delegate.h"
#include "content/public/browser/web_contents.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/base/models/simple_combobox_model.h"
#include "ui/base/mojom/dialog_button.mojom.h"
#include "ui/base/mojom/ui_base_types.mojom-shared.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/controls/combobox/combobox.h"
#include "ui/views/controls/label.h"
#include "ui/views/controls/textfield/textfield.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/layout/layout_provider.h"
#include "ui/views/widget/widget.h"
#include "ui/views/window/dialog_delegate.h"
#include "ui/views/window/non_client_view.h"

namespace gm_browser {
namespace {

class PinDialog final : public views::View {
  METADATA_HEADER(PinDialog, views::View)
 public:
  PinDialog(views::DialogDelegate* delegate,
            const std::string& origin,
            const std::vector<std::u16string>& identities,
            const std::u16string& message,
            PinCallback callback)
      : callback_(std::move(callback)) {
    delegate->SetTitle(l10n_util::GetStringUTF16(IDS_GM_PIN_TITLE));
    delegate->SetButtonLabel(ui::mojom::DialogButton::kOk,
                             l10n_util::GetStringUTF16(IDS_GM_PIN_AUTHORIZE));
    delegate->SetButtonLabel(ui::mojom::DialogButton::kCancel,
                             l10n_util::GetStringUTF16(IDS_GM_PIN_CANCEL));
    delegate->SetAcceptCallbackWithClose(
        base::BindRepeating(&PinDialog::Accept, base::Unretained(this)));
    delegate->SetCancelCallback(
        base::BindOnce(&PinDialog::Cancel, base::Unretained(this)));
    delegate->SetCloseCallback(
        base::BindOnce(&PinDialog::Cancel, base::Unretained(this)));
    const auto* layout_provider = views::LayoutProvider::Get();
    delegate->set_fixed_width(layout_provider->GetDistanceMetric(
        views::DISTANCE_MODAL_DIALOG_PREFERRED_WIDTH));
    delegate->set_margins(layout_provider->GetDialogInsetsForContentType(
        views::DialogContentType::kText, views::DialogContentType::kControl));
    SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kVertical, gfx::Insets(),
        layout_provider->GetDistanceMetric(
            views::DISTANCE_RELATED_CONTROL_VERTICAL)));
    auto* label =
        AddChildView(std::make_unique<views::Label>(l10n_util::GetStringFUTF16(
            IDS_GM_PIN_REQUEST, base::UTF8ToUTF16(origin))));
    label->SetMultiLine(true);
    label->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    std::vector<ui::SimpleComboboxModel::Item> items;
    for (const auto& identity : identities) {
      items.emplace_back(identity);
    }
    identities_ = AddChildView(std::make_unique<views::Combobox>(
        std::make_unique<ui::SimpleComboboxModel>(std::move(items))));
    identities_->GetViewAccessibility().SetName(
        l10n_util::GetStringUTF16(IDS_GM_PIN_CERTIFICATE));
    status_ = AddChildView(std::make_unique<views::Label>(message));
    status_->SetMultiLine(true);
    status_->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    status_->SetVisible(!message.empty());
    AddChildView(std::make_unique<views::Label>(
        l10n_util::GetStringUTF16(IDS_GM_PIN_PROMPT)));
    pin_ = AddChildView(std::make_unique<views::Textfield>());
    pin_->SetTextInputType(ui::TEXT_INPUT_TYPE_PASSWORD);
    pin_->GetViewAccessibility().SetName(
        l10n_util::GetStringUTF16(IDS_GM_PIN_PROMPT));
    delegate->SetInitiallyFocusedView(pin_);
  }
  ~PinDialog() override { Cancel(); }

 private:
  bool Accept() {
    if (!callback_) {
      return true;
    }
    auto selected = identities_->GetSelectedIndex();
    if (!selected) {
      ShowInputError(IDS_GM_PIN_SELECT_CERTIFICATE);
      return false;
    }
    // Borrow the text buffer. Do not leave an additional plaintext string copy
    // on the stack after the textfield and its edit history have been cleared.
    const auto& text = pin_->GetText();
    if (text.empty() || text.size() > 64 ||
        !std::ranges::all_of(
            text, [](char16_t c) { return c >= 0x20 && c <= 0x7e; })) {
      ShowInputError(IDS_GM_PIN_INVALID_INPUT);
      return false;
    }
    auto value = std::make_unique<gm_key::SecurePin>(
        std::vector<uint8_t>(text.begin(), text.end()));
    ClearPin();
    std::move(callback_).Run(*selected, std::move(value));
    return true;
  }

  void ClearPin() {
    pin_->SetText(std::u16string());
    pin_->ClearEditHistory();
  }

  void ShowInputError(int message_id) {
    status_->SetText(l10n_util::GetStringUTF16(message_id));
    status_->SetVisible(true);
    pin_->SetInvalid(true);
    PreferredSizeChanged();
    if (auto* widget = GetWidget()) {
      widget->SetSize(widget->non_client_view()->GetPreferredSize({}));
    }
    pin_->RequestFocus();
  }

  void Cancel() {
    if (!callback_) {
      return;
    }
    const auto selected = identities_->GetSelectedIndex();
    ClearPin();
    std::move(callback_).Run(selected.value_or(0), nullptr);
  }
  PinCallback callback_;
  raw_ptr<views::Combobox> identities_ = nullptr;
  raw_ptr<views::Textfield> pin_ = nullptr;
  raw_ptr<views::Label> status_ = nullptr;
};

BEGIN_METADATA(PinDialog)
END_METADATA

// The current Views API requires the client to own both widget and delegate.
// One owner lives until the dialog closes; the widget is destroyed first.
class PinDialogOwner {
 public:
  PinDialogOwner(const std::string& origin,
                 const std::vector<std::u16string>& identities,
                 const std::u16string& message,
                 PinCallback callback)
      : PinDialogOwner(std::make_unique<views::DialogDelegate>()) {
    delegate_->SetContentsView(std::make_unique<PinDialog>(
        delegate_.get(), origin, identities, message, std::move(callback)));
  }

  explicit PinDialogOwner(std::unique_ptr<views::DialogDelegate> delegate,
                          base::OnceClosure on_close = {})
      : delegate_(std::move(delegate)), on_close_(std::move(on_close)) {
    delegate_->SetOwnershipOfNewWidget(
        views::Widget::InitParams::CLIENT_OWNS_WIDGET);
  }

  base::WeakPtr<views::Widget> Show(content::WebContents* web_contents) {
    if (web_contents) {
      delegate_->SetModalType(ui::mojom::ModalType::kChild);
      widget_ = constrained_window::ShowWebModalDialogViewsOwned(
          delegate_.get(), web_contents,
          views::Widget::InitParams::CLIENT_OWNS_WIDGET);
    } else {
      widget_.reset(views::DialogDelegate::CreateDialogWidget(
          delegate_.get(), nullptr, nullptr));
      widget_->Show();
    }
    // Widget::OnNativeWidgetDestroyed also invokes this callback after external
    // native destruction, including CloseNow() and destruction by a parent.
    widget_->MakeCloseSynchronous(
        base::BindOnce([](PinDialogOwner* owner,
                          views::Widget::ClosedReason) { delete owner; },
                       base::Unretained(this)));
    return widget_->GetWeakPtr();
  }

 private:
  ~PinDialogOwner() {
    widget_.reset();
    delegate_.reset();
    if (on_close_) {
      std::move(on_close_).Run();
    }
  }

  // Declaration order ensures the widget releases its delegate before the
  // delegate is destroyed.
  std::unique_ptr<views::DialogDelegate> delegate_;
  std::unique_ptr<views::Widget> widget_;
  base::OnceClosure on_close_;
};

bool HasPageDialogHost(content::WebContents* web_contents) {
  auto* top_level =
      web_contents && !web_contents->IsBeingDestroyed()
          ? constrained_window::GetTopLevelWebContents(web_contents)
          : nullptr;
  auto* manager =
      top_level
          ? web_modal::WebContentsModalDialogManager::FromWebContents(top_level)
          : nullptr;
  return manager && manager->delegate() &&
         manager->delegate()->GetWebContentsModalDialogHost(top_level);
}
}  // namespace

base::WeakPtr<views::Widget> ShowPinDialogForWebContents(
    content::WebContents* web_contents,
    const std::string& origin,
    const std::vector<std::u16string>& identities,
    const std::u16string& message,
    PinCallback callback) {
  if (!HasPageDialogHost(web_contents)) {
    std::move(callback).Run(0, nullptr);
    return {};
  }
  auto* owner =
      new PinDialogOwner(origin, identities, message, std::move(callback));
  return owner->Show(web_contents);
}

base::WeakPtr<views::Widget> ShowPinLockedDialogForWebContents(
    content::WebContents* web_contents,
    const std::string& origin,
    base::OnceClosure on_close) {
  if (!HasPageDialogHost(web_contents)) {
    std::move(on_close).Run();
    return {};
  }
  auto delegate = std::make_unique<views::DialogDelegate>();
  delegate->SetTitle(l10n_util::GetStringUTF16(IDS_GM_PIN_LOCKED_TITLE));
  delegate->SetButtons(static_cast<int>(ui::mojom::DialogButton::kOk));
  delegate->SetButtonLabel(ui::mojom::DialogButton::kOk,
                           l10n_util::GetStringUTF16(IDS_GM_PIN_CLOSE));
  const auto* layout_provider = views::LayoutProvider::Get();
  delegate->set_fixed_width(layout_provider->GetDistanceMetric(
      views::DISTANCE_MODAL_DIALOG_PREFERRED_WIDTH));
  delegate->set_margins(layout_provider->GetDialogInsetsForContentType(
      views::DialogContentType::kText, views::DialogContentType::kText));
  auto label = std::make_unique<views::Label>(l10n_util::GetStringFUTF16(
      IDS_GM_PIN_LOCKED_MESSAGE, base::UTF8ToUTF16(origin)));
  label->SetMultiLine(true);
  label->SetHorizontalAlignment(gfx::ALIGN_LEFT);
  delegate->SetContentsView(std::move(label));
  auto* owner = new PinDialogOwner(std::move(delegate), std::move(on_close));
  return owner->Show(web_contents);
}

base::WeakPtr<views::Widget> ShowPinDialog(
    const std::string& origin,
    const std::vector<std::u16string>& identities,
    const std::u16string& message,
    PinCallback callback) {
  CHECK_IS_TEST();
  auto* owner =
      new PinDialogOwner(origin, identities, message, std::move(callback));
  return owner->Show(nullptr);
}
}  // namespace gm_browser
