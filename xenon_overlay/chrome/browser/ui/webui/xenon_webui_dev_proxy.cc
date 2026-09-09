// Copyright 2026 The Xenon Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ui/webui/xenon_webui_dev_proxy.h"

#include <list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/memory/ref_counted_memory.h"
#include "base/memory/scoped_refptr.h"
#include "content/browser/webui/url_data_source_impl.h"
#include "content/browser/webui/url_data_manager_backend.h"
#include "content/browser/webui/web_ui_data_source_impl.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/url_data_source.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_data_source.h"
#include "content/public/common/url_constants.h"
#include "net/base/load_flags.h"
#include "net/base/mime_util.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/content_security_policy.mojom.h"
#include "xenon_overlay/chrome/browser/ui/webui/xenon_webui_controller.h"

namespace xenon {

namespace {

class XenonDevProxySource : public content::URLDataSource {
 public:
  XenonDevProxySource(content::BrowserContext* browser_context,
                      std::string host,
                      std::string dev_url,
                      scoped_refptr<content::URLDataSourceImpl> original_source_impl)
      : browser_context_(browser_context),
        host_(std::move(host)),
        dev_url_(std::move(dev_url)),
        original_source_impl_(original_source_impl) {
    if (!dev_url_.empty() && dev_url_.back() != '/') {
      dev_url_ += '/';
    }
  }

  ~XenonDevProxySource() override { pending_requests_.clear(); }

  XenonDevProxySource(const XenonDevProxySource&) = delete;
  XenonDevProxySource& operator=(const XenonDevProxySource&) = delete;

  std::string GetSource() override { return host_; }

  std::string GetMimeType(const GURL& url) override {
    const std::string path(url.path());
    const std::string query(url.query());

    if (path.empty() || path == "/" || path.ends_with(".html") ||
        path.ends_with(".htm")) {
      return "text/html";
    }

    if (query.find("import") != std::string::npos ||
        query.find("raw") != std::string::npos ||
        query.find("worker") != std::string::npos) {
      return "application/javascript";
    }

    if (path.find("@vite/") != std::string::npos ||
        path.find("@react-refresh") != std::string::npos ||
        path.find("/@") != std::string::npos) {
      return "application/javascript";
    }

    if (path.ends_with(".js") || path.ends_with(".mjs") ||
        path.ends_with(".cjs") || path.ends_with(".ts") ||
        path.ends_with(".tsx") || path.ends_with(".jsx") ||
        path.ends_with(".vue") || path.ends_with(".svelte")) {
      return "application/javascript";
    }

    if (path.ends_with(".css") || path.ends_with(".scss") ||
        path.ends_with(".sass") || path.ends_with(".less") ||
        path.ends_with(".styl") || path.ends_with(".stylus") ||
        path.ends_with(".pcss") || path.ends_with(".postcss")) {
      if (query.find("direct") != std::string::npos ||
          query.find("used") != std::string::npos) {
        return "text/css";
      }
      if (path.find("/src/") != std::string::npos ||
          path.find("/node_modules/") != std::string::npos) {
        return "application/javascript";
      }
      return "text/css";
    }

    base::FilePath file_path = base::FilePath::FromUTF8Unsafe(path);
    std::string mime_type;
    if (net::GetMimeTypeFromFile(file_path, &mime_type) && !mime_type.empty()) {
      return mime_type;
    }

    if (path.ends_with(".avif")) return "image/avif";
    if (path.ends_with(".apng")) return "image/apng";
    if (path.ends_with(".cur")) return "image/x-icon";
    if (path.ends_with(".flac")) return "audio/flac";
    if (path.ends_with(".opus")) return "audio/opus";
    if (path.ends_with(".m4a")) return "audio/mp4";
    if (path.ends_with(".jsonc") || path.ends_with(".json5")) {
      return "application/json";
    }
    if (path.ends_with(".yaml") || path.ends_with(".yml")) return "text/yaml";
    if (path.ends_with(".toml")) return "text/plain";
    if (path.ends_with(".md") || path.ends_with(".markdown")) {
      return "text/markdown";
    }
    if (path.ends_with(".map")) return "application/json";
    if (path.ends_with(".webmanifest")) return "application/manifest+json";
    if (path.ends_with(".br")) return "application/x-brotli";

    const std::string request_path =
        content::URLDataSource::URLToRequestPath(url);
    if (!ShouldProxyRequest(request_path)) {
      if (original_source_impl_) {
        return original_source_impl_->source()->GetMimeType(url);
      }
    }

    return "application/javascript";
  }

  bool ShouldServiceRequest(const GURL& url,
                            content::BrowserContext* browser_context,
                            int render_process_id) override {
    return url.SchemeIs(content::kChromeUIScheme) && url.host() == host_;
  }

  bool ShouldServeMimeTypeAsContentTypeHeader() override { return true; }

  bool ShouldReplaceI18nInJS() override {
    return original_source_impl_ &&
           original_source_impl_->source()->ShouldReplaceI18nInJS();
  }

  void StartDataRequest(const GURL& url,
                        const content::WebContents::Getter& wc_getter,
                        GotDataCallback callback) override {
    const std::string request_path =
        content::URLDataSource::URLToRequestPath(url);

    if (!ShouldProxyRequest(request_path)) {
      if (original_source_impl_) {
        original_source_impl_->source()->StartDataRequest(url, wc_getter,
                                                          std::move(callback));
      } else {
        std::move(callback).Run(nullptr);
      }
      return;
    }

    std::string path(url.path());
    if (path.empty() || path == "/") {
      path = "/index.html";
    }

    const std::string query(url.query());
    const std::string path_and_query =
        query.empty() ? path : path + "?" + query;

    const GURL target_url = ResolveDevServerUrl(path_and_query);
    if (!target_url.is_valid()) {
      std::move(callback).Run(nullptr);
      return;
    }

    StartProxyRequest(target_url, std::move(callback));
  }

  std::string GetContentSecurityPolicy(
      network::mojom::CSPDirectiveName directive) override {
    return "";
  }

  bool AllowCaching() override { return false; }

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

    GotDataCallback callback;
    std::unique_ptr<network::SimpleURLLoader> loader;
  };

  static std::string PathWithoutQuery(std::string_view path) {
    const size_t query_pos = path.find('?');
    if (query_pos == std::string_view::npos) {
      return std::string(path);
    }
    return std::string(path.substr(0, query_pos));
  }

  bool ShouldProxyRequest(std::string_view path) {
    const std::string path_only = PathWithoutQuery(path);
    if (path_only == "strings.js" || path_only == "strings.m.js" ||
        path_only.find(".mojom-webui.js") != std::string::npos) {
      return false;
    }
    return true;
  }

  GURL ResolveDevServerUrl(const std::string& path_and_query) {
    if (dev_url_.ends_with("/") && path_and_query.starts_with("/")) {
      return GURL(dev_url_ + path_and_query.substr(1));
    }
    return GURL(dev_url_ + path_and_query);
  }

  void StartProxyRequest(const GURL& url, GotDataCallback callback) {
    auto traffic_annotation = net::DefineNetworkTrafficAnnotation(
        "xenon_dev_proxy", R"(
          semantics {
            sender: "Xenon WebUI Dev Proxy"
            description: "Proxies WebUI requests to a local front-end development server."
            trigger: "Only when a --[host]-dev-url switch is specified."
            data: "None"
            destination: LOCAL
          }
          policy {
            cookies_allowed: NO
            setting: "This is a developer-only feature and is disabled by default."
          })");

    auto request = std::make_unique<network::ResourceRequest>();
    request->url = url;
    request->credentials_mode = network::mojom::CredentialsMode::kOmit;
    request->load_flags = net::LOAD_BYPASS_CACHE | net::LOAD_DISABLE_CACHE;

    scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory =
        browser_context_->GetDefaultStoragePartition()
            ->GetURLLoaderFactoryForBrowserProcess();

    auto request_iter = pending_requests_.emplace(pending_requests_.end());
    request_iter->callback = std::move(callback);
    request_iter->loader =
        network::SimpleURLLoader::Create(std::move(request), traffic_annotation);

    request_iter->loader->DownloadToStringOfUnboundedSizeUntilCrashAndDie(
        url_loader_factory.get(),
        base::BindOnce(&XenonDevProxySource::OnProxyLoadComplete,
                       base::Unretained(this), request_iter));
  }

  void OnProxyLoadComplete(std::list<PendingRequest>::iterator request_iter,
                           std::optional<std::string> response_body) {
    GotDataCallback callback = std::move(request_iter->callback);
    pending_requests_.erase(request_iter);

    if (response_body) {
      std::move(callback).Run(base::MakeRefCounted<base::RefCountedString>(
          std::move(*response_body)));
    } else {
      std::move(callback).Run(base::MakeRefCounted<base::RefCountedString>(""));
    }
  }

  raw_ptr<content::BrowserContext> browser_context_;
  std::string host_;
  std::string dev_url_;
  scoped_refptr<content::URLDataSourceImpl> original_source_impl_;
  std::list<PendingRequest> pending_requests_;
};

// Chromium 142 stores template replacements on URLDataSourceImpl rather than
// URLDataSource. Preserve original WebUI replacements while serving proxy data.
class XenonDevProxySourceImpl : public content::URLDataSourceImpl {
 public:
  XenonDevProxySourceImpl(
      std::string source_name,
      std::unique_ptr<content::URLDataSource> source,
      scoped_refptr<content::URLDataSourceImpl> original_source_impl)
      : content::URLDataSourceImpl(std::move(source_name), std::move(source)),
        original_source_impl_(std::move(original_source_impl)) {}

  const ui::TemplateReplacements* GetReplacements() const override {
    return original_source_impl_
               ? original_source_impl_->GetReplacements()
               : nullptr;
  }

 protected:
  ~XenonDevProxySourceImpl() override = default;

 private:
  scoped_refptr<content::URLDataSourceImpl> original_source_impl_;
};

}  // namespace

bool TrySetupXenonWebuiDevProxy(content::WebUI* web_ui,
                                content::WebUIDataSource* source,
                                const std::string& webui_host) {
  const base::CommandLine* cl = base::CommandLine::ForCurrentProcess();
  std::string dev_url;

  const std::string per_host_switch = webui_host + "-dev-url";
  if (cl->HasSwitch(per_host_switch)) {
    dev_url = cl->GetSwitchValueASCII(per_host_switch);
  } else if (webui_host == kXenonLoginWebUIHost &&
             cl->HasSwitch("xenon-dev-server-url")) {
    dev_url = cl->GetSwitchValueASCII("xenon-dev-server-url");
  } else {
    return false;
  }

  if (dev_url.empty()) {
    return false;
  }

  auto* browser_context = web_ui->GetWebContents()->GetBrowserContext();
  auto* impl = static_cast<content::WebUIDataSourceImpl*>(source);
  impl->EnsureLoadTimeDataDefaultsAdded();

  auto original_source_impl =
      scoped_refptr<content::URLDataSourceImpl>(impl);
  auto proxy_source = std::make_unique<XenonDevProxySource>(
      browser_context, webui_host, dev_url, original_source_impl);
  auto proxy_source_impl = base::MakeRefCounted<XenonDevProxySourceImpl>(
      webui_host, std::move(proxy_source), std::move(original_source_impl));
  content::URLDataManagerBackend::GetForBrowserContext(browser_context)
      ->AddDataSource(proxy_source_impl.get());

  return true;
}

}  // namespace xenon
