// Copyright 2021 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/base/proxy_string_util.h"

#include <string>
#include <string_view>
#include <vector>

#include "base/check.h"
#include "base/notreached.h"
#include "base/strings/escape.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "build/buildflag.h"
#include "net/base/proxy_server.h"
#include "net/base/url_util.h"
#include "net/http/http_util.h"
#include "net/net_buildflags.h"
#include "url/third_party/mozilla/url_parse.h"
#include "url/url_canon_stdstring.h"
#include "url/url_util.h"

namespace net {

namespace {

// Parses the proxy type from a PAC string, to a ProxyServer::Scheme.
// This mapping is case-insensitive. If no type could be matched
// returns SCHEME_INVALID.
ProxyServer::Scheme GetSchemeFromPacTypeInternal(std::string_view type) {
  if (base::EqualsCaseInsensitiveASCII(type, "proxy")) {
    return ProxyServer::SCHEME_HTTP;
  }
  if (base::EqualsCaseInsensitiveASCII(type, "socks")) {
    // Default to v4 for compatibility. This is because the SOCKS4 vs SOCKS5
    // notation didn't originally exist, so if a client returns SOCKS they
    // really meant SOCKS4.
    return ProxyServer::SCHEME_SOCKS4;
  }
  if (base::EqualsCaseInsensitiveASCII(type, "socks4")) {
    return ProxyServer::SCHEME_SOCKS4;
  }
  if (base::EqualsCaseInsensitiveASCII(type, "socks5")) {
    return ProxyServer::SCHEME_SOCKS5;
  }
  if (base::EqualsCaseInsensitiveASCII(type, "https")) {
    return ProxyServer::SCHEME_HTTPS;
  }
  if (base::EqualsCaseInsensitiveASCII(type, "vless")) {
    return ProxyServer::SCHEME_VLESS;
  }
  if (base::EqualsCaseInsensitiveASCII(type, "vmess")) {
    return ProxyServer::SCHEME_VMESS;
  }
  if (base::EqualsCaseInsensitiveASCII(type, "trojan")) {
    return ProxyServer::SCHEME_TROJAN;
  }

  return ProxyServer::SCHEME_INVALID;
}

std::string ConstructHostPortString(std::string_view hostname, uint16_t port) {
  DCHECK(!hostname.empty());
  DCHECK((hostname.front() == '[' && hostname.back() == ']') ||
         hostname.find(":") == std::string_view::npos);

  return base::StrCat({hostname, ":", base::NumberToString(port)});
}

// Decodes one userinfo subcomponent (username or password) the same way as
// net::GetIdentityFromURL()'s UnescapeIdentityString helper: use
// UnescapeBinaryURLComponentSafe so control bytes stay escaped, and do not
// apply query-style '+' decoding (see net/base/url_util.cc).
std::string UnescapeLeafUserinfoPiece(std::string_view escaped_piece) {
  std::string unescaped;
  if (base::UnescapeBinaryURLComponentSafe(escaped_piece,
                                           /*fail_on_path_separators=*/false,
                                           &unescaped)) {
    return unescaped;
  }
  return std::string(escaped_piece);
}

std::string LeafCredentialFromAuthority(
    std::string_view authority,
    const url::Component& username_component,
    const url::Component& password_component) {
  std::string cred;
  if (username_component.is_valid()) {
    cred = UnescapeLeafUserinfoPiece(authority.substr(
        username_component.begin, username_component.len));
  }
  if (password_component.is_valid()) {
    if (!cred.empty()) {
      cred.push_back(':');
    }
    cred += UnescapeLeafUserinfoPiece(authority.substr(
        password_component.begin, password_component.len));
  }
  return cred;
}

// Serializes userinfo with url::EncodeURIComponent, consistent with encoding
// used elsewhere when building URL components (see url/url_util.h).
std::string LeafProxyUri(const ProxyServer& proxy_server,
                         std::string_view scheme_with_colon_slashslash) {
  const std::string hostport =
      ConstructHostPortString(proxy_server.GetHost(), proxy_server.GetPort());
  const std::string& cred = proxy_server.credential();
  std::string base_uri;
  if (cred.empty()) {
    base_uri = std::string(scheme_with_colon_slashslash) + hostport;
  } else {
    std::string encoded;
    url::StdStringCanonOutput o(&encoded);
    url::EncodeURIComponent(cred, &o);
    o.Complete();
    base_uri =
        base::StrCat({scheme_with_colon_slashslash, encoded, "@", hostport});
  }
  const std::string& q = proxy_server.leaf_uri_query();
  const std::string& f = proxy_server.leaf_uri_fragment();
  if (!q.empty()) {
    base_uri.push_back('?');
    base_uri += q;
  }
  if (!f.empty()) {
    base_uri.push_back('#');
    base_uri += f;
  }
  return base_uri;
}

// Strips `#fragment` then `?query` from the leaf authority string (content
// after scheme://). Per URL ordering, fragment is removed first.
void LeafStripUriQueryAndFragment(std::string_view* authority_in_out,
                                  std::string* leaf_query,
                                  std::string* leaf_fragment) {
  std::string_view s = *authority_in_out;
  leaf_query->clear();
  leaf_fragment->clear();

  const size_t hash = s.find('#');
  if (hash != std::string_view::npos) {
    *leaf_fragment = std::string(s.substr(hash + 1));
    s = s.substr(0, hash);
  }
  const size_t qmark = s.find('?');
  if (qmark != std::string_view::npos) {
    *leaf_query = std::string(s.substr(qmark + 1));
    s = s.substr(0, qmark);
  }
  *authority_in_out = s;
}

std::tuple<std::string_view, std::string_view>
PacResultElementToSchemeAndHostPort(std::string_view pac_result_element) {
  // Trim the leading/trailing whitespace.
  pac_result_element = HttpUtil::TrimLWS(pac_result_element);

  // Input should match:
  // ( <type> 1*(LWS) <host-and-port> )

  // Start by finding the first space (if any).
  size_t space = 0;
  for (; space < pac_result_element.size(); space++) {
    if (HttpUtil::IsLWS(pac_result_element[space])) {
      break;
    }
  }
  // Everything to the left of the space is the scheme.
  std::string_view scheme = pac_result_element.substr(0, space);

  // And everything to the right of the space is the
  // <host>[":" <port>].
  std::string_view host_and_port = pac_result_element.substr(space);
  return std::make_tuple(scheme, host_and_port);
}

}  // namespace

ProxyChain PacResultElementToProxyChain(std::string_view pac_result_element) {
  // Proxy chains are not supported in PAC strings, so this is just parsed
  // as a single server.
  auto [type, host_and_port] =
      PacResultElementToSchemeAndHostPort(pac_result_element);
  if (base::EqualsCaseInsensitiveASCII(type, "direct") &&
      host_and_port.empty()) {
    return ProxyChain::Direct();
  }
  return ProxyChain(PacResultElementToProxyServer(pac_result_element));
}

ProxyServer PacResultElementToProxyServer(std::string_view pac_result_element) {
  auto [type, host_and_port] =
      PacResultElementToSchemeAndHostPort(pac_result_element);
  ProxyServer::Scheme scheme = GetSchemeFromPacTypeInternal(type);
  return ProxySchemeHostAndPortToProxyServer(scheme, host_and_port);
}

std::string ProxyServerToPacResultElement(const ProxyServer& proxy_server) {
  switch (proxy_server.scheme()) {
    case ProxyServer::SCHEME_HTTP:
      return std::string("PROXY ") +
             ConstructHostPortString(proxy_server.GetHost(),
                                     proxy_server.GetPort());
    case ProxyServer::SCHEME_SOCKS4:
      // For compatibility send SOCKS instead of SOCKS4.
      return std::string("SOCKS ") +
             ConstructHostPortString(proxy_server.GetHost(),
                                     proxy_server.GetPort());
    case ProxyServer::SCHEME_SOCKS5:
      return std::string("SOCKS5 ") +
             ConstructHostPortString(proxy_server.GetHost(),
                                     proxy_server.GetPort());
    case ProxyServer::SCHEME_HTTPS:
      return std::string("HTTPS ") +
             ConstructHostPortString(proxy_server.GetHost(),
                                     proxy_server.GetPort());
    case ProxyServer::SCHEME_QUIC:
      return std::string("QUIC ") +
             ConstructHostPortString(proxy_server.GetHost(),
                                     proxy_server.GetPort());
    case ProxyServer::SCHEME_VLESS:
      return std::string("VLESS ") +
             ConstructHostPortString(proxy_server.GetHost(),
                                     proxy_server.GetPort());
    case ProxyServer::SCHEME_VMESS:
      return std::string("VMESS ") +
             ConstructHostPortString(proxy_server.GetHost(),
                                     proxy_server.GetPort());
    case ProxyServer::SCHEME_TROJAN:
      return std::string("TROJAN ") +
             ConstructHostPortString(proxy_server.GetHost(),
                                     proxy_server.GetPort());
    default:
      // Got called with an invalid scheme.
      NOTREACHED();
  }
}

ProxyChain ProxyUriToProxyChain(std::string_view uri,
                                ProxyServer::Scheme default_scheme,
                                bool is_quic_allowed) {
  // If uri is direct, return direct proxy chain.
  uri = HttpUtil::TrimLWS(uri);
  size_t colon = uri.find("://");
  if (colon != std::string_view::npos &&
      base::EqualsCaseInsensitiveASCII(uri.substr(0, colon), "direct")) {
    if (!uri.substr(colon + 3).empty()) {
      return ProxyChain();  // Invalid -- Direct chain cannot have a host/port.
    }
    return ProxyChain::Direct();
  }
  return ProxyChain(
      ProxyUriToProxyServer(uri, default_scheme, is_quic_allowed));
}

ProxyServer ProxyUriToProxyServer(std::string_view uri,
                                  ProxyServer::Scheme default_scheme,
                                  bool is_quic_allowed) {
  // We will default to |default_scheme| if no scheme specifier was given.
  ProxyServer::Scheme scheme = default_scheme;

  // Trim the leading/trailing whitespace.
  uri = HttpUtil::TrimLWS(uri);

  // Check for [<scheme> "://"]
  size_t colon = uri.find(':');
  if (colon != std::string_view::npos && uri.size() - colon >= 3 &&
      uri[colon + 1] == '/' && uri[colon + 2] == '/') {
    scheme = GetSchemeFromUriScheme(uri.substr(0, colon), is_quic_allowed);
    uri = uri.substr(colon + 3);  // Skip past the "://"
  }

  // Now parse the <host>[":"<port>].
  return ProxySchemeHostAndPortToProxyServer(scheme, uri);
}

std::string ProxyServerToProxyUri(const ProxyServer& proxy_server) {
  switch (proxy_server.scheme()) {
    case ProxyServer::SCHEME_HTTP:
      // Leave off "http://" since it is our default scheme.
      return ConstructHostPortString(proxy_server.GetHost(),
                                     proxy_server.GetPort());
    case ProxyServer::SCHEME_SOCKS4:
      return std::string("socks4://") +
             ConstructHostPortString(proxy_server.GetHost(),
                                     proxy_server.GetPort());
    case ProxyServer::SCHEME_SOCKS5:
      return std::string("socks5://") +
             ConstructHostPortString(proxy_server.GetHost(),
                                     proxy_server.GetPort());
    case ProxyServer::SCHEME_HTTPS:
      return std::string("https://") +
             ConstructHostPortString(proxy_server.GetHost(),
                                     proxy_server.GetPort());
    case ProxyServer::SCHEME_QUIC:
      return std::string("quic://") +
             ConstructHostPortString(proxy_server.GetHost(),
                                     proxy_server.GetPort());
    case ProxyServer::SCHEME_VLESS:
      return LeafProxyUri(proxy_server, "vless://");
    case ProxyServer::SCHEME_VMESS:
      return LeafProxyUri(proxy_server, "vmess://");
    case ProxyServer::SCHEME_TROJAN:
      return LeafProxyUri(proxy_server, "trojan://");
    default:
      // Got called with an invalid scheme.
      NOTREACHED();
  }
}

ProxyServer ProxySchemeHostAndPortToProxyServer(
    ProxyServer::Scheme scheme,
    std::string_view host_and_port) {
  // Trim leading/trailing space.
  host_and_port = HttpUtil::TrimLWS(host_and_port);

  if (scheme == ProxyServer::SCHEME_INVALID) {
    return ProxyServer();
  }

  std::string_view authority = host_and_port;
  std::string leaf_query;
  std::string leaf_fragment;
  const bool is_leaf_outbound =
      scheme == ProxyServer::SCHEME_VLESS ||
      scheme == ProxyServer::SCHEME_VMESS ||
      scheme == ProxyServer::SCHEME_TROJAN;
  if (is_leaf_outbound) {
    LeafStripUriQueryAndFragment(&authority, &leaf_query, &leaf_fragment);
  }

  url::Component username_component;
  url::Component password_component;
  url::Component hostname_component;
  url::Component port_component;
  url::ParseAuthority(authority, url::Component(0, authority.size()),
                      url::ParserMode::kSpecialURL, &username_component,
                      &password_component, &hostname_component,
                      &port_component);

  if (is_leaf_outbound) {
    if (hostname_component.is_empty()) {
      return ProxyServer();
    }
    std::string_view hostname =
        authority.substr(hostname_component.begin, hostname_component.len);
    if (port_component.is_valid() && port_component.is_empty()) {
      return ProxyServer();
    }
    std::string_view port =
        port_component.is_nonempty()
            ? authority.substr(port_component.begin, port_component.len)
            : "";
    std::string cred = LeafCredentialFromAuthority(
        authority, username_component, password_component);
    ProxyServer base =
        ProxyServer::FromSchemeHostAndPort(scheme, hostname, port);
    if (!base.is_valid()) {
      return ProxyServer();
    }
    return ProxyServer(scheme, base.host_port_pair(), std::move(cred),
                       std::move(leaf_query), std::move(leaf_fragment));
  }

  if (username_component.is_valid() || password_component.is_valid() ||
      hostname_component.is_empty()) {
    return ProxyServer();
  }

  std::string_view hostname =
      authority.substr(hostname_component.begin, hostname_component.len);

  // Reject inputs like "foo:". /url parsing and canonicalization code generally
  // allows it and treats it the same as a URL without a specified port, but
  // Chrome has traditionally disallowed it in proxy specifications.
  if (port_component.is_valid() && port_component.is_empty()) {
    return ProxyServer();
  }
  std::string_view port =
      port_component.is_nonempty()
          ? authority.substr(port_component.begin, port_component.len)
          : "";

  return ProxyServer::FromSchemeHostAndPort(scheme, hostname, port);
}

ProxyServer::Scheme GetSchemeFromUriScheme(std::string_view scheme,
                                           bool is_quic_allowed) {
  if (base::EqualsCaseInsensitiveASCII(scheme, "http")) {
    return ProxyServer::SCHEME_HTTP;
  }
  if (base::EqualsCaseInsensitiveASCII(scheme, "socks4")) {
    return ProxyServer::SCHEME_SOCKS4;
  }
  if (base::EqualsCaseInsensitiveASCII(scheme, "socks")) {
    return ProxyServer::SCHEME_SOCKS5;
  }
  if (base::EqualsCaseInsensitiveASCII(scheme, "socks5")) {
    return ProxyServer::SCHEME_SOCKS5;
  }
  if (base::EqualsCaseInsensitiveASCII(scheme, "https")) {
    return ProxyServer::SCHEME_HTTPS;
  }
#if BUILDFLAG(ENABLE_QUIC_PROXY_SUPPORT)
  if (is_quic_allowed && base::EqualsCaseInsensitiveASCII(scheme, "quic")) {
    return ProxyServer::SCHEME_QUIC;
  }
#endif  // BUILDFLAG(ENABLE_QUIC_PROXY_SUPPORT)
  if (base::EqualsCaseInsensitiveASCII(scheme, "vless")) {
    return ProxyServer::SCHEME_VLESS;
  }
  if (base::EqualsCaseInsensitiveASCII(scheme, "vmess")) {
    return ProxyServer::SCHEME_VMESS;
  }
  if (base::EqualsCaseInsensitiveASCII(scheme, "trojan")) {
    return ProxyServer::SCHEME_TROJAN;
  }
  return ProxyServer::SCHEME_INVALID;
}

ProxyChain MultiProxyUrisToProxyChain(std::string_view uris,
                                      ProxyServer::Scheme default_scheme,
                                      bool is_quic_allowed) {
#if BUILDFLAG(ENABLE_BRACKETED_PROXY_URIS)
  uris = HttpUtil::TrimLWS(uris);
  if (uris.empty()) {
    return ProxyChain();
  }

  bool has_multi_proxy_brackets = uris.front() == '[' && uris.back() == ']';
  // Remove `[]` if present
  if (has_multi_proxy_brackets) {
    uris = HttpUtil::TrimLWS(uris.substr(1, uris.size() - 2));
  }

  std::vector<ProxyServer> proxy_server_list;
  std::vector<std::string_view> uris_list = base::SplitStringPiece(
      uris, " ", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
  size_t number_of_proxy_uris = uris_list.size();
  bool has_invalid_format =
      number_of_proxy_uris > 1 && !has_multi_proxy_brackets;

  // If uris list is empty or has invalid formatting for multi-proxy chains, an
  // invalid `ProxyChain` should be returned.
  if (uris_list.empty() || has_invalid_format) {
    return ProxyChain();
  }

  for (const auto& uri : uris_list) {
    // If direct is found, it MUST be the only uri in the list. Otherwise, it is
    // an invalid `ProxyChain()`.
    if (base::EqualsCaseInsensitiveASCII(uri, "direct://")) {
      return number_of_proxy_uris > 1 ? ProxyChain() : ProxyChain::Direct();
    }

    proxy_server_list.push_back(
        ProxyUriToProxyServer(uri, default_scheme, is_quic_allowed));
  }

  return ProxyChain(std::move(proxy_server_list));
#else
  // This function should not be called in non-debug modes.
  NOTREACHED();
#endif  // !BUILDFLAG(ENABLE_BRACKETED_PROXY_URIS)
}

#if BUILDFLAG(ENABLE_CHROMIUM_LEAF) && BUILDFLAG(CHROMIUM_LEAF_BUILTIN_DEFAULT_PROXY)
// Defaults seeded into prefs::kProxy when the builtin Leaf profile default is
// used. For runtime overrides from browser code, set profile string prefs
// (merged into net::ProxyConfig by PrefProxyConfigTrackerImpl::ReadPrefConfig):
//   components/proxy_config/proxy_config_pref_names.h —
//     proxy_config::prefs::kChromiumLeafVlessUri
//     proxy_config::prefs::kChromiumLeafProxyHostPatterns
// Example: profile->GetPrefs()->SetString(kChromiumLeafVlessUri, "vless://...");
const char kChromiumLeafDefaultProxyUri[] =
    "vless://85ad7b82-738b-44f7-91ce-64a1ff53a314@www.ettreasure.com:30507"
    "?encryption=none&security=none&type=ws&host=www.ettreasure.com&path=%2F30507"
    "#kxinarvy";
// Pre-seed split-tunnel list (same syntax as proxy bypass_list). Non-empty
// browser pref kChromiumLeafProxyHostPatterns overrides this after load.
const char kChromiumLeafDefaultProxyHostPatterns[] = "";
#endif

}  // namespace net
