// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_network_request_bridge.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "base/base64.h"
#include "base/functional/bind.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/time/time.h"
#include "net/base/load_flags.h"
#include "net/base/net_errors.h"
#include "net/http/http_response_headers.h"
#include "net/http/http_util.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/url_response_head.mojom.h"

namespace xenon::ipc {

namespace {

xenon::ipc::mojom::IpcResultPtr Success(base::Value value) {
  auto result = xenon::ipc::mojom::IpcResult::New();
  result->success = true;
  result->value = std::move(value);
  return result;
}

xenon::ipc::mojom::IpcResultPtr Failure(const std::string& error) {
  auto result = xenon::ipc::mojom::IpcResult::New();
  result->success = false;
  result->error = error;
  return result;
}

bool IsManagedRequestHeader(std::string_view name) {
  return base::EqualsCaseInsensitiveASCII(name, "host") ||
         base::EqualsCaseInsensitiveASCII(name, "content-length") ||
         base::EqualsCaseInsensitiveASCII(name, "connection") ||
         base::EqualsCaseInsensitiveASCII(name, "proxy-connection") ||
         base::EqualsCaseInsensitiveASCII(name, "transfer-encoding");
}

struct RequestState {
  std::unique_ptr<network::SimpleURLLoader> loader;
  NetworkRequestCallback callback;

  void Cancel(const std::string& message) {
    loader.reset();
    if (callback) {
      std::move(callback).Run(Failure(message));
    }
  }
};

}  // namespace

base::OnceClosure PerformNetworkRequest(
    scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory,
    base::Value arguments,
    NetworkRequestCallback callback) {
  if (!url_loader_factory || !arguments.is_list() ||
      arguments.GetList().empty() ||
      !arguments.GetList().front().is_dict()) {
    std::move(callback).Run(Failure("EINVAL: invalid network request"));
    return {};
  }

  const base::DictValue& request = arguments.GetList().front().GetDict();
  const std::string* url_string = request.FindString("url");
  const GURL url(url_string ? *url_string : std::string());
  if (!url.is_valid() || !url.SchemeIsHTTPOrHTTPS()) {
    std::move(callback).Run(
        Failure("EINVAL: network request requires an HTTP(S) URL"));
    return {};
  }

  auto resource_request = std::make_unique<network::ResourceRequest>();
  resource_request->url = url;
  const std::string* method = request.FindString("method");
  resource_request->method = method ? *method : "GET";
  if (resource_request->method.empty()) {
    resource_request->method = "GET";
  }
  if (!net::HttpUtil::IsValidHeaderName(resource_request->method)) {
    std::move(callback).Run(Failure("EINVAL: invalid HTTP method"));
    return {};
  }
  const bool use_session_cookies =
      request.FindBool("useSessionCookies").value_or(false);
  resource_request->credentials_mode =
      use_session_cookies ? network::mojom::CredentialsMode::kInclude
                          : network::mojom::CredentialsMode::kOmit;

  // Node.js http/https modules do not route through system proxy. Bypass
  // browser proxy settings to match real Node networking semantics and avoid
  // ERR_PROXY_CONNECTION_FAILED in hosted Electron applications.
  resource_request->load_flags |= net::LOAD_BYPASS_PROXY;

  if (const base::DictValue* headers = request.FindDict("headers")) {
    for (const auto [name, value] : *headers) {
      if (!value.is_string() || IsManagedRequestHeader(name) ||
          !net::HttpUtil::IsValidHeaderName(name) ||
          !net::HttpUtil::IsValidHeaderValue(value.GetString())) {
        continue;
      }
      resource_request->headers.SetHeader(name, value.GetString());
    }
  }

  std::string body;
  constexpr size_t kMaxUploadBytes = 32 * 1024 * 1024;
  if (const std::string* encoded_body = request.FindString("bodyBase64");
      encoded_body && encoded_body->size() > ((kMaxUploadBytes + 2) / 3) * 4) {
    std::move(callback).Run(
        Failure("ERR_BUFFER_TOO_LARGE: HTTP upload exceeds 32 MiB"));
    return {};
  }
  if (const std::string* encoded_body = request.FindString("bodyBase64");
      encoded_body && !base::Base64Decode(*encoded_body, &body)) {
    std::move(callback).Run(
        Failure("EINVAL: network request body is not valid base64"));
    return {};
  }
  if (body.size() > kMaxUploadBytes) {
    std::move(callback).Run(
        Failure("ERR_BUFFER_TOO_LARGE: HTTP upload exceeds 32 MiB"));
    return {};
  }

  static const net::NetworkTrafficAnnotationTag kTrafficAnnotation =
      net::DefineNetworkTrafficAnnotation("xenon_electron_network_request", R"(
        semantics {
          sender: "Xenon Electron-compatible network bridge"
          description:
            "Sends an HTTP(S) request initiated through a hosted Electron or Node networking API."
          trigger:
            "A trusted hosted application calls a Node HTTP(S) networking API."
          data:
            "The URL, headers, and body supplied by the hosted application."
          destination: OTHER
          destination_other:
            "The destination selected by the hosted application."
        }
        policy {
          cookies_allowed: YES
          cookies_store: "The active browser profile, only when the request explicitly enables session cookies."
          setting:
            "This feature is available only to explicitly allowed hosted Electron application origins."
          policy_exception_justification:
            "Not implemented; this is an application runtime capability."
        })");

  std::unique_ptr<network::SimpleURLLoader> loader =
      network::SimpleURLLoader::Create(std::move(resource_request),
                                       kTrafficAnnotation);
  loader->SetAllowHttpErrorResults(true);
  const int timeout_ms = request.FindInt("timeoutMs").value_or(30000);
  if (timeout_ms <= 0 || timeout_ms > 300000) {
    std::move(callback).Run(
        Failure("EINVAL: HTTP timeout must be 1..300000 ms"));
    return {};
  }
  loader->SetTimeoutDuration(base::Milliseconds(timeout_ms));
  if (!body.empty()) {
    loader->AttachStringForUpload(std::move(body));
  }

  auto state = std::make_shared<RequestState>();
  state->loader = std::move(loader);
  state->callback = std::move(callback);
  if (const std::string* redirect = request.FindString("redirect");
      redirect && *redirect == "error") {
    state->loader->SetOnRedirectCallback(base::BindRepeating(
        [](std::weak_ptr<RequestState> weak, const GURL&,
           const net::RedirectInfo&, const network::mojom::URLResponseHead&,
           std::vector<std::string>*) {
          if (auto state = weak.lock()) {
            state->Cancel("ERR_HTTP_REDIRECT: HTTP redirect was rejected");
          }
        },
        std::weak_ptr<RequestState>(state)));
  }
  network::SimpleURLLoader* loader_ptr = state->loader.get();
  loader_ptr->DownloadToString(
      url_loader_factory.get(),
      base::BindOnce(
          [](std::shared_ptr<RequestState> state,
             std::optional<std::string> response_body) {
            auto loader = std::move(state->loader);
            auto callback = std::move(state->callback);
            const int net_error = loader->NetError();
            const network::mojom::URLResponseHead* response_info =
                loader->ResponseInfo();
            if (!response_body || net_error != net::OK) {
              std::move(callback).Run(Failure(
                  "Network request failed: " + net::ErrorToString(net_error)));
              return;
            }

            base::DictValue response;
            response.Set("statusCode", 0);
            response.Set("statusMessage", "");
            response.Set("finalUrl", loader->GetFinalURL().spec());
            base::DictValue response_headers;
            if (response_info && response_info->headers) {
              response.Set("statusCode",
                           response_info->headers->response_code());
              response.Set("statusMessage",
                           response_info->headers->GetStatusText());
              const auto version = response_info->headers->GetHttpVersion();
              response.Set("httpVersion",
                           base::NumberToString(version.major_value()) + "." +
                               base::NumberToString(version.minor_value()));
              size_t iterator = 0;
              std::string name;
              std::string value;
              while (response_info->headers->EnumerateHeaderLines(
                  &iterator, &name, &value)) {
                if (const std::string* existing =
                        response_headers.FindString(name)) {
                  response_headers.Set(name, *existing + ", " + value);
                } else {
                  response_headers.Set(name, value);
                }
              }
            }
            response.Set("headers", std::move(response_headers));
            std::string body_to_encode =
                response_body ? *response_body : std::string();
            response.Set("bodyBase64", base::Base64Encode(body_to_encode));
            std::move(callback).Run(Success(base::Value(std::move(response))));
          },
          state),
      network::SimpleURLLoader::kMaxBoundedStringDownloadSize);
  return base::BindOnce(
      [](std::weak_ptr<RequestState> weak) {
        if (auto state = weak.lock()) {
          state->Cancel("ABORT_ERR: HTTP request was aborted");
        }
      },
      std::weak_ptr<RequestState>(state));
}

}  // namespace xenon::ipc
