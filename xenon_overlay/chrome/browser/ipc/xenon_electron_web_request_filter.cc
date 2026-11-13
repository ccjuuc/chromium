// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_web_request_filter.h"

#include <algorithm>
#include <utility>

#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "net/http/http_response_headers.h"
#include "net/http/http_util.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/mojom/fetch_api.mojom-shared.h"
#include "third_party/blink/public/mojom/loader/resource_load_info.mojom-shared.h"
#include "url/gurl.h"

namespace xenon::ipc {
namespace {
constexpr size_t kMaxHeaders = 256 * 1024;
bool KnownType(std::string_view type) {
  return type == "mainFrame" || type == "subFrame" || type == "stylesheet" ||
         type == "script" || type == "image" || type == "font" ||
         type == "object" || type == "xhr" || type == "ping" ||
         type == "cspReport" || type == "media" || type == "webSocket" ||
         type == "other";
}
bool ParsePatterns(const base::DictValue& value,
                   std::string_view key,
                   std::vector<URLPattern>* patterns,
                   std::string* error) {
  const auto* input = value.Find(key);
  if (!input) {
    return true;
  }
  const auto* list = input->GetIfList();
  if (!list || list->size() > 1024) {
    *error = "ERR_INVALID_ARG_VALUE: filter patterns must be an array";
    return false;
  }
  for (const auto& item : *list) {
    if (!item.is_string() || item.GetString().size() > 16384 ||
        item.GetString().find('\0') != std::string::npos) {
      *error = "ERR_INVALID_ARG_VALUE: invalid webRequest URL pattern";
      return false;
    }
    // Authorization is the exact Electron Session, not extension host grants.
    // SCHEME_ALL permits explicitly declared custom protocols in this Session.
    URLPattern pattern(URLPattern::SCHEME_ALL);
    if (pattern.Parse(item.GetString()) != URLPattern::ParseResult::kSuccess) {
      *error = "ERR_INVALID_ARG_VALUE: invalid webRequest URL pattern: " +
               item.GetString();
      return false;
    }
    if (pattern.scheme() == "ws" || pattern.scheme() == "wss") {
      *error =
          "ERR_NOT_SUPPORTED: WebSocket handshake interception is unavailable";
      return false;
    }
    patterns->push_back(std::move(pattern));
  }
  return true;
}
}  // namespace

ElectronWebRequestFilter::ElectronWebRequestFilter() = default;
ElectronWebRequestFilter::~ElectronWebRequestFilter() = default;
ElectronWebRequestFilter::ElectronWebRequestFilter(ElectronWebRequestFilter&&) =
    default;
ElectronWebRequestFilter& ElectronWebRequestFilter::operator=(
    ElectronWebRequestFilter&&) = default;

bool ElectronWebRequestFilter::Parse(const base::DictValue& value,
                                     std::string* error) {
  ElectronWebRequestFilter next;
  if (!ParsePatterns(value, "urls", &next.urls_, error) ||
      !ParsePatterns(value, "excludeUrls", &next.excluded_, error)) {
    return false;
  }
  if (const auto* input = value.Find("types")) {
    const auto* list = input->GetIfList();
    if (!list) {
      *error = "ERR_INVALID_ARG_VALUE: filter types must be an array";
      return false;
    }
    for (const auto& type : *list) {
      if (!type.is_string() || !KnownType(type.GetString())) {
        *error = "ERR_INVALID_ARG_VALUE: invalid webRequest resource type";
        return false;
      }
      if (type.GetString() == "webSocket") {
        *error =
            "ERR_NOT_SUPPORTED: WebSocket handshake interception is "
            "unavailable";
        return false;
      }
      next.types_.insert(type.GetString());
    }
  }
  *this = std::move(next);
  return true;
}

bool ElectronWebRequestFilter::Matches(const GURL& url,
                                       std::string_view type) const {
  const auto matches = [&](const URLPattern& pattern) {
    return pattern.MatchesURL(url);
  };
  return url.is_valid() && (types_.empty() || types_.contains(type)) &&
         (urls_.empty() || std::ranges::any_of(urls_, matches)) &&
         !std::ranges::any_of(excluded_, matches);
}

std::string ElectronWebRequestResourceType(
    const network::ResourceRequest& request) {
  if (request.resource_type ==
      static_cast<int>(blink::mojom::ResourceType::kPing)) {
    return "ping";
  }
  using Destination = network::mojom::RequestDestination;
  switch (request.destination) {
    case Destination::kDocument:
      return "mainFrame";
    case Destination::kFrame:
    case Destination::kIframe:
      return "subFrame";
    case Destination::kStyle:
      return "stylesheet";
    case Destination::kScript:
    case Destination::kWorker:
    case Destination::kSharedWorker:
    case Destination::kServiceWorker:
      return "script";
    case Destination::kImage:
      return "image";
    case Destination::kFont:
      return "font";
    case Destination::kObject:
    case Destination::kEmbed:
      return "object";
    case Destination::kAudio:
    case Destination::kVideo:
    case Destination::kTrack:
      return "media";
    case Destination::kReport:
      return "cspReport";
    case Destination::kEmpty:
      return "xhr";
    default:
      return "other";
  }
}

base::DictValue ElectronWebRequestHeaders(
    const net::HttpResponseHeaders& headers) {
  base::DictValue result;
  size_t iterator = 0;
  std::string name, value;
  while (headers.EnumerateHeaderLines(&iterator, &name, &value)) {
    name = base::ToLowerASCII(name);
    auto* list = result.FindList(name);
    if (!list) {
      list = result.Set(name, base::ListValue())->GetIfList();
    }
    list->Append(value);
  }
  return result;
}

base::DictValue ElectronWebRequestHeaders(
    const net::HttpRequestHeaders& headers) {
  base::DictValue result;
  for (const auto& header : headers.GetHeaderVector()) {
    result.Set(header.key, header.value);
  }
  return result;
}

bool BuildElectronWebRequestRequestHeaders(
    const base::DictValue& reply,
    std::optional<net::HttpRequestHeaders>* replacement,
    std::string* error) {
  replacement->reset();
  const auto* value = reply.Find("requestHeaders");
  if (!value) {
    return true;
  }
  const auto* headers = value->GetIfDict();
  if (!headers) {
    *error = "ERR_INVALID_ARG_VALUE: requestHeaders must be an object";
    return false;
  }
  net::HttpRequestHeaders result;
  size_t bytes = 0;
  std::set<std::string> names;
  for (auto [name, input] : *headers) {
    if (!net::HttpUtil::IsValidHeaderName(name) ||
        !names.insert(base::ToLowerASCII(name)).second || !input.is_string() ||
        !net::HttpUtil::IsValidHeaderValue(input.GetString())) {
      *error = "ERR_INVALID_ARG_VALUE: invalid or duplicate request header";
      return false;
    }
    bytes += name.size() + input.GetString().size() + 4;
    if (bytes > kMaxHeaders) {
      *error = "ERR_INVALID_ARG_VALUE: excessive request headers";
      return false;
    }
    result.SetHeader(name, input.GetString());
  }
  *replacement = std::move(result);
  return true;
}

bool BuildElectronWebRequestHeaders(
    const net::HttpResponseHeaders& original,
    const base::DictValue& reply,
    scoped_refptr<net::HttpResponseHeaders>* replacement,
    std::string* error) {
  replacement->reset();
  const auto* headers_value = reply.Find("responseHeaders");
  const auto* status_value = reply.Find("statusLine");
  if (!headers_value && !status_value) {
    return true;
  }
  const auto* headers = headers_value ? headers_value->GetIfDict() : nullptr;
  if (headers_value && !headers) {
    *error = "ERR_INVALID_ARG_VALUE: responseHeaders must be an object";
    return false;
  }
  std::string status = original.GetStatusLine();
  if (status_value) {
    if (!status_value->is_string()) {
      *error = "ERR_INVALID_ARG_VALUE: statusLine must be a string";
      return false;
    }
    status = status_value->GetString();
  }
  // HttpResponseHeaders is deliberately forgiving; validate before parsing.
  const size_t space = status.find(' ');
  int code = 0;
  const auto version = std::string_view(status).substr(0, space);
  if ((version != "HTTP/1.0" && version != "HTTP/1.1" &&
       version != "HTTP/2.0" && version != "HTTP/3.0") ||
      space == std::string::npos || space + 4 > status.size() ||
      status.size() > 1024 ||
      !base::StringToInt(std::string_view(status).substr(space + 1, 3),
                         &code) ||
      code < 100 || code > 599 ||
      (space + 4 < status.size() && status[space + 4] != ' ') ||
      !net::HttpUtil::IsValidHeaderValue(status)) {
    *error = "ERR_INVALID_ARG_VALUE: invalid HTTP status line";
    return false;
  }
  auto result = base::MakeRefCounted<net::HttpResponseHeaders>(status);
  auto unchanged =
      headers ? base::DictValue() : ElectronWebRequestHeaders(original);
  const auto& values = headers ? *headers : unchanged;
  size_t bytes = status.size();
  std::set<std::string> names;
  for (auto [name, input] : values) {
    // Header dictionary entries are case-insensitive. Case aliases are an
    // ambiguous replacement and must not accidentally weaken a security header.
    const auto normalized = base::ToLowerASCII(name);
    if (!net::HttpUtil::IsValidHeaderName(name) ||
        !names.insert(normalized).second) {
      *error =
          "ERR_INVALID_ARG_VALUE: invalid or duplicate response header name";
      return false;
    }
    const auto add = [&](const base::Value& value) {
      if (!value.is_string() ||
          !net::HttpUtil::IsValidHeaderValue(value.GetString())) {
        return false;
      }
      bytes += name.size() + value.GetString().size() + 4;
      if (bytes > kMaxHeaders) {
        return false;
      }
      result->AddHeader(name, value.GetString());
      return true;
    };
    if (const auto* list = input.GetIfList()) {
      for (const auto& value : *list) {
        if (!add(value)) {
          *error =
              "ERR_INVALID_ARG_VALUE: invalid or excessive response headers";
          return false;
        }
      }
    } else if (!add(input)) {
      *error =
          "ERR_INVALID_ARG_VALUE: response header must be a string or array";
      return false;
    }
  }
  *replacement = std::move(result);
  return true;
}
}  // namespace xenon::ipc
