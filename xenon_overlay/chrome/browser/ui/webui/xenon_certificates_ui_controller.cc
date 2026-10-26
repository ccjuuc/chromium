// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ui/webui/xenon_certificates_ui_controller.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/i18n/time_formatting.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_view_util.h"
#include "base/time/time.h"
#include "base/values.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_data_source.h"
#include "content/public/browser/web_ui_message_handler.h"
#include "content/public/common/url_constants.h"
#include "net/base/load_flags.h"
#include "net/base/net_errors.h"
#include "net/cert/cert_status_flags.h"
#include "net/cert/x509_certificate.h"
#include "net/http/http_response_headers.h"
#include "net/ssl/ssl_info.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/cpp/url_loader_completion_status.h"
#include "services/network/public/mojom/fetch_api.mojom.h"
#include "services/network/public/mojom/url_loader_factory.mojom.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "url/gurl.h"
#include "url/url_constants.h"
#include "xenon_overlay/chrome/browser/managed_certificates/managed_certificate_service.h"
#include "xenon_overlay/chrome/browser/managed_certificates/managed_certificate_types.h"
#include "xenon_overlay/resources/grit/xenon_resources.h"

namespace xenon {
namespace {

constexpr char kCertificatesHost[] = "xenon-certificates";
constexpr size_t kMaxTestUrlLength = 2048;

base::Value StatusTime(base::Time time) {
  return time.is_null() ? base::Value()
                        : base::Value(base::TimeFormatAsIso8601(time));
}

base::DictValue ProbeResult(const std::string& url, int net_error) {
  base::DictValue result;
  result.Set("url", url);
  result.Set("netError", net_error);
  result.Set("netErrorName", net::ErrorToString(net_error));
  result.Set("httpStatus", base::Value());
  result.Set("tlsPassed", false);
  return result;
}

base::DictValue CertificateDetails(const net::SSLInfo& ssl_info) {
  const auto& cert = ssl_info.cert;
  base::DictValue result;
  result.Set("subject", cert->subject().GetDisplayName());
  result.Set("issuer", cert->issuer().GetDisplayName());
  result.Set("validFrom", base::TimeFormatAsIso8601(cert->valid_start()));
  result.Set("validTo", base::TimeFormatAsIso8601(cert->valid_expiry()));
  result.Set("sha256",
             base::HexEncode(net::X509Certificate::CalculateFingerprint256(
                 cert->cert_buffer())));
  result.Set("knownRoot", ssl_info.is_issued_by_known_root);
  result.Set("certStatus", static_cast<int>(ssl_info.cert_status));
  return result;
}

class XenonCertificatesMessageHandler : public content::WebUIMessageHandler {
 public:
  XenonCertificatesMessageHandler() = default;
  ~XenonCertificatesMessageHandler() override = default;

 private:
  void RegisterMessages() override {
    web_ui()->RegisterMessageCallback(
        "getManagedCertificateStatus",
        base::BindRepeating(&XenonCertificatesMessageHandler::GetStatus,
                            base::Unretained(this)));
    web_ui()->RegisterMessageCallback(
        "testManagedCertificateUrl",
        base::BindRepeating(&XenonCertificatesMessageHandler::TestUrl,
                            base::Unretained(this)));
  }

  void OnJavascriptDisallowed() override {
    loader_.reset();
    weak_factory_.InvalidateWeakPtrs();
  }

  void GetStatus(const base::ListValue& args) {
    if (args.empty() || !args[0].is_string()) {
      return;
    }
    AllowJavascript();
    auto* service = certificates::XenonManagedCertificateService::Get();
    const auto status = service->GetStatus();
    base::DictValue result;
    result.Set("started", status.started);
    result.Set("enabled", status.enabled);
    result.Set("ready", status.ready);
    result.Set("downloading", status.downloading);
    result.Set("sourceId", status.source_id);
    result.Set("configurationError", status.configuration_error);
    result.Set("refreshError", status.refresh_error);
    result.Set("lastAttempt", StatusTime(status.last_attempt));
    result.Set("lastSuccess", StatusTime(status.last_success));
    result.Set("snapshot", base::Value());
    if (const auto* snapshot = service->snapshot()) {
      base::DictValue details;
      details.Set("sourceId", snapshot->source_id);
      // Preserve the full uint64 generation in JavaScript.
      details.Set("generation", base::NumberToString(snapshot->generation));
      details.Set("issuedAt", base::TimeFormatAsIso8601(snapshot->issued_at));
      base::ListValue certs;
      for (const auto& cert : snapshot->certificates) {
        base::DictValue entry;
        entry.Set("name", cert.name);
        entry.Set("role", cert.trust_anchor ? "trust_anchor" : "intermediate");
        entry.Set("sha256", certificates::Sha256Hex(
                                base::as_string_view(base::span(cert.der))));
        base::ListValue dns;
        for (const auto& name : cert.permitted_dns_names) {
          dns.Append(name);
        }
        entry.Set("dns", std::move(dns));
        base::ListValue cidrs;
        for (const auto& cidr : cert.permitted_cidrs) {
          cidrs.Append(cidr.address.ToString() + "/" +
                       base::NumberToString(cidr.prefix_length));
        }
        entry.Set("cidrs", std::move(cidrs));
        certs.Append(std::move(entry));
      }
      details.Set("certificates", std::move(certs));
      result.Set("snapshot", std::move(details));
    }
    ResolveJavascriptCallback(args[0], result);
  }

  void TestUrl(const base::ListValue& args) {
    if (args.empty() || !args[0].is_string()) {
      return;
    }
    AllowJavascript();
    if (args.size() != 2 || !args[1].is_string()) {
      ResolveJavascriptCallback(args[0],
                                ProbeResult("", net::ERR_INVALID_ARGUMENT));
      return;
    }
    const std::string& input = args[1].GetString();
    if (input.size() > kMaxTestUrlLength) {
      ResolveJavascriptCallback(args[0], ProbeResult("", net::ERR_INVALID_URL));
      return;
    }
    const GURL url(input);
    if (!url.is_valid() || !url.SchemeIs(url::kHttpsScheme) ||
        url.has_username() || url.has_password() || url.has_ref()) {
      ResolveJavascriptCallback(args[0],
                                ProbeResult(input, net::ERR_INVALID_URL));
      return;
    }
    if (loader_) {
      ResolveJavascriptCallback(args[0],
                                ProbeResult(url.spec(), net::ERR_IO_PENDING));
      return;
    }

    constexpr auto annotation = net::DefineNetworkTrafficAnnotation(
        "xenon_managed_certificate_url_test", R"(
          semantics {
            sender: "Xenon certificate test page"
            description: "Checks HTTPS certificate verification for a URL entered by the user."
            trigger: "User clicks Test on chrome://xenon-certificates."
            data: "The user-entered HTTPS URL. No cookies or stored credentials."
            destination: OTHER
            destination_other: "The HTTPS destination selected by the user."
          }
          policy {
            cookies_allowed: NO
            setting: "Requests are made only when the user clicks Test."
            policy_exception_justification: "Diagnostic request initiated by the user."
          })");
    auto request = std::make_unique<network::ResourceRequest>();
    request->url = url;
    request->method = "HEAD";
    request->credentials_mode = network::mojom::CredentialsMode::kOmit;
    request->do_not_prompt_for_login = true;
    request->redirect_mode = network::mojom::RedirectMode::kError;
    request->load_flags = net::LOAD_BYPASS_CACHE | net::LOAD_DISABLE_CACHE |
                          net::LOAD_DO_NOT_USE_EMBEDDED_IDENTITY;
    loader_ = network::SimpleURLLoader::Create(std::move(request), annotation);
    loader_->SetAllowHttpErrorResults(true);
    loader_->SetTimeoutDuration(base::Seconds(15));
    loader_->SetURLLoaderFactoryOptions(
        network::mojom::kURLLoadOptionSendSSLInfoWithResponse |
        network::mojom::kURLLoadOptionSendSSLInfoForCertificateError);
    loader_->SetOnRedirectCallback(
        base::BindRepeating(&XenonCertificatesMessageHandler::OnTestRedirect,
                            weak_factory_.GetWeakPtr(), args[0].GetString()));
    // Use this page's profile/partition and its ordinary verifier. The
    // service's separate bootstrap context would test a different trust set.
    auto factory = web_ui()
                       ->GetWebContents()
                       ->GetPrimaryMainFrame()
                       ->GetStoragePartition()
                       ->GetURLLoaderFactoryForBrowserProcess();
    loader_->DownloadHeadersOnly(
        factory.get(),
        base::BindOnce(&XenonCertificatesMessageHandler::OnTestComplete,
                       weak_factory_.GetWeakPtr(), args[0].GetString(),
                       url.spec()));
  }

  void OnTestRedirect(std::string callback_id,
                      const GURL& url_before_redirect,
                      const net::RedirectInfo&,
                      const network::mojom::URLResponseHead& response,
                      std::vector<std::string>*) {
    // The first response already contains the TLS verification result. Finish
    // here and destroy the loader before it can follow the redirect.
    FinishTest(std::move(callback_id), url_before_redirect.spec(), net::OK,
               response.headers.get(),
               response.ssl_info ? &*response.ssl_info : nullptr);
  }

  void OnTestComplete(std::string callback_id,
                      std::string url,
                      scoped_refptr<net::HttpResponseHeaders> headers) {
    const auto* response = loader_->ResponseInfo();
    const net::SSLInfo* ssl_info =
        response && response->ssl_info ? &*response->ssl_info : nullptr;
    const auto& completion = loader_->CompletionStatus();
    if (!ssl_info && completion && completion->ssl_info) {
      ssl_info = &*completion->ssl_info;
    }
    FinishTest(std::move(callback_id), std::move(url), loader_->NetError(),
               headers.get(), ssl_info);
  }

  void FinishTest(std::string callback_id,
                  std::string url,
                  int net_error,
                  const net::HttpResponseHeaders* headers,
                  const net::SSLInfo* ssl_info) {
    base::DictValue result = ProbeResult(url, net_error);
    if (headers) {
      result.Set("httpStatus", headers->response_code());
    }
    if (ssl_info && ssl_info->is_valid()) {
      result.Set("certificate", CertificateDetails(*ssl_info));
      // An HTTP error is distinct from certificate verification. Requiring a
      // response also avoids reporting a partial TLS handshake as a success.
      result.Set("tlsPassed",
                 headers && !net::IsCertStatusError(ssl_info->cert_status));
    }
    loader_.reset();
    ResolveJavascriptCallback(base::Value(callback_id), result);
  }

  std::unique_ptr<network::SimpleURLLoader> loader_;
  base::WeakPtrFactory<XenonCertificatesMessageHandler> weak_factory_{this};
};

}  // namespace

XenonCertificatesUIController::XenonCertificatesUIController(
    content::WebUI* web_ui)
    : content::WebUIController(web_ui) {
  auto* source = content::WebUIDataSource::CreateAndAdd(
      web_ui->GetWebContents()->GetBrowserContext(), kCertificatesHost);
  source->SetDefaultResource(IDR_XENON_CERTIFICATES_HTML);
  source->AddResourcePath("index.css", IDR_XENON_CERTIFICATES_CSS);
  source->AddResourcePath("index.js", IDR_XENON_CERTIFICATES_JS);
  web_ui->AddMessageHandler(
      std::make_unique<XenonCertificatesMessageHandler>());
}

XenonCertificatesUIController::~XenonCertificatesUIController() = default;

XenonCertificatesUIConfig::XenonCertificatesUIConfig()
    : content::WebUIConfig(content::kChromeUIScheme, kCertificatesHost) {}

XenonCertificatesUIConfig::~XenonCertificatesUIConfig() = default;

std::unique_ptr<content::WebUIController>
XenonCertificatesUIConfig::CreateWebUIController(content::WebUI* web_ui,
                                                 const GURL& url) {
  return std::make_unique<XenonCertificatesUIController>(web_ui);
}

}  // namespace xenon
