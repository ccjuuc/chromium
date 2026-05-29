// Copyright 2026 The Xenon Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ui/webui/xenon_webui_controller.h"

#include <mutex>
#include <utility>

#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_delegate.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_browser_interface_broker_registry.h"
#include "content/public/browser/web_ui_data_source.h"
#include "content/public/browser/web_ui_message_handler.h"
#include "content/public/common/url_constants.h"
#include "xenon_overlay/chrome/browser/xenon_login_controller.h"
#include "xenon_overlay/resources/grit/xenon_resources.h"

#include <list>
#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/ref_counted_memory.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/url_data_source.h"
#include "net/base/mime_util.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/url_loader_factory.mojom.h"

namespace xenon {

// =============================================================================
// --- Xenon WebUI Development Server Proxy (Debug Only) ---
// =============================================================================
// This class implements a transparent reverse proxy for local WebUI development.
// It intercepts requests from the sandboxed chrome:// origin and redirects them
// asynchronously to a local npm run dev Vite server (or other framework server).
//
// Key Features:
// 1. Dynamic Server Target: Configure target port via --xenon-dev-server-url
//    (defaults to http://127.0.0.1:3000).
// 2. Safe Lifecycle Handling: Utilizes structured list-based PendingRequests
//    to clean up outstanding network callbacks safely upon destruction, preventing
//    dangling pointers and memory safety violations.
// 3. Elegant Whitelist MIME Resolution: Delegating static assets to net::GetMimeTypeFromFile
//    while defaulting Vite compiled scripts, styles, and modules to application/javascript
//    automatically, supporting React, Vue, Svelte, and beyond with zero maintenance.
// =============================================================================
class XenonDevProxySource : public content::URLDataSource {
 public:
  XenonDevProxySource(content::BrowserContext* browser_context, std::string host)
      : browser_context_(browser_context), host_(std::move(host)) {}

  ~XenonDevProxySource() override {
    pending_requests_.clear();
  }

  XenonDevProxySource(const XenonDevProxySource&) = delete;
  XenonDevProxySource& operator=(const XenonDevProxySource&) = delete;

  // content::URLDataSource implementation:
  std::string GetSource() override { return host_; }

  std::string GetMimeType(const GURL& url) override {
    std::string path(url.path());
    
    // 1. HTML Documents
    if (path.empty() || path == "/" || path.ends_with(".html") || path.ends_with(".htm")) {
      return "text/html";
    }

    // 2. CSS Stylesheets (Direct vs JS-Compiled Modules)
    if (path.ends_with(".css")) {
      if (url.query().find("direct") != std::string::npos) {
        return "text/css";
      }
      if (path.find("/src/") != std::string::npos || path.find("/node_modules/") != std::string::npos) {
        return "application/javascript";
      }
      return "text/css";
    }

    // 3. Fallback to Chromium's native OS-based MIME utility for standard binary/static assets
    base::FilePath file_path = base::FilePath::FromUTF8Unsafe(path);
    std::string mime_type;
    if (net::GetMimeTypeFromFile(file_path, &mime_type)) {
      // If it's a known non-script asset type (images, fonts, media, config, wasm), use it.
      // Otherwise, let it fall through to be treated as a JS module.
      if (!mime_type.empty() && 
          (mime_type.starts_with("image/") || 
           mime_type.starts_with("font/") || 
           mime_type.starts_with("audio/") || 
           mime_type.starts_with("video/") || 
           mime_type == "application/wasm" || 
           mime_type == "application/json")) {
        return mime_type;
      }
    }

    // 4. Default: Every other file in a Vite/Webpack project is compiled to a JavaScript module
    return "application/javascript";
  }

  bool ShouldServiceRequest(const GURL& url,
                            content::BrowserContext* db,
                            int render_process_id) override {
    return url.SchemeIs(content::kChromeUIScheme) && url.host() == host_;
  }

  // Force Chromium WebUI loader factory to serialize and send resolved MIME types
  // as standard HTTP Content-Type headers, required for ES Module validation.
  bool ShouldServeMimeTypeAsContentTypeHeader() override {
    return true;
  }

  void StartDataRequest(
      const GURL& url,
      const content::WebContents::Getter& wc_getter,
      content::URLDataSource::GotDataCallback callback) override {
    std::string path(url.path());
    if (path.empty() || path == "/") {
      path = "/index.html";
    }
    
    GURL dev_server_url = ResolveDevServerUrl(path);

    auto resource_request = std::make_unique<network::ResourceRequest>();
    resource_request->url = dev_server_url;
    resource_request->credentials_mode = network::mojom::CredentialsMode::kOmit;

    net::NetworkTrafficAnnotationTag traffic_annotation =
        net::DefineNetworkTrafficAnnotation("xenon_dev_proxy", R"(
          semantics {
            sender: "Xenon WebUI Dev Proxy"
            description: "Proxies WebUI requests to local dev server for hot reloading."
            trigger: "Local developer loading xenon WebUI page."
            data: "None."
            destination: LOCAL
          }
          policy {
            cookies_allowed: NO
            setting: "This is a local development feature only."
            policy_exception_justification: "Not applicable."
          })");

    auto request_iter = pending_requests_.emplace(pending_requests_.begin());
    request_iter->callback = std::move(callback);
    request_iter->loader =
        network::SimpleURLLoader::Create(std::move(resource_request),
                                         traffic_annotation);

    auto* loader_ptr = request_iter->loader.get();
    loader_ptr->DownloadToStringOfUnboundedSizeUntilCrashAndDie(
        browser_context_->GetDefaultStoragePartition()
            ->GetURLLoaderFactoryForBrowserProcess()
            .get(),
        base::BindOnce(&XenonDevProxySource::OnDataLoaded,
                       base::Unretained(this), request_iter));
  }

  std::string GetContentSecurityPolicy(
      network::mojom::CSPDirectiveName directive) override {
    // Return empty CSP to allow loopback WebSockets (HMR) and external stylesheets in Dev mode.
    return "";
  }

 private:
  struct PendingRequest {
    PendingRequest() = default;
    ~PendingRequest() {
      if (callback) {
        std::move(callback).Run(base::MakeRefCounted<base::RefCountedString>(""));
      }
    }
    PendingRequest(const PendingRequest&) = delete;
    PendingRequest& operator=(const PendingRequest&) = delete;
    PendingRequest(PendingRequest&&) = default;
    PendingRequest& operator=(PendingRequest&&) = default;

    content::URLDataSource::GotDataCallback callback;
    std::unique_ptr<network::SimpleURLLoader> loader;
  };

  GURL ResolveDevServerUrl(const std::string& path) {
    std::string base_url = "http://127.0.0.1:3000";
    auto* command_line = base::CommandLine::ForCurrentProcess();
    if (command_line->HasSwitch("xenon-dev-server-url")) {
      base_url = command_line->GetSwitchValueASCII("xenon-dev-server-url");
    }
    
    if (base_url.ends_with("/") && path.starts_with("/")) {
      return GURL(base_url + path.substr(1));
    }
    return GURL(base_url + path);
  }

  void OnDataLoaded(std::list<PendingRequest>::iterator request_iter,
                    std::optional<std::string> response_body) {
    auto callback = std::move(request_iter->callback);
    pending_requests_.erase(request_iter);

    if (response_body) {
      scoped_refptr<base::RefCountedString> ref_response =
          base::MakeRefCounted<base::RefCountedString>(std::move(*response_body));
      std::move(callback).Run(ref_response);
    } else {
      std::move(callback).Run(base::MakeRefCounted<base::RefCountedString>(""));
    }
  }

  raw_ptr<content::BrowserContext> browser_context_;
  std::string host_;
  std::list<PendingRequest> pending_requests_;
};

// =============================================================================
// --- Message Handlers & Interface Registration ---
// =============================================================================
namespace {

constexpr char kLoginMessageDone[] = "xenonLoginDone";
constexpr char kLoginMessageLogoutTest[] = "xenonLoginLogoutTest";
constexpr char kLoginMessageClose[] = "xenonLoginClose";

// `WebUIBrowserInterfaceBrokerRegistry::ForWebUI` must run at most once per
// controller type. Lazily register when the first xenon WebUI page is created.
void EnsureTrustedBrokerKnowsPageHandler() {
  static std::once_flag once;
  std::call_once(once, [] {
    content::WebUIBrowserInterfaceBrokerRegistry::GetTrustedRegistry()
        .ForWebUI<XenonWebUIController>()
        .Add<mojom::PageHandler>();
  });
}

// Login page uses chrome.send only (no Mojo in JS). Mirrors XenonPageHandler
// Close / SetAppSessionLoggedIn.
class XenonLoginWebUIMessageHandler : public content::WebUIMessageHandler {
 public:
  XenonLoginWebUIMessageHandler() = default;
  XenonLoginWebUIMessageHandler(const XenonLoginWebUIMessageHandler&) = delete;
  XenonLoginWebUIMessageHandler& operator=(const XenonLoginWebUIMessageHandler&) =
      delete;
  ~XenonLoginWebUIMessageHandler() override = default;

 private:
  void RegisterMessages() override {
    web_ui()->RegisterMessageCallback(
        kLoginMessageDone,
        base::BindRepeating(&XenonLoginWebUIMessageHandler::HandleLoginDone,
                            base::Unretained(this)));
    web_ui()->RegisterMessageCallback(
        kLoginMessageLogoutTest,
        base::BindRepeating(&XenonLoginWebUIMessageHandler::HandleLogoutTest,
                            base::Unretained(this)));
    web_ui()->RegisterMessageCallback(
        kLoginMessageClose,
        base::BindRepeating(&XenonLoginWebUIMessageHandler::HandleClose,
                            base::Unretained(this)));
  }

  void HandleLoginDone(const base::Value::List& args) {
    Profile* profile = Profile::FromWebUI(web_ui());
    if (profile) {
      XenonLoginController::GetInstance()->SetAppSessionLoggedIn(profile, true);
    }
  }

  void HandleLogoutTest(const base::Value::List& args) {
    Profile* profile = Profile::FromWebUI(web_ui());
    if (profile) {
      XenonLoginController::GetInstance()->SetAppSessionLoggedIn(profile, false);
    }
  }

  void HandleClose(const base::Value::List& args) {
    content::WebContents* contents = web_ui()->GetWebContents();
    if (!contents) {
      return;
    }
    if (content::WebContentsDelegate* delegate = contents->GetDelegate()) {
      delegate->CloseContents(contents);
    }
  }
};

}  // namespace

// =============================================================================
// --- XenonWebUIController Implementation ---
// =============================================================================
XenonWebUIController::XenonWebUIController(content::WebUI* web_ui,
                                           std::string webui_host)
    : ui::MojoWebUIController(web_ui, webui_host == kXenonLoginWebUIHost),
      webui_host_(std::move(webui_host)) {
  EnsureTrustedBrokerKnowsPageHandler();

  // Bind the transparent loopback proxy for login frontend development
  // only if the development server URL switch is explicitly configured on the command line.
  if (webui_host_ == kXenonLoginWebUIHost &&
      base::CommandLine::ForCurrentProcess()->HasSwitch("xenon-dev-server-url")) {
    content::URLDataSource::Add(
        web_ui->GetWebContents()->GetBrowserContext(),
        std::make_unique<XenonDevProxySource>(
            web_ui->GetWebContents()->GetBrowserContext(), webui_host_));
    web_ui->AddMessageHandler(std::make_unique<XenonLoginWebUIMessageHandler>());
    return;
  }

  // In Production / Release builds, load compiled assets directly from pak resources
  content::WebUIDataSource* source = content::WebUIDataSource::CreateAndAdd(
      web_ui->GetWebContents()->GetBrowserContext(), webui_host_);

  if (webui_host_ == kXenonLoginWebUIHost) {
    web_ui->AddMessageHandler(std::make_unique<XenonLoginWebUIMessageHandler>());
    source->AddResourcePath("login.css", IDR_XENON_LOGIN_CSS);
    source->AddResourcePath("login.js", IDR_XENON_LOGIN_JS);
    source->SetDefaultResource(IDR_XENON_LOGIN_HTML);
  } else {
    source->AddResourcePath("xenon.mojom-webui.js",
                            IDR_XENON_WEBUI_XENON_MOJOM_WEBUI_JS);
    source->AddResourcePath("index.css", IDR_XENON_WEBUI_INDEX_CSS);
    source->AddResourcePath("index.js", IDR_XENON_WEBUI_INDEX_JS);
    source->SetDefaultResource(IDR_XENON_WEBUI_INDEX_HTML);
  }
}

XenonWebUIController::~XenonWebUIController() = default;

void XenonWebUIController::BindInterface(
    mojo::PendingReceiver<mojom::PageHandler> receiver) {
  page_handler_ =
      std::make_unique<XenonPageHandler>(std::move(receiver), web_ui());
}

WEB_UI_CONTROLLER_TYPE_IMPL(XenonWebUIController)

// =============================================================================
// --- WebUI Configurations ---
// =============================================================================
XenonWebUIConfig::XenonWebUIConfig()
    : content::WebUIConfig(content::kChromeUIScheme, kXenonOverlayWebUIHost) {}

XenonWebUIConfig::~XenonWebUIConfig() = default;

std::unique_ptr<content::WebUIController> XenonWebUIConfig::CreateWebUIController(
    content::WebUI* web_ui,
    const GURL& url) {
  return std::make_unique<XenonWebUIController>(
      web_ui, std::string(kXenonOverlayWebUIHost));
}

XenonLoginWebUIConfig::XenonLoginWebUIConfig()
    : content::WebUIConfig(content::kChromeUIScheme, kXenonLoginWebUIHost) {}

XenonLoginWebUIConfig::~XenonLoginWebUIConfig() = default;

std::unique_ptr<content::WebUIController>
XenonLoginWebUIConfig::CreateWebUIController(content::WebUI* web_ui,
                                             const GURL& url) {
  return std::make_unique<XenonWebUIController>(
      web_ui, std::string(kXenonLoginWebUIHost));
}

}  // namespace xenon
