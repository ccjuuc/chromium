// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ui/webui/xenon_ui_controller.h"

#include "base/functional/bind.h"
#include "base/strings/utf_string_conversions.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_data_source.h"
#include "content/public/browser/web_ui_message_handler.h"
#include "content/public/common/url_constants.h"
#include "xenon_overlay/chrome/browser/xenon_extension_manager.h"
#include "xenon_overlay/chrome/browser/ui/xenon_common_dialog.h"
#include "xenon_overlay/chrome/browser/ui/xenon_shadow_test_window.h"
#include "xenon_overlay/chrome/browser/ui/xenon_web_dialog.h"
#include "xenon_overlay/resources/grit/xenon_resources.h"

namespace xenon {

namespace {
constexpr char kHost[] = "xenon-ui";

class XenonUIMessageHandler : public content::WebUIMessageHandler {
 public:
  XenonUIMessageHandler() = default;
  ~XenonUIMessageHandler() override = default;

  XenonUIMessageHandler(const XenonUIMessageHandler&) = delete;
  XenonUIMessageHandler& operator=(const XenonUIMessageHandler&) = delete;

 private:
  void RegisterMessages() override {
    web_ui()->RegisterMessageCallback(
        "showCommonDialog",
        base::BindRepeating(&XenonUIMessageHandler::HandleShowCommonDialog,
                            base::Unretained(this)));

    web_ui()->RegisterMessageCallback(
        "showWebDialog",
        base::BindRepeating(&XenonUIMessageHandler::HandleShowWebDialog,
                            base::Unretained(this)));

    web_ui()->RegisterMessageCallback(
        "showExtension",
        base::BindRepeating(&XenonUIMessageHandler::HandleShowExtension,
                            base::Unretained(this)));

    web_ui()->RegisterMessageCallback(
        "showWidgetShadowTestWindow",
        base::BindRepeating(
            &XenonUIMessageHandler::HandleShowWidgetShadowTestWindow,
            base::Unretained(this)));

    web_ui()->RegisterMessageCallback(
        "showWidgetShadowSample",
        base::BindRepeating(
            &XenonUIMessageHandler::HandleShowWidgetShadowSample,
            base::Unretained(this)));

    web_ui()->RegisterMessageCallback(
        "showViewShadowTestWindow",
        base::BindRepeating(
            &XenonUIMessageHandler::HandleShowViewShadowTestWindow,
            base::Unretained(this)));
  }

  void HandleShowExtension(const base::Value::List& args) {
    AllowJavascript();
    content::WebContents* web_contents = web_ui()->GetWebContents();
    if (!web_contents) {
      return;
    }
    XenonExtensionManager::GetInstance()->ShowExtension(
        web_contents->GetBrowserContext());
  }

  void HandleShowWidgetShadowTestWindow(const base::Value::List& args) {
    AllowJavascript();
    content::WebContents* web_contents = web_ui()->GetWebContents();
    if (!web_contents) {
      return;
    }
    XenonShadowTestWindow::ShowWidgetShadowTestWindow(
        web_contents->GetTopLevelNativeWindow());
  }

  void HandleShowWidgetShadowSample(const base::Value::List& args) {
    AllowJavascript();
    if (args.size() < 3) {
      return;
    }
    const std::string shadow_type_str =
        args[0].is_string() ? args[0].GetString() : "";
    const bool borderless = args[1].is_bool() ? args[1].GetBool() : false;
    const bool show_backdrop = args[2].is_bool() ? args[2].GetBool() : true;

    content::WebContents* web_contents = web_ui()->GetWebContents();
    if (!web_contents) {
      return;
    }
    XenonShadowTestWindow::ShowWidgetShadowSample(
        web_contents->GetTopLevelNativeWindow(), shadow_type_str, borderless,
        show_backdrop);
  }

  void HandleShowViewShadowTestWindow(const base::Value::List& args) {
    AllowJavascript();
    content::WebContents* web_contents = web_ui()->GetWebContents();
    if (!web_contents) {
      return;
    }
    XenonShadowTestWindow::ShowViewShadowTestWindow(
        web_contents->GetTopLevelNativeWindow());
  }

  void HandleShowCommonDialog(const base::Value::List& args) {
    AllowJavascript();

    if (args.size() < 8) {
      return;
    }

    const std::string style_str = args[0].is_string() ? args[0].GetString() : "medium";
    const std::u16string title = args[1].is_string() ? base::UTF8ToUTF16(args[1].GetString()) : std::u16string();
    const std::u16string body_text = args[2].is_string() ? base::UTF8ToUTF16(args[2].GetString()) : std::u16string();
    const std::u16string checkbox_text = args[3].is_string() ? base::UTF8ToUTF16(args[3].GetString()) : std::u16string();
    const bool checkbox_checked = args[4].is_bool() ? args[4].GetBool() : false;
    const std::u16string cancel_text = args[5].is_string() ? base::UTF8ToUTF16(args[5].GetString()) : std::u16string();
    const std::u16string confirm_text = args[6].is_string() ? base::UTF8ToUTF16(args[6].GetString()) : std::u16string();
    const bool show_mask = args[7].is_bool() ? args[7].GetBool() : true;

    XenonCommonDialog::Style style = (style_str == "small")
                                         ? XenonCommonDialog::Style::kSmall
                                         : XenonCommonDialog::Style::kMedium;

    gfx::NativeWindow parent = nullptr;
    content::WebContents* web_contents = web_ui()->GetWebContents();
    if (web_contents) {
      parent = web_contents->GetTopLevelNativeWindow();
    }

    XenonCommonDialog::Show(
        parent, style, title, body_text, checkbox_text, checkbox_checked,
        cancel_text, confirm_text,
        base::BindOnce(&XenonUIMessageHandler::OnDialogResult,
                       weak_ptr_factory_.GetWeakPtr()),
        show_mask);
  }

  void HandleShowWebDialog(const base::Value::List& args) {
    AllowJavascript();

    if (args.empty() || !args[0].is_dict()) {
      return;
    }
    const base::Value::Dict& options = args[0].GetDict();

    const std::string* url_str = options.FindString("url");
    if (!url_str) {
      return;
    }
    GURL url(*url_str);
    if (!url.is_valid()) {
      return;
    }

    content::WebContents* web_contents = web_ui()->GetWebContents();
    if (!web_contents) {
      return;
    }

    const std::string* title_str = options.FindString("title");
    std::u16string title = title_str ? base::UTF8ToUTF16(*title_str) : u"Xenon Web Dialog";
    int width = options.FindInt("width").value_or(800);
    int height = options.FindInt("height").value_or(600);
    bool modal = options.FindBool("modal").value_or(true);

    XenonWebDialog::ShowForLogin(
        web_contents->GetBrowserContext(), url, width, height, title,
        /*out_widget=*/nullptr, web_contents->GetTopLevelNativeWindow(),
        modal ? ui::mojom::ModalType::kWindow : ui::mojom::ModalType::kNone,
        base::OnceClosure(), /*show_close_button=*/true);
  }

  void OnDialogResult(const XenonCommonDialog::Result& result) {
    FireWebUIListener("dialog-result", base::Value(result.accepted),
                      base::Value(result.checkbox_checked));
  }

  base::WeakPtrFactory<XenonUIMessageHandler> weak_ptr_factory_{this};
};

}  // namespace

XenonUIController::XenonUIController(content::WebUI* web_ui)
    : content::WebUIController(web_ui) {
  content::WebUIDataSource* source = content::WebUIDataSource::CreateAndAdd(
      web_ui->GetWebContents()->GetBrowserContext(), kHost);

  source->AddResourcePath("xenon_ui.css", IDR_XENON_UI_CSS);
  source->AddResourcePath("xenon_ui.js", IDR_XENON_UI_JS);
  source->SetDefaultResource(IDR_XENON_UI_HTML);

  source->DisableTrustedTypesCSP();

  web_ui->AddMessageHandler(std::make_unique<XenonUIMessageHandler>());
}

XenonUIController::~XenonUIController() = default;

XenonUIConfig::XenonUIConfig()
    : content::WebUIConfig(content::kChromeUIScheme, kHost) {}

XenonUIConfig::~XenonUIConfig() = default;

std::unique_ptr<content::WebUIController>
XenonUIConfig::CreateWebUIController(content::WebUI* web_ui, const GURL& url) {
  return std::make_unique<XenonUIController>(web_ui);
}

}  // namespace xenon
