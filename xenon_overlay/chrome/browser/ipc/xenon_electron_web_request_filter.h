// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
#ifndef XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_WEB_REQUEST_FILTER_H_
#define XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_WEB_REQUEST_FILTER_H_

#include <optional>
#include <set>
#include <string>
#include <vector>

#include "base/memory/scoped_refptr.h"
#include "base/values.h"
#include "extensions/common/url_pattern.h"
#include "net/http/http_request_headers.h"

class GURL;
namespace net {
class HttpResponseHeaders;
}
namespace network {
struct ResourceRequest;
}

namespace xenon::ipc {
class ElectronWebRequestFilter {
 public:
  ElectronWebRequestFilter();
  ~ElectronWebRequestFilter();
  ElectronWebRequestFilter(ElectronWebRequestFilter&&);
  ElectronWebRequestFilter& operator=(ElectronWebRequestFilter&&);
  ElectronWebRequestFilter(const ElectronWebRequestFilter&) = delete;
  ElectronWebRequestFilter& operator=(const ElectronWebRequestFilter&) = delete;
  bool Parse(const base::DictValue& value, std::string* error);
  bool Matches(const GURL& url, std::string_view resource_type) const;

 private:
  std::vector<URLPattern> urls_;
  std::vector<URLPattern> excluded_;
  std::set<std::string, std::less<>> types_;
};
std::string ElectronWebRequestResourceType(
    const network::ResourceRequest& request);
base::DictValue ElectronWebRequestHeaders(
    const net::HttpResponseHeaders& headers);
base::DictValue ElectronWebRequestHeaders(
    const net::HttpRequestHeaders& headers);
// A present requestHeaders object replaces the entire header set. Omission
// keeps it unchanged, and malformed values never produce a partial result.
bool BuildElectronWebRequestRequestHeaders(
    const base::DictValue& reply,
    std::optional<net::HttpRequestHeaders>* replacement,
    std::string* error);
// Validates the entire modification before returning a replacement. Null
// output means leave headers intact; cancellation is handled by the caller.
bool BuildElectronWebRequestHeaders(
    const net::HttpResponseHeaders& original,
    const base::DictValue& reply,
    scoped_refptr<net::HttpResponseHeaders>* replacement,
    std::string* error);
}  // namespace xenon::ipc
#endif
