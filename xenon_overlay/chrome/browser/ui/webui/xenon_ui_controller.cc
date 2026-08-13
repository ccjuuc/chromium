// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ui/webui/xenon_ui_controller.h"

#include <memory>

#include "base/functional/bind.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/global_browser_collection.h"
#include "chrome/browser/ui/views/bubble/webui_bubble_manager.h"
#include "chrome/browser/ui/webui/tab_search/tab_search_ui.h"
#include "chrome/common/webui_url_constants.h"
#include "chrome/grit/generated_resources.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_data_source.h"
#include "content/public/browser/web_ui_message_handler.h"
#include "content/public/common/url_constants.h"
#include "ui/base/mojom/menu_source_type.mojom.h"
#include "ui/views/bubble/bubble_border.h"
#include "ui/views/controls/menu/menu_delegate.h"
#include "ui/views/controls/menu/menu_item_view.h"
#include "ui/views/widget/widget.h"
#include "xenon_overlay/chrome/browser/ui/xenon_common_bubble.h"
#include "xenon_overlay/chrome/browser/ui/xenon_common_dialog.h"
#include "xenon_overlay/chrome/browser/ui/xenon_menu_runner.h"
#include "xenon_overlay/chrome/browser/ui/xenon_menu_shadow.h"
#include "xenon_overlay/chrome/browser/ui/xenon_shadow_test_window.h"
#include "xenon_overlay/chrome/browser/ui/xenon_web_dialog.h"
#include "xenon_overlay/chrome/browser/xenon_extension_manager.h"
#include "xenon_overlay/resources/grit/xenon_resources.h"
#include "xenon_overlay/xenon/chrome/browser/ui/views/xenon_toast.h"

namespace xenon {

namespace {
constexpr char kHost[] = "xenon-ui";
constexpr int kXenonMenuRunnerFirstCommandId = 1;
constexpr int kXenonMenuRunnerSecondCommandId = 2;

views::Widget* GetParentWidget(content::WebUI* web_ui) {
  content::WebContents* web_contents = web_ui->GetWebContents();
  return web_contents ? views::Widget::GetWidgetForNativeWindow(
                            web_contents->GetTopLevelNativeWindow())
                      : nullptr;
}

xunlei::XenonToast::Type ToastTypeFromString(const std::string& type) {
  if (type == "success") {
    return xunlei::XenonToast::Type::kSuccess;
  }
  if (type == "error") {
    return xunlei::XenonToast::Type::kError;
  }
  if (type == "warning") {
    return xunlei::XenonToast::Type::kWarning;
  }
  if (type == "loading") {
    return xunlei::XenonToast::Type::kLoading;
  }
  return xunlei::XenonToast::Type::kInfo;
}

ShadowStyle ShadowStyleFromString(const std::string& style) {
  if (style == "kNone") {
    return ShadowStyle::kNone;
  }
  if (style == "kViewShadow") {
    return ShadowStyle::kViewShadow;
  }
  if (style == "kCompositorShadow") {
    return ShadowStyle::kCompositorShadow;
  }
  if (style == "kBoxShadow") {
    return ShadowStyle::kBoxShadow;
  }
  return ShadowStyle::kBubbleBorder;
}

XenonMenuShadow ParseXenonMenuShadow(const base::ListValue& args) {
  XenonMenuShadow shadow;

  if (args.size() >= 1) {
    const std::string style = args[0].is_string() ? args[0].GetString() : "";
    shadow.style = ShadowStyleFromString(style);
  }
  if (args.size() >= 2) {
    shadow.elevation = args[1].is_int() ? args[1].GetInt() : kDefaultElevation;
  }
  if (args.size() >= 3) {
    if (args[2].is_double()) {
      shadow.opacity = args[2].GetDouble();
    } else if (args[2].is_int()) {
      shadow.opacity = args[2].GetInt();
    }
  }
  if (args.size() >= 5) {
    shadow.x_offset = args[3].is_int() ? args[3].GetInt() : kDefaultXOffset;
    shadow.y_offset = args[4].is_int() ? args[4].GetInt() : kDefaultYOffset;
  }
  if (args.size() >= 7) {
    shadow.spread = args[5].is_int() ? args[5].GetInt() : kDefaultSpread;
    shadow.color_hex =
        args[6].is_string() ? args[6].GetString() : kDefaultColorHex;
  }

  return shadow;
}

class XenonUIMessageHandler : public content::WebUIMessageHandler,
                              public views::MenuDelegate {
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

    web_ui()->RegisterMessageCallback(
        "showViewBorderTestWindow",
        base::BindRepeating(
            &XenonUIMessageHandler::HandleShowViewBorderTestWindow,
            base::Unretained(this)));

    web_ui()->RegisterMessageCallback(
        "showViewAnimationTestWindow",
        base::BindRepeating(
            &XenonUIMessageHandler::HandleShowViewAnimationTestWindow,
            base::Unretained(this)));

    web_ui()->RegisterMessageCallback(
        "showToast",
        base::BindRepeating(&XenonUIMessageHandler::HandleShowToast,
                            base::Unretained(this)));

    web_ui()->RegisterMessageCallback(
        "showXenonMenuRunner",
        base::BindRepeating(&XenonUIMessageHandler::HandleShowXenonMenuRunner,
                            base::Unretained(this)));

    web_ui()->RegisterMessageCallback(
        "showXenonCommonBubble",
        base::BindRepeating(&XenonUIMessageHandler::HandleShowXenonCommonBubble,
                            base::Unretained(this)));

    web_ui()->RegisterMessageCallback(
        "showXenonWebUIBubble",
        base::BindRepeating(&XenonUIMessageHandler::HandleShowXenonWebUIBubble,
                            base::Unretained(this)));
  }

  void HandleShowExtension(const base::ListValue& args) {
    AllowJavascript();
    content::WebContents* web_contents = web_ui()->GetWebContents();
    if (!web_contents) {
      return;
    }
    XenonExtensionManager::GetInstance()->ShowExtension(
        web_contents->GetBrowserContext());
  }

  void HandleShowWidgetShadowTestWindow(const base::ListValue& args) {
    AllowJavascript();
    content::WebContents* web_contents = web_ui()->GetWebContents();
    if (!web_contents) {
      return;
    }
    XenonShadowTestWindow::ShowWidgetShadowTestWindow(
        web_contents->GetTopLevelNativeWindow());
  }

  void HandleShowWidgetShadowSample(const base::ListValue& args) {
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

  void HandleShowViewShadowTestWindow(const base::ListValue& args) {
    AllowJavascript();
    content::WebContents* web_contents = web_ui()->GetWebContents();
    if (!web_contents) {
      return;
    }
    XenonShadowTestWindow::ShowViewShadowTestWindow(
        web_contents->GetTopLevelNativeWindow());
  }

  void HandleShowViewBorderTestWindow(const base::ListValue& args) {
    AllowJavascript();
    content::WebContents* web_contents = web_ui()->GetWebContents();
    if (!web_contents) {
      return;
    }
    XenonShadowTestWindow::ShowViewBorderTestWindow(
        web_contents->GetTopLevelNativeWindow());
  }

  void HandleShowViewAnimationTestWindow(const base::ListValue& args) {
    AllowJavascript();
    content::WebContents* web_contents = web_ui()->GetWebContents();
    if (!web_contents) {
      return;
    }
    XenonShadowTestWindow::ShowViewAnimationTestWindow(
        web_contents->GetTopLevelNativeWindow());
  }

  void HandleShowCommonDialog(const base::ListValue& args) {
    AllowJavascript();

    if (args.size() < 8) {
      return;
    }

    const std::string style_str =
        args[0].is_string() ? args[0].GetString() : "medium";
    const std::u16string title = args[1].is_string()
                                     ? base::UTF8ToUTF16(args[1].GetString())
                                     : std::u16string();
    const std::u16string body_text =
        args[2].is_string() ? base::UTF8ToUTF16(args[2].GetString())
                            : std::u16string();
    const std::u16string checkbox_text =
        args[3].is_string() ? base::UTF8ToUTF16(args[3].GetString())
                            : std::u16string();
    const bool checkbox_checked = args[4].is_bool() ? args[4].GetBool() : false;
    const std::u16string cancel_text =
        args[5].is_string() ? base::UTF8ToUTF16(args[5].GetString())
                            : std::u16string();
    const std::u16string confirm_text =
        args[6].is_string() ? base::UTF8ToUTF16(args[6].GetString())
                            : std::u16string();
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

  void HandleShowWebDialog(const base::ListValue& args) {
    AllowJavascript();

    if (args.empty() || !args[0].is_dict()) {
      return;
    }
    const base::DictValue& options = args[0].GetDict();

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

    XenonWebDialog::ShowWithOptions(
        web_contents->GetBrowserContext(), url, options,
        /*out_widget=*/nullptr, web_contents->GetTopLevelNativeWindow(),
        base::OnceClosure());
  }

  void HandleShowToast(const base::ListValue& args) {
    AllowJavascript();

    if (args.empty() || !args[0].is_dict()) {
      return;
    }
    const base::DictValue& options = args[0].GetDict();

    content::WebContents* web_contents = web_ui()->GetWebContents();
    if (!web_contents) {
      return;
    }

    xunlei::XenonToast::Params params;
    if (const std::string* type = options.FindString("type")) {
      params.type = ToastTypeFromString(*type);
    }
    if (const std::string* text = options.FindString("text")) {
      params.text = base::UTF8ToUTF16(*text);
    }
    if (const std::string* action_text = options.FindString("action_text")) {
      params.action_text = base::UTF8ToUTF16(*action_text);
    }
    if (const std::optional<int> duration_ms = options.FindInt("duration_ms")) {
      if (*duration_ms > 0) {
        params.duration = base::Milliseconds(*duration_ms);
      }
    }
    if (!params.action_text.empty()) {
      params.action_callback =
          base::BindOnce(&XenonUIMessageHandler::OnToastAction,
                         weak_ptr_factory_.GetWeakPtr());
    }

    xunlei::XenonToast::Show(web_contents->GetTopLevelNativeWindow(),
                             std::move(params));
  }

  void OnDialogResult(const XenonCommonDialog::Result& result) {
    FireWebUIListener("dialog-result", base::Value(result.accepted),
                      base::Value(result.checkbox_checked));
  }

  void OnToastAction() { FireWebUIListener("toast-action", base::Value(true)); }

  void HandleShowXenonMenuRunner(const base::ListValue& args) {
    AllowJavascript();

    content::WebContents* web_contents = web_ui()->GetWebContents();
    views::Widget* parent_widget = GetParentWidget(web_ui());
    if (!web_contents || !parent_widget) {
      return;
    }

    XenonMenuShadow shadow = ParseXenonMenuShadow(args);

    xenon_menu_runner_.reset();
    auto menu = std::make_unique<views::MenuItemView>(this);
    menu->AppendMenuItem(kXenonMenuRunnerFirstCommandId, u"测试菜单项 A");
    menu->AppendMenuItem(kXenonMenuRunnerSecondCommandId, u"测试菜单项 B");

    xenon_menu_runner_ = std::make_unique<XenonMenuRunner>(
        std::move(menu), views::MenuRunner::NO_FLAGS);
    xenon_menu_runner_->RunMenuAt(parent_widget, nullptr,
                                  web_contents->GetContainerBounds(),
                                  views::MenuAnchorPosition::kTopLeft,
                                  ui::mojom::MenuSourceType::kNone, shadow);
  }

  void HandleShowXenonCommonBubble(const base::ListValue& args) {
    AllowJavascript();

    content::WebContents* web_contents = web_ui()->GetWebContents();
    views::Widget* parent_widget = GetParentWidget(web_ui());
    if (!web_contents || !parent_widget) {
      return;
    }

    const gfx::Rect container_bounds = web_contents->GetContainerBounds();
    XenonCommonBubble::ShowAt(parent_widget->GetContentsView(),
                              gfx::Rect(container_bounds.origin(),
                                        gfx::Size(container_bounds.width(), 0)),
                              views::BubbleBorder::TOP_LEFT,
                              u"XenonCommonBubble\n16px 圆角 + 同步阴影设置",
                              ParseXenonMenuShadow(args));
  }

  void HandleShowXenonWebUIBubble(const base::ListValue& args) {
    AllowJavascript();

    content::WebContents* web_contents = web_ui()->GetWebContents();
    views::Widget* parent_widget = GetParentWidget(web_ui());
    BrowserWindowInterface* browser_window =
        web_contents ? GlobalBrowserCollection::GetInstance()->FindBrowserWithTab(
                           web_contents)
                     : nullptr;
    Browser* browser = browser_window
                           ? browser_window->GetBrowserForMigrationOnly()
                           : nullptr;
    if (!web_contents || !parent_widget || !browser) {
      return;
    }

    XenonMenuShadow shadow = ParseXenonMenuShadow(args);

    if (xenon_webui_bubble_manager_ &&
        xenon_webui_bubble_manager_->GetBubbleWidget()) {
      xenon_webui_bubble_manager_->CloseBubble();
      return;
    }

    if (!xenon_webui_bubble_manager_) {
      xenon_webui_bubble_manager_ = WebUIBubbleManager::Create<TabSearchUI>(
          browser, GURL(chrome::kChromeUITabSearchURL),
          IDS_ACCNAME_TAB_SEARCH);
      XenonCommonBubble::ConfigureWebUIBubbleManager(
          xenon_webui_bubble_manager_.get());
    }

    const gfx::Rect container_bounds = web_contents->GetContainerBounds();
    if (xenon_webui_bubble_manager_->ShowBubble(
            gfx::Rect(container_bounds.origin(),
                      gfx::Size(container_bounds.width(), 0)),
            views::BubbleBorder::TOP_LEFT)) {
      XenonCommonBubble::ApplyWebUIBubbleStyle(
          xenon_webui_bubble_manager_.get(), shadow);
    }
  }

  // views::MenuDelegate:
  void ExecuteCommand(int id) override {
    FireWebUIListener("xenon-menu-command", base::Value(id));
  }

  std::unique_ptr<XenonMenuRunner> xenon_menu_runner_;
  std::unique_ptr<WebUIBubbleManager> xenon_webui_bubble_manager_;
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

std::unique_ptr<content::WebUIController> XenonUIConfig::CreateWebUIController(
    content::WebUI* web_ui,
    const GURL& url) {
  return std::make_unique<XenonUIController>(web_ui);
}

}  // namespace xenon
