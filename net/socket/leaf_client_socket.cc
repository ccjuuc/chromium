// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/socket/leaf_client_socket.h"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

#include "base/base64.h"
#include "base/check_op.h"
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/notreached.h"
#include "base/rand_util.h"
#include "base/strings/escape.h"
#include "base/strings/string_util.h"
#include "base/task/sequenced_task_runner.h"
#include "build/buildflag.h"
#include "net/base/io_buffer.h"
#include "net/base/net_errors.h"
#include "net/net_buildflags.h"
#include "net/socket/chromium_leaf_vless_handshake.h"
#include "net/socket/next_proto.h"
#include "net/socket/socket_tag.h"

namespace net {

namespace {

#if BUILDFLAG(ENABLE_CHROMIUM_LEAF)
bool VmessFeedWsPayload(chromium_leaf::LeafVmessStreamEngine* engine,
                        const std::vector<uint8_t>& payload,
                        std::vector<uint8_t>* pending_plaintext) {
  std::vector<uint8_t> dec;
  if (!engine->FeedCipherText(
          base::span<const uint8_t>(payload.data(), payload.size()), &dec)) {
    LOG(ERROR) << "[LEAF_PROXY_DEBUG] VMess FeedCipherText failed (WS payload) "
               << "len=" << payload.size();
    return false;
  }
  if (!dec.empty()) {
    pending_plaintext->insert(pending_plaintext->end(), dec.begin(),
                              dec.end());
  }
  // FeedCipherText may buffer partial VMess records; success still means this WS
  // payload was consumed.
  return true;
}
#endif

std::vector<uint8_t> BuildMaskedWsBinaryFrame(base::span<const uint8_t> payload) {
  const uint64_t plen = static_cast<uint64_t>(payload.size());
  if (plen > 0xFFFFFFFFULL) {
    return {};
  }
  std::vector<uint8_t> frame;
  frame.push_back(0x82);
  uint8_t maskbit = 0x80;
  if (plen < 126) {
    frame.push_back(maskbit | static_cast<uint8_t>(plen));
  } else if (plen <= 0xFFFF) {
    frame.push_back(maskbit | 126);
    frame.push_back(static_cast<uint8_t>((plen >> 8) & 0xff));
    frame.push_back(static_cast<uint8_t>(plen & 0xff));
  } else {
    frame.push_back(maskbit | 127);
    for (int i = 7; i >= 0; --i) {
      frame.push_back(static_cast<uint8_t>((plen >> (8 * i)) & 0xff));
    }
  }
  uint8_t mask[4];
  base::RandBytes(mask);
  frame.insert(frame.end(), std::begin(mask), std::end(mask));
  for (size_t i = 0; i < payload.size(); ++i) {
    frame.push_back(static_cast<uint8_t>(payload[i] ^ mask[i % 4]));
  }
  return frame;
}

}  // namespace

LeafClientSocket::LeafClientSocket(
    std::unique_ptr<StreamSocket> transport_socket,
    const HostPortPair& destination,
    LeafOutboundProtocol protocol,
    std::string leaf_credential,
    std::string leaf_uri_query,
    std::string leaf_uri_fragment,
    std::string leaf_proxy_authority_host,
    const NetworkTrafficAnnotationTag& traffic_annotation)
    : transport_socket_(std::move(transport_socket)),
      destination_(destination),
      protocol_(protocol),
      leaf_credential_(std::move(leaf_credential)),
      leaf_uri_query_(std::move(leaf_uri_query)),
      leaf_uri_fragment_(std::move(leaf_uri_fragment)),
      leaf_proxy_authority_host_(std::move(leaf_proxy_authority_host)),
      traffic_annotation_(traffic_annotation),
      net_log_(transport_socket_->NetLog()),
      handshake_io_callback_(
          base::BindRepeating(&LeafClientSocket::OnHandshakeIOComplete,
                              base::Unretained(this))) {}

LeafClientSocket::~LeafClientSocket() {
  Disconnect();
}

int LeafClientSocket::Connect(CompletionOnceCallback callback) {
  DCHECK(transport_socket_);
  DCHECK(user_connect_callback_.is_null());
  if (completed_handshake_) {
    return OK;
  }

#if !BUILDFLAG(ENABLE_CHROMIUM_LEAF)
  net_log_.BeginEvent(NetLogEventType::LEAF_PROXY_CONNECT);
  net_log_.EndEventWithNetErrorCode(NetLogEventType::LEAF_PROXY_CONNECT,
                                     ERR_NOT_IMPLEMENTED);
  return ERR_NOT_IMPLEMENTED;
#else

#if BUILDFLAG(ENABLE_CHROMIUM_LEAF)
  VLOG(1) << "[LEAF_PROXY] LeafClientSocket::Connect protocol="
          << static_cast<int>(protocol_) << " dest=" << destination_.ToString()
          << " query_len=" << leaf_uri_query_.size()
          << " fragment_len=" << leaf_uri_fragment_.size();
#endif

  net_log_.BeginEvent(NetLogEventType::LEAF_PROXY_CONNECT);

  if (protocol_ != LeafOutboundProtocol::kVless &&
      protocol_ != LeafOutboundProtocol::kVmess) {
    net_log_.EndEventWithNetErrorCode(NetLogEventType::LEAF_PROXY_CONNECT,
                                      ERR_NOT_IMPLEMENTED);
    return ERR_NOT_IMPLEMENTED;
  }
  if (leaf_credential_.empty()) {
    net_log_.EndEventWithNetErrorCode(NetLogEventType::LEAF_PROXY_CONNECT,
                                      ERR_INVALID_ARGUMENT);
    return ERR_INVALID_ARGUMENT;
  }

  std::array<uint8_t, 16> uuid{};
  if (!LeafVlessParseUuid(leaf_credential_, &uuid)) {
    net_log_.EndEventWithNetErrorCode(NetLogEventType::LEAF_PROXY_CONNECT,
                                      ERR_INVALID_ARGUMENT);
    return ERR_INVALID_ARGUMENT;
  }

  vless_uuid_ = uuid;
#if BUILDFLAG(ENABLE_CHROMIUM_LEAF)
  vmess_engine_.reset();
  if (protocol_ == LeafOutboundProtocol::kVmess) {
    use_vision_ = false;
    need_strip_vless_response_ = false;
    std::string cipher = LeafVlessQueryLookup(leaf_uri_query_, "encryption");
    if (cipher.empty()) {
      cipher = LeafVlessQueryLookup(leaf_uri_query_, "cipher");
    }
    if (cipher.empty()) {
      cipher = "chacha20-poly1305";
    }
    chromium_leaf::VmessSessionMaterial vmess_sess;
    if (!chromium_leaf::LeafVmessBuildClientRequest(
            uuid, destination_.host(), destination_.port(), cipher,
            &handshake_vless_hdr_, &vmess_sess)) {
      net_log_.EndEventWithNetErrorCode(NetLogEventType::LEAF_PROXY_CONNECT,
                                        ERR_INVALID_ARGUMENT);
      return ERR_INVALID_ARGUMENT;
    }
    vmess_engine_ =
        std::make_unique<chromium_leaf::LeafVmessStreamEngine>(vmess_sess);
    VLOG(1) << "[LEAF_PROXY_DEBUG] LeafClientSocket VMess client hello built "
            << "dest=" << destination_.ToString() << " cipher=" << cipher
            << " authority_host=" << leaf_proxy_authority_host_
            << " underlying_negotiated_alpn="
            << NextProtoToString(transport_socket_->GetNegotiatedProtocol());
  } else {
    const std::string flow =
        base::ToLowerASCII(LeafVlessQueryLookup(leaf_uri_query_, "flow"));
    use_vision_ = (flow == "xtls-rprx-vision");
    handshake_vless_hdr_ =
        use_vision_ ? LeafVlessBuildVisionRequestHeader(
                          uuid, destination_.host(), destination_.port())
                    : LeafVlessBuildRequestHeader(uuid, destination_.host(),
                                                  destination_.port());
  }
#else
  use_vision_ = false;
  handshake_vless_hdr_ =
      LeafVlessBuildRequestHeader(uuid, destination_.host(), destination_.port());
#endif
  if (handshake_vless_hdr_.empty()) {
    net_log_.EndEventWithNetErrorCode(NetLogEventType::LEAF_PROXY_CONNECT,
                                      ERR_INVALID_ARGUMENT);
    return ERR_INVALID_ARGUMENT;
  }

  handshake_type_ =
      base::ToLowerASCII(LeafVlessQueryLookup(leaf_uri_query_, "type"));
  handshake_write_buf_.reset();
  handshake_read_buf_ = nullptr;
  handshake_http_headers_.clear();
  handshake_tcp_prefix_read_ = 0;
  vless_tcp_addon_remaining_ = -1;
  handshake_ws_http_str_.clear();
  ws_rx_accumulator_.clear();
  ws_framing_ = false;

  if (handshake_type_.empty() || handshake_type_ == "tcp") {
    handshake_state_ = HandshakeState::kTcpWriteVless;
  } else if (handshake_type_ == "ws") {
    std::string ws_host = LeafVlessQueryLookup(leaf_uri_query_, "host");
    if (ws_host.empty()) {
      ws_host = leaf_proxy_authority_host_;
    }
    std::string path = base::UnescapeURLComponent(
        LeafVlessQueryLookup(leaf_uri_query_, "path"),
        base::UnescapeRule::NORMAL | base::UnescapeRule::SPACES |
            base::UnescapeRule::PATH_SEPARATORS);
    if (path.empty()) {
      path = "/";
    }
    if (path[0] != '/') {
      path.insert(path.begin(), '/');
    }
    std::array<uint8_t, 16> key_raw{};
    base::RandBytes(key_raw);
    std::string sec_key = base::Base64Encode(key_raw);
    // Xray: headers from streamSettings; URI can carry useragent=/origin= for parity.
    std::string ws_ua = LeafVlessQueryLookup(leaf_uri_query_, "useragent");
    if (ws_ua.empty()) {
      ws_ua = LeafVlessQueryLookup(leaf_uri_query_, "userAgent");
    }
    std::string ws_origin = LeafVlessQueryLookup(leaf_uri_query_, "origin");
    handshake_ws_http_str_ = LeafVlessBuildWebSocketUpgradeRequest(
        path, ws_host, sec_key, ws_ua, ws_origin);
    VLOG(1) << "[LEAF_PROXY_DEBUG] LeafClientSocket WS GET host=" << ws_host
            << " path=" << path
            << " protocol=" << static_cast<int>(protocol_)
            << " final_dest=" << destination_.ToString();
    {
      size_t nl = handshake_ws_http_str_.find('\r');
      if (nl == std::string::npos) {
        nl = handshake_ws_http_str_.find('\n');
      }
      VLOG(1) << "[LEAF_PROXY_DEBUG] LeafClientSocket WS request line: "
              << handshake_ws_http_str_.substr(0, std::min(nl, size_t(300)));
    }
    handshake_state_ = HandshakeState::kWsWriteHttp;
  } else {
    net_log_.EndEventWithNetErrorCode(NetLogEventType::LEAF_PROXY_CONNECT,
                                      ERR_NOT_IMPLEMENTED);
    return ERR_NOT_IMPLEMENTED;
  }

  int rv = DoHandshakeLoop(OK);
  if (rv == ERR_IO_PENDING) {
    user_connect_callback_ = std::move(callback);
  } else {
    net_log_.EndEventWithNetErrorCode(NetLogEventType::LEAF_PROXY_CONNECT, rv);
    if (rv == OK) {
      completed_handshake_ = true;
      if (handshake_type_ == "ws") {
        ws_framing_ = true;
      }
#if BUILDFLAG(ENABLE_CHROMIUM_LEAF)
      if (protocol_ == LeafOutboundProtocol::kVless && use_vision_) {
        vision_parser_ =
            std::make_unique<LeafVlessVisionParser>(vless_uuid_);
      }
#endif
    }
  }
#if BUILDFLAG(ENABLE_CHROMIUM_LEAF)
  VLOG(1) << "[LEAF_PROXY] LeafClientSocket::Connect handshake "
          << "rv=" << rv
          << (rv == ERR_IO_PENDING ? " (ERR_IO_PENDING async)" : "")
          << " ws_framing=" << ws_framing_;
#endif
  return rv;
#endif  // BUILDFLAG(ENABLE_CHROMIUM_LEAF)
}

void LeafClientSocket::OnHandshakeIOComplete(int result) {
  int rv = DoHandshakeLoop(result);
  if (rv != ERR_IO_PENDING) {
    net_log_.EndEventWithNetErrorCode(NetLogEventType::LEAF_PROXY_CONNECT, rv);
    // Set state BEFORE invoking the callback: the callback recipient (e.g.
    // SSLClientSocket) may immediately call Read()/Write() on this socket,
    // which check completed_handshake_.
    if (rv == OK) {
      completed_handshake_ = true;
      if (handshake_type_ == "ws") {
        ws_framing_ = true;
      }
#if BUILDFLAG(ENABLE_CHROMIUM_LEAF)
      if (protocol_ == LeafOutboundProtocol::kVless && use_vision_) {
        vision_parser_ =
            std::make_unique<LeafVlessVisionParser>(vless_uuid_);
      }
#endif
    }
    handshake_state_ = HandshakeState::kNone;
#if BUILDFLAG(ENABLE_CHROMIUM_LEAF)
    VLOG(1) << "[LEAF_PROXY] LeafClientSocket handshake async done "
             << "rv=" << rv << " ws_framing=" << ws_framing_;
#endif
    if (!user_connect_callback_.is_null()) {
      std::move(user_connect_callback_).Run(rv);
    }
  }
}

int LeafClientSocket::DoHandshakeLoop(int result) {
  int rv = result;
  DCHECK_NE(handshake_state_, HandshakeState::kNone);

  do {
    HandshakeState state = handshake_state_;
    handshake_state_ = HandshakeState::kNone;
    switch (state) {
      case HandshakeState::kTcpWriteVless: {
        DCHECK_EQ(OK, rv);
        if (!handshake_write_buf_) {
          auto vb =
              base::MakeRefCounted<VectorIOBuffer>(std::move(handshake_vless_hdr_));
          const int sz = static_cast<int>(vb->size());
          handshake_write_buf_ =
              base::MakeRefCounted<DrainableIOBuffer>(std::move(vb), sz);
        }
        handshake_state_ = HandshakeState::kTcpWriteVlessComplete;
        rv = transport_socket_->Write(
            handshake_write_buf_.get(), handshake_write_buf_->BytesRemaining(),
            handshake_io_callback_, traffic_annotation_);
        break;
      }
      case HandshakeState::kTcpWriteVlessComplete: {
        if (rv < 0) {
          return rv;
        }
        handshake_write_buf_->DidConsume(rv);
        if (handshake_write_buf_->BytesRemaining() > 0) {
          handshake_state_ = HandshakeState::kTcpWriteVless;
          rv = OK;
          break;
        }
        handshake_write_buf_.reset();
        // VLESS: server may defer bytes until dest speaks; strip 2-byte (+addon)
        // response on first Read. VMess: response is AEAD-framed — do not strip.
        need_strip_vless_response_ =
            (protocol_ == LeafOutboundProtocol::kVless) && !use_vision_;
        return OK;
      }

      case HandshakeState::kWsWriteHttp: {
        DCHECK_EQ(OK, rv);
        if (!handshake_write_buf_) {
          std::vector<uint8_t> bytes(handshake_ws_http_str_.begin(),
                                    handshake_ws_http_str_.end());
          handshake_ws_http_str_.clear();
          auto vb = base::MakeRefCounted<VectorIOBuffer>(std::move(bytes));
          const int sz = static_cast<int>(vb->size());
          handshake_write_buf_ =
              base::MakeRefCounted<DrainableIOBuffer>(std::move(vb), sz);
        }
        handshake_state_ = HandshakeState::kWsWriteHttpComplete;
        rv = transport_socket_->Write(
            handshake_write_buf_.get(), handshake_write_buf_->BytesRemaining(),
            handshake_io_callback_, traffic_annotation_);
        break;
      }
      case HandshakeState::kWsWriteHttpComplete: {
        if (rv < 0) {
          return rv;
        }
        handshake_write_buf_->DidConsume(rv);
        if (handshake_write_buf_->BytesRemaining() > 0) {
          handshake_state_ = HandshakeState::kWsWriteHttp;
          rv = OK;
          break;
        }
        handshake_write_buf_.reset();
        handshake_state_ = HandshakeState::kWsReadHeaders;
        rv = OK;
        break;
      }
      case HandshakeState::kWsReadHeaders: {
        DCHECK_EQ(OK, rv);
        handshake_read_buf_ =
            base::MakeRefCounted<IOBufferWithSize>(kHandshakeHeaderReadChunk);
        handshake_state_ = HandshakeState::kWsReadHeadersComplete;
        rv = transport_socket_->Read(
            handshake_read_buf_.get(), kHandshakeHeaderReadChunk,
            handshake_io_callback_);
        break;
      }
      case HandshakeState::kWsReadHeadersComplete: {
        if (rv < 0) {
          return rv;
        }
        if (rv == 0) {
          return ERR_CONNECTION_CLOSED;
        }
        handshake_http_headers_.append(handshake_read_buf_->data(),
                                        static_cast<size_t>(rv));
        if (handshake_http_headers_.size() > 512 * 1024) {
          return ERR_FAILED;
        }
        const size_t sep = handshake_http_headers_.find("\r\n\r\n");
        if (sep == std::string::npos) {
          handshake_state_ = HandshakeState::kWsReadHeaders;
          rv = OK;
          break;
        }
        // TLS may coalesce 101 headers with the first WebSocket frame; bytes
        // after the header terminator belong in the framing reader, not here.
        if (sep + 4 < handshake_http_headers_.size()) {
          ws_rx_accumulator_.insert(
              ws_rx_accumulator_.end(),
              handshake_http_headers_.begin() +
                  static_cast<ptrdiff_t>(sep + 4),
              handshake_http_headers_.end());
          VLOG(1) << "[LEAF_PROXY_DEBUG] Leaf WS coalesced read: "
                  << (handshake_http_headers_.size() - (sep + 4))
                  << " bytes after 101 headers -> rx accumulator";
        }
        std::string headers_for_check =
            handshake_http_headers_.substr(0, sep + 4);
        handshake_http_headers_.clear();
        if (!LeafVlessHttpResponseFirstLineIs101(headers_for_check)) {
          LOG(ERROR) << "[LEAF_PROXY_DEBUG] Leaf WS not 101, header_prefix="
                     << headers_for_check.substr(
                            0, std::min<size_t>(headers_for_check.size(), 600u));
          return ERR_FAILED;
        }
        {
          size_t eol = headers_for_check.find("\r\n");
          VLOG(1) << "[LEAF_PROXY_DEBUG] Leaf WS 101 line="
                  << headers_for_check.substr(
                         0, std::min(eol, size_t(400)));
        }
        std::vector<uint8_t> frame =
            BuildMaskedWsBinaryFrame(handshake_vless_hdr_);
        if (frame.empty()) {
          return ERR_INVALID_ARGUMENT;
        }
        auto vb = base::MakeRefCounted<VectorIOBuffer>(std::move(frame));
        const int sz = static_cast<int>(vb->size());
        handshake_write_buf_ =
            base::MakeRefCounted<DrainableIOBuffer>(std::move(vb), sz);
        handshake_vless_hdr_.clear();
        handshake_state_ = HandshakeState::kWsWriteFrameComplete;
        rv = transport_socket_->Write(
            handshake_write_buf_.get(), handshake_write_buf_->BytesRemaining(),
            handshake_io_callback_, traffic_annotation_);
        break;
      }

      case HandshakeState::kWsWriteFrameComplete: {
        if (rv < 0) {
          return rv;
        }
        handshake_write_buf_->DidConsume(rv);
        if (handshake_write_buf_->BytesRemaining() > 0) {
          handshake_state_ = HandshakeState::kWsWriteFrameComplete;
          rv = transport_socket_->Write(
              handshake_write_buf_.get(),
              handshake_write_buf_->BytesRemaining(),
              handshake_io_callback_, traffic_annotation_);
          break;
        }
        handshake_write_buf_.reset();
        // VLESS-only: strip short server ack from first WS payload. VMess uses
        // AEAD response; stripping would corrupt the stream (symptom: TLS/H2 to
        // origin fails or random disconnects).
        need_strip_vless_response_ =
            (protocol_ == LeafOutboundProtocol::kVless) && !use_vision_;
        VLOG(1) << "[LEAF_PROXY_DEBUG] Leaf WS upgrade complete "
                << "need_strip_vless_ack=" << need_strip_vless_response_
                << " leaf_protocol=" << static_cast<int>(protocol_);
        return OK;
      }

      case HandshakeState::kNone:
      default:
        NOTREACHED();
    }
  } while (rv != ERR_IO_PENDING && handshake_state_ != HandshakeState::kNone);

  return rv;
}

void LeafClientSocket::Disconnect() {
  completed_handshake_ = false;
  ws_framing_ = false;
  use_vision_ = false;
  vless_uuid_.fill(0);
#if BUILDFLAG(ENABLE_CHROMIUM_LEAF)
  vision_parser_.reset();
  vision_rx_queue_.clear();
  vision_pending_user_buf_ = nullptr;
  vision_pending_user_len_ = 0;
  vmess_engine_.reset();
  vmess_rx_plain_.clear();
  vmess_read_buf_ = nullptr;
  vmess_pending_write_buf_ = nullptr;
  vmess_pending_user_write_len_ = 0;
  vmess_pending_write_callback_.Reset();
#endif
  handshake_state_ = HandshakeState::kNone;
  user_connect_callback_.Reset();
  vless_tcp_addon_remaining_ = -1;
  handshake_write_buf_.reset();
  handshake_read_buf_ = nullptr;
  handshake_http_headers_.clear();
  handshake_vless_hdr_.clear();
  handshake_type_.clear();
  handshake_tcp_prefix_read_ = 0;
  handshake_ws_http_str_.clear();
  pending_plaintext_.clear();
  need_strip_vless_response_ = false;
  ws_rx_accumulator_.clear();
  ws_message_fragment_.clear();
  ws_in_fragment_message_ = false;
  ws_read_buf_ = nullptr;
  ws_user_read_dst_ = nullptr;
  ws_user_read_len_ = 0;
  pending_read_callback_.Reset();
  ws_pending_write_buf_ = nullptr;
  ws_pending_write_original_len_ = 0;
  ws_pending_write_callback_.Reset();
  if (transport_socket_) {
    transport_socket_->Disconnect();
  }
}

bool LeafClientSocket::IsConnected() const {
  return completed_handshake_ && transport_socket_ &&
         transport_socket_->IsConnected();
}

bool LeafClientSocket::IsConnectedAndIdle() const {
  return completed_handshake_ && transport_socket_ &&
         transport_socket_->IsConnectedAndIdle();
}

const NetLogWithSource& LeafClientSocket::NetLog() const {
  return net_log_;
}

bool LeafClientSocket::WasEverUsed() const {
  return false;
}

NextProto LeafClientSocket::GetNegotiatedProtocol() const {
  return NextProto::kProtoUnknown;
}

bool LeafClientSocket::GetSSLInfo(SSLInfo* ssl_info) {
  return false;
}

int64_t LeafClientSocket::GetTotalReceivedBytes() const {
  return 0;
}

void LeafClientSocket::ApplySocketTag(const SocketTag& tag) {
  if (transport_socket_) {
    transport_socket_->ApplySocketTag(tag);
  }
}

bool LeafClientSocket::PopOneWebSocketFrame(std::vector<uint8_t>* accumulator,
                                            std::vector<uint8_t>* payload_out,
                                            bool* fin_out,
                                            uint8_t* opcode_out) {
  if (accumulator->size() < 2) {
    return false;
  }
  size_t i = 0;
  uint8_t b0 = (*accumulator)[i++];
  uint8_t b1 = (*accumulator)[i++];
  *fin_out = (b0 & 0x80) != 0;
  *opcode_out = b0 & 0x0F;
  bool masked = (b1 & 0x80) != 0;
  uint64_t plen = b1 & 0x7F;
  if (plen == 126) {
    if (accumulator->size() < i + 2) {
      return false;
    }
    plen = (static_cast<uint64_t>((*accumulator)[i]) << 8) |
           (*accumulator)[i + 1];
    i += 2;
  } else if (plen == 127) {
    if (accumulator->size() < i + 8) {
      return false;
    }
    plen = 0;
    for (int k = 0; k < 8; ++k) {
      plen = (plen << 8) | (*accumulator)[i++];
    }
  }
  uint8_t mask[4] = {0};
  if (masked) {
    if (accumulator->size() < i + 4) {
      return false;
    }
    memcpy(mask, accumulator->data() + i, 4);
    i += 4;
  }
  if (plen > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
    return false;
  }
  size_t plen_sz = static_cast<size_t>(plen);
  if (accumulator->size() < i + plen_sz) {
    return false;
  }
  payload_out->assign(
     accumulator->begin() + static_cast<ptrdiff_t>(i),
     accumulator->begin() + static_cast<ptrdiff_t>(i + plen_sz));
  if (masked) {
    for (size_t j = 0; j < payload_out->size(); ++j) {
      (*payload_out)[j] ^= mask[j % 4];
    }
  }
  i += plen_sz;
  accumulator->erase(
      accumulator->begin(),
      accumulator->begin() + static_cast<ptrdiff_t>(i));
  return true;
}

bool LeafClientSocket::PullNextCompleteWsMessageIntoPending() {
  while (true) {
    std::vector<uint8_t> payload;
    bool fin = false;
    uint8_t opcode = 0;
    if (!PopOneWebSocketFrame(&ws_rx_accumulator_, &payload, &fin, &opcode)) {
      return false;
    }
    if (opcode == 0x8) {
      return false;
    }
    if (opcode == 0x9 || opcode == 0xA) {
      continue;
    }
    if (opcode == 0x2) {
      if (fin) {
#if BUILDFLAG(ENABLE_CHROMIUM_LEAF)
        if (vmess_engine_) {
          if (!VmessFeedWsPayload(vmess_engine_.get(), payload,
                                  &pending_plaintext_)) {
            return false;
          }
          if (!pending_plaintext_.empty()) {
            return true;
          }
          continue;
        }
        if (use_vision_) {
          DCHECK(vision_parser_);
          std::vector<uint8_t> out = vision_parser_->Feed(
              base::span<const uint8_t>(payload.data(), payload.size()));
          pending_plaintext_.insert(pending_plaintext_.end(), out.begin(),
                                    out.end());
          return !pending_plaintext_.empty();
        }
#endif
        pending_plaintext_.insert(pending_plaintext_.end(), payload.begin(),
                                  payload.end());
        return true;
      }
#if BUILDFLAG(ENABLE_CHROMIUM_LEAF)
      // VMess ciphertext must be fed in order as a single byte stream; do not
      // pass a non-FIN binary fragment alone (would split AEAD records).
      if (vmess_engine_) {
        ws_message_fragment_ = std::move(payload);
        ws_in_fragment_message_ = true;
        continue;
      }
      if (use_vision_) {
        DCHECK(vision_parser_);
        std::vector<uint8_t> out = vision_parser_->Feed(
            base::span<const uint8_t>(payload.data(), payload.size()));
        pending_plaintext_.insert(pending_plaintext_.end(), out.begin(),
                                 out.end());
        return !pending_plaintext_.empty();
      }
#endif
      ws_message_fragment_ = std::move(payload);
      ws_in_fragment_message_ = true;
      continue;
    }
    if (opcode == 0x0 && ws_in_fragment_message_) {
      ws_message_fragment_.insert(ws_message_fragment_.end(), payload.begin(),
                                  payload.end());
      if (fin) {
#if BUILDFLAG(ENABLE_CHROMIUM_LEAF)
        if (vmess_engine_) {
          if (!VmessFeedWsPayload(vmess_engine_.get(), ws_message_fragment_,
                                  &pending_plaintext_)) {
            return false;
          }
          ws_message_fragment_.clear();
          ws_in_fragment_message_ = false;
          if (!pending_plaintext_.empty()) {
            return true;
          }
          continue;
        }
        if (use_vision_) {
          DCHECK(vision_parser_);
          std::vector<uint8_t> out = vision_parser_->Feed(base::span<const uint8_t>(
              ws_message_fragment_.data(), ws_message_fragment_.size()));
          ws_message_fragment_.clear();
          ws_in_fragment_message_ = false;
          pending_plaintext_.insert(pending_plaintext_.end(), out.begin(),
                                    out.end());
          return !pending_plaintext_.empty();
        }
#endif
        pending_plaintext_.insert(
            pending_plaintext_.end(),
            std::make_move_iterator(ws_message_fragment_.begin()),
            std::make_move_iterator(ws_message_fragment_.end()));
        ws_message_fragment_.clear();
        ws_in_fragment_message_ = false;
        return true;
      }
      continue;
    }
    continue;
  }
}

void LeafClientSocket::OnWsTransportRead(int result) {
  CHECK(pending_read_callback_);

  if (result <= 0) {
    std::move(pending_read_callback_).Run(result);
    return;
  }

  ws_rx_accumulator_.insert(ws_rx_accumulator_.end(), ws_read_buf_->data(),
                            ws_read_buf_->data() + result);

  while (PullNextCompleteWsMessageIntoPending()) {}

  int user_len = ws_user_read_len_;
  char* user_dst = ws_user_read_dst_.get();
  if (!pending_plaintext_.empty()) {
    // Strip the VLESS server response header from the first payload.
    if (need_strip_vless_response_) {
      need_strip_vless_response_ = false;
      if (!LeafVlessStripServerResponse(&pending_plaintext_)) {
        LOG(ERROR) << "[LEAF_PROXY_DEBUG] LeafVlessStripServerResponse failed "
                   << "(WS async path) pending_len=" << pending_plaintext_.size();
        std::move(pending_read_callback_).Run(ERR_FAILED);
        return;
      }
      if (pending_plaintext_.empty()) {
        // Response header consumed all data; read more.
        int rv = transport_socket_->Read(
            ws_read_buf_.get(), kWsReadChunk,
            base::BindOnce(&LeafClientSocket::OnWsTransportRead,
                           weak_factory_.GetWeakPtr()));
        if (rv != ERR_IO_PENDING) {
          OnWsTransportRead(rv);
        }
        return;
      }
    }
    const int n = static_cast<int>(std::min(
        static_cast<size_t>(user_len), pending_plaintext_.size()));
    memcpy(user_dst, pending_plaintext_.data(), static_cast<size_t>(n));
    pending_plaintext_.erase(pending_plaintext_.begin(),
                             pending_plaintext_.begin() + n);
    std::move(pending_read_callback_).Run(n);
    return;
  }

  int rv = transport_socket_->Read(
      ws_read_buf_.get(), kWsReadChunk,
      base::BindOnce(&LeafClientSocket::OnWsTransportRead,
                     weak_factory_.GetWeakPtr()));
  if (rv != ERR_IO_PENDING) {
    OnWsTransportRead(rv);
  }
}

void LeafClientSocket::CompleteTcpVlessStripAndIssueUserRead(
    scoped_refptr<IOBuffer> user_buf,
    int user_buf_len) {
  need_strip_vless_response_ = false;
  handshake_tcp_prefix_read_ = 0;
  vless_tcp_addon_remaining_ = -1;
  handshake_read_buf_ = nullptr;

  int rv = transport_socket_->Read(
      user_buf.get(), user_buf_len,
      base::BindOnce(&LeafClientSocket::OnTcpStripFinalReadComplete,
                     weak_factory_.GetWeakPtr()));
  if (rv != ERR_IO_PENDING) {
    std::move(pending_read_callback_).Run(rv);
  }
}

void LeafClientSocket::IssueTcpVlessAddonRead(scoped_refptr<IOBuffer> user_buf,
                                              int user_buf_len) {
  DCHECK_GT(vless_tcp_addon_remaining_, 0);
  const int need = vless_tcp_addon_remaining_;
  handshake_read_buf_ = base::MakeRefCounted<IOBufferWithSize>(need);
  int rv = transport_socket_->Read(
      handshake_read_buf_.get(), need,
      base::BindOnce(&LeafClientSocket::OnTcpVlessStripReadComplete,
                     weak_factory_.GetWeakPtr(), user_buf, user_buf_len));
  if (rv != ERR_IO_PENDING) {
    OnTcpVlessStripReadComplete(user_buf, user_buf_len, rv);
  }
}

void LeafClientSocket::OnTcpVlessStripReadComplete(
    scoped_refptr<IOBuffer> user_buf,
    int user_buf_len,
    int result) {
  if (result <= 0) {
    std::move(pending_read_callback_)
        .Run(result == 0 ? ERR_CONNECTION_CLOSED : result);
    return;
  }

  if (handshake_tcp_prefix_read_ < 2) {
    memcpy(handshake_tcp_prefix_.data() + handshake_tcp_prefix_read_,
           handshake_read_buf_->data(), static_cast<size_t>(result));
    handshake_tcp_prefix_read_ += result;
    if (handshake_tcp_prefix_read_ < 2) {
      const int need = 2 - handshake_tcp_prefix_read_;
      handshake_read_buf_ = base::MakeRefCounted<IOBufferWithSize>(need);
      int rv = transport_socket_->Read(
          handshake_read_buf_.get(), need,
          base::BindOnce(&LeafClientSocket::OnTcpVlessStripReadComplete,
                         weak_factory_.GetWeakPtr(), user_buf, user_buf_len));
      if (rv != ERR_IO_PENDING) {
        OnTcpVlessStripReadComplete(std::move(user_buf), user_buf_len, rv);
      }
      return;
    }
    vless_tcp_addon_remaining_ = static_cast<int>(handshake_tcp_prefix_[1]);
    if (vless_tcp_addon_remaining_ == 0) {
      CompleteTcpVlessStripAndIssueUserRead(std::move(user_buf), user_buf_len);
      return;
    }
    IssueTcpVlessAddonRead(std::move(user_buf), user_buf_len);
    return;
  }

  DCHECK_EQ(handshake_tcp_prefix_read_, 2);
  DCHECK_GT(vless_tcp_addon_remaining_, 0);
  vless_tcp_addon_remaining_ -= result;
  if (vless_tcp_addon_remaining_ > 0) {
    IssueTcpVlessAddonRead(std::move(user_buf), user_buf_len);
    return;
  }

  CompleteTcpVlessStripAndIssueUserRead(std::move(user_buf), user_buf_len);
}

int LeafClientSocket::ReadWithoutFraming(IOBuffer* buf,
                                         int buf_len,
                                         CompletionOnceCallback callback) {
#if BUILDFLAG(ENABLE_CHROMIUM_LEAF)
  if (use_vision_) {
    DCHECK(vision_parser_);
    while (true) {
      if (!vision_rx_queue_.empty()) {
        const int n = static_cast<int>(std::min(
            static_cast<size_t>(buf_len), vision_rx_queue_.size()));
        memcpy(buf->data(), vision_rx_queue_.data(), static_cast<size_t>(n));
        vision_rx_queue_.erase(
            vision_rx_queue_.begin(),
            vision_rx_queue_.begin() + static_cast<ptrdiff_t>(n));
        return n;
      }
      if (!handshake_read_buf_.get()) {
        handshake_read_buf_ =
            base::MakeRefCounted<IOBufferWithSize>(kWsReadChunk);
      }
      int rv = transport_socket_->Read(
          handshake_read_buf_.get(), kWsReadChunk,
          base::BindOnce(&LeafClientSocket::OnVisionTransportRead,
                         weak_factory_.GetWeakPtr()));
      if (rv == ERR_IO_PENDING) {
        vision_pending_user_buf_ = scoped_refptr<IOBuffer>(buf);
        vision_pending_user_len_ = buf_len;
        pending_read_callback_ = std::move(callback);
        return ERR_IO_PENDING;
      }
      if (rv <= 0) {
        return rv == 0 ? ERR_CONNECTION_CLOSED : rv;
      }
      std::vector<uint8_t> fed = vision_parser_->Feed(
          base::span<const uint8_t>(
              reinterpret_cast<const uint8_t*>(handshake_read_buf_->data()),
              static_cast<size_t>(rv)));
      vision_rx_queue_.insert(vision_rx_queue_.end(), fed.begin(), fed.end());
    }
  }
  if (vmess_engine_) {
    while (true) {
      if (!vmess_rx_plain_.empty()) {
        const int n = static_cast<int>(std::min(
            static_cast<size_t>(buf_len), vmess_rx_plain_.size()));
        memcpy(buf->data(), vmess_rx_plain_.data(), static_cast<size_t>(n));
        vmess_rx_plain_.erase(
            vmess_rx_plain_.begin(),
            vmess_rx_plain_.begin() + static_cast<ptrdiff_t>(n));
        return n;
      }
      if (!vmess_read_buf_.get()) {
        vmess_read_buf_ =
            base::MakeRefCounted<IOBufferWithSize>(kWsReadChunk);
      }
      int rv = transport_socket_->Read(
          vmess_read_buf_.get(), kWsReadChunk,
          base::BindOnce(&LeafClientSocket::OnVmessTransportRead,
                         weak_factory_.GetWeakPtr(),
                         scoped_refptr<IOBuffer>(buf), buf_len));
      if (rv == ERR_IO_PENDING) {
        pending_read_callback_ = std::move(callback);
        return ERR_IO_PENDING;
      }
      if (rv <= 0) {
        return rv == 0 ? ERR_CONNECTION_CLOSED : rv;
      }
      std::vector<uint8_t> dec;
      if (!vmess_engine_->FeedCipherText(
              base::span<const uint8_t>(
                  reinterpret_cast<const uint8_t*>(vmess_read_buf_->data()),
                  static_cast<size_t>(rv)),
              &dec)) {
        LOG(ERROR) << "[LEAF_PROXY_DEBUG] VMess FeedCipherText failed "
                   << "(sync transport read) ct_len=" << rv;
        return ERR_FAILED;
      }
      vmess_rx_plain_.insert(vmess_rx_plain_.end(), dec.begin(), dec.end());
    }
  }
#endif
  // VLESS response: 1-byte version, 1-byte addon length, then |addon| opaque
  // bytes (same layout as LeafVlessStripServerResponse for the WS path).
  while (need_strip_vless_response_) {
    if (handshake_tcp_prefix_read_ < 2) {
      const int need = 2 - handshake_tcp_prefix_read_;
      handshake_read_buf_ = base::MakeRefCounted<IOBufferWithSize>(need);
      int rv = transport_socket_->Read(
          handshake_read_buf_.get(), need,
          base::BindOnce(&LeafClientSocket::OnTcpVlessStripReadComplete,
                         weak_factory_.GetWeakPtr(), scoped_refptr<IOBuffer>(buf),
                         buf_len));
      if (rv == ERR_IO_PENDING) {
        pending_read_callback_ = std::move(callback);
        return ERR_IO_PENDING;
      }
      if (rv <= 0) {
        return rv == 0 ? ERR_CONNECTION_CLOSED : rv;
      }
      memcpy(handshake_tcp_prefix_.data() + handshake_tcp_prefix_read_,
             handshake_read_buf_->data(), static_cast<size_t>(rv));
      handshake_tcp_prefix_read_ += rv;
      continue;
    }

    DCHECK_EQ(handshake_tcp_prefix_read_, 2);
    if (vless_tcp_addon_remaining_ == -1) {
      vless_tcp_addon_remaining_ = static_cast<int>(handshake_tcp_prefix_[1]);
    }
    if (vless_tcp_addon_remaining_ == 0) {
      need_strip_vless_response_ = false;
      handshake_tcp_prefix_read_ = 0;
      vless_tcp_addon_remaining_ = -1;
      handshake_read_buf_ = nullptr;
      return transport_socket_->Read(buf, buf_len, std::move(callback));
    }

    DCHECK_GT(vless_tcp_addon_remaining_, 0);
    const int need = vless_tcp_addon_remaining_;
    handshake_read_buf_ = base::MakeRefCounted<IOBufferWithSize>(need);
    int rv = transport_socket_->Read(
        handshake_read_buf_.get(), need,
        base::BindOnce(&LeafClientSocket::OnTcpVlessStripReadComplete,
                       weak_factory_.GetWeakPtr(), scoped_refptr<IOBuffer>(buf),
                       buf_len));
    if (rv == ERR_IO_PENDING) {
      pending_read_callback_ = std::move(callback);
      return ERR_IO_PENDING;
    }
    if (rv <= 0) {
      return rv == 0 ? ERR_CONNECTION_CLOSED : rv;
    }
    vless_tcp_addon_remaining_ -= rv;
  }

  return transport_socket_->Read(buf, buf_len, std::move(callback));
}

void LeafClientSocket::OnTcpStripFinalReadComplete(int result) {
  std::move(pending_read_callback_).Run(result);
}

#if BUILDFLAG(ENABLE_CHROMIUM_LEAF)
void LeafClientSocket::OnVisionTransportRead(int result) {
  DCHECK(!pending_read_callback_.is_null());
  DCHECK(vision_pending_user_buf_.get());
  DCHECK(vision_parser_);

  if (result <= 0) {
    vision_pending_user_buf_ = nullptr;
    std::move(pending_read_callback_)
        .Run(result == 0 ? ERR_CONNECTION_CLOSED : result);
    return;
  }

  std::vector<uint8_t> fed = vision_parser_->Feed(
      base::span<const uint8_t>(
          reinterpret_cast<const uint8_t*>(handshake_read_buf_->data()),
          static_cast<size_t>(result)));
  vision_rx_queue_.insert(vision_rx_queue_.end(), fed.begin(), fed.end());

  if (vision_rx_queue_.empty()) {
    int rv = transport_socket_->Read(
        handshake_read_buf_.get(), kWsReadChunk,
        base::BindOnce(&LeafClientSocket::OnVisionTransportRead,
                       weak_factory_.GetWeakPtr()));
    if (rv != ERR_IO_PENDING) {
      OnVisionTransportRead(rv);
    }
    return;
  }

  const int n = std::min(vision_pending_user_len_,
                         static_cast<int>(vision_rx_queue_.size()));
  memcpy(vision_pending_user_buf_->data(), vision_rx_queue_.data(),
         static_cast<size_t>(n));
  vision_rx_queue_.erase(
      vision_rx_queue_.begin(),
      vision_rx_queue_.begin() + static_cast<ptrdiff_t>(n));
  vision_pending_user_buf_ = nullptr;
  std::move(pending_read_callback_).Run(n);
}
#endif  // ENABLE_CHROMIUM_LEAF

int LeafClientSocket::ReadWithWsFraming(IOBuffer* buf,
                                        int buf_len,
                                        CompletionOnceCallback callback) {
  if (buf_len <= 0) {
    return ERR_INVALID_ARGUMENT;
  }

  if (!ws_read_buf_.get()) {
    ws_read_buf_ = base::MakeRefCounted<IOBufferWithSize>(kWsReadChunk);
  }

  while (true) {
    size_t to_copy =
        std::min(static_cast<size_t>(buf_len), pending_plaintext_.size());
    if (to_copy > 0) {
      // If we still need to strip the VLESS response header, do it now
      // from the first data we're about to deliver.
      if (need_strip_vless_response_) {
        need_strip_vless_response_ = false;
        if (!LeafVlessStripServerResponse(&pending_plaintext_)) {
          LOG(ERROR) << "[LEAF_PROXY_DEBUG] LeafVlessStripServerResponse failed "
                     << "(WS sync read) pending_len=" << pending_plaintext_.size();
          return ERR_FAILED;
        }
        // Re-check: stripping may have consumed all data.
        to_copy =
            std::min(static_cast<size_t>(buf_len), pending_plaintext_.size());
        if (to_copy == 0) {
          continue;  // Need to read more data from transport.
        }
      }
      memcpy(buf->data(), pending_plaintext_.data(), to_copy);
      pending_plaintext_.erase(
          pending_plaintext_.begin(),
          pending_plaintext_.begin() + static_cast<ptrdiff_t>(to_copy));
      return static_cast<int>(to_copy);
    }

    while (PullNextCompleteWsMessageIntoPending()) {
      to_copy =
          std::min(static_cast<size_t>(buf_len), pending_plaintext_.size());
      if (to_copy > 0) {
        memcpy(buf->data(), pending_plaintext_.data(), to_copy);
        pending_plaintext_.erase(
            pending_plaintext_.begin(),
            pending_plaintext_.begin() + static_cast<ptrdiff_t>(to_copy));
        return static_cast<int>(to_copy);
      }
    }

    int rv = transport_socket_->Read(
        ws_read_buf_.get(), kWsReadChunk,
        base::BindOnce(&LeafClientSocket::OnWsTransportRead,
                       weak_factory_.GetWeakPtr()));
    if (rv == ERR_IO_PENDING) {
      ws_user_read_dst_ = buf->data();
      ws_user_read_len_ = buf_len;
      pending_read_callback_ = std::move(callback);
      return ERR_IO_PENDING;
    }
    if (rv <= 0) {
      return rv;
    }

    ws_rx_accumulator_.insert(ws_rx_accumulator_.end(), ws_read_buf_->data(),
                              ws_read_buf_->data() + rv);
  }
}

int LeafClientSocket::Read(IOBuffer* buf,
                           int buf_len,
                           CompletionOnceCallback callback) {
  if (!completed_handshake_ || !transport_socket_) {
    return ERR_UNEXPECTED;
  }
  DCHECK(pending_read_callback_.is_null());
  DCHECK(!callback.is_null());
  if (!ws_framing_) {
    return ReadWithoutFraming(buf, buf_len, std::move(callback));
  }
  return ReadWithWsFraming(buf, buf_len, std::move(callback));
}

int LeafClientSocket::ReadIfReady(IOBuffer* buf,
                                  int buf_len,
                                  CompletionOnceCallback callback) {
  if (!completed_handshake_ || !transport_socket_) {
    return ERR_UNEXPECTED;
  }
  // SocketBIOAdapter (TLS on this socket) prefers ReadIfReady(). Our WS path
  // only implements Read(): returning OK here is interpreted as 0 bytes / EOF,
  // and returning ERR_IO_PENDING without scheduling a callback deadlocks the
  // BIO. Defer to Read() like other sockets without a true peek-style read.
  if (ws_framing_) {
    return ERR_READ_IF_READY_NOT_IMPLEMENTED;
  }
  // TCP path must strip the VLESS response via Read(); do not peek past it.
  if (need_strip_vless_response_) {
    return ERR_READ_IF_READY_NOT_IMPLEMENTED;
  }
#if BUILDFLAG(ENABLE_CHROMIUM_LEAF)
  if (use_vision_) {
    return ERR_READ_IF_READY_NOT_IMPLEMENTED;
  }
  if (vmess_engine_) {
    return ERR_READ_IF_READY_NOT_IMPLEMENTED;
  }
#endif
  return transport_socket_->ReadIfReady(buf, buf_len, std::move(callback));
}

int LeafClientSocket::CancelReadIfReady() {
  if (!completed_handshake_ || !transport_socket_) {
    return ERR_UNEXPECTED;
  }
  if (ws_framing_) {
    return ERR_READ_IF_READY_NOT_IMPLEMENTED;
  }
  if (need_strip_vless_response_) {
    return ERR_READ_IF_READY_NOT_IMPLEMENTED;
  }
#if BUILDFLAG(ENABLE_CHROMIUM_LEAF)
  if (use_vision_) {
    return ERR_READ_IF_READY_NOT_IMPLEMENTED;
  }
  if (vmess_engine_) {
    return ERR_READ_IF_READY_NOT_IMPLEMENTED;
  }
#endif
  return transport_socket_->CancelReadIfReady();
}

int LeafClientSocket::Write(IOBuffer* buf,
                            int buf_len,
                            CompletionOnceCallback callback,
                            const NetworkTrafficAnnotationTag& traffic_annotation) {
  if (!completed_handshake_ || !transport_socket_) {
    return ERR_UNEXPECTED;
  }
  DCHECK(!callback.is_null());
#if BUILDFLAG(ENABLE_CHROMIUM_LEAF)
  if (vmess_engine_) {
    if (buf_len <= 0) {
      return ERR_INVALID_ARGUMENT;
    }
    vmess_engine_->QueuePlainText(base::span(
        reinterpret_cast<const uint8_t*>(buf->data()),
        static_cast<size_t>(buf_len)));
    vmess_pending_user_write_len_ = buf_len;
    DCHECK(vmess_pending_write_callback_.is_null());
    vmess_pending_write_callback_ = std::move(callback);
    int rv = FlushVmessCipherWrites(traffic_annotation);
    if (rv != ERR_IO_PENDING) {
      vmess_pending_write_callback_.Reset();
      return rv == OK ? buf_len : rv;
    }
    return ERR_IO_PENDING;
  }
#endif
  if (!ws_framing_) {
    return transport_socket_->Write(buf, buf_len, std::move(callback),
                                     traffic_annotation);
  }
  DCHECK(ws_pending_write_callback_.is_null());
  if (buf_len <= 0) {
    return ERR_INVALID_ARGUMENT;
  }
  auto frame = BuildMaskedWsBinaryFrame(base::span(
      reinterpret_cast<const uint8_t*>(buf->data()),
      static_cast<size_t>(buf_len)));
  if (frame.empty()) {
    return ERR_INVALID_ARGUMENT;
  }

  auto vb = base::MakeRefCounted<VectorIOBuffer>(std::move(frame));
  const int frame_size = static_cast<int>(vb->size());
  ws_pending_write_buf_ = base::MakeRefCounted<DrainableIOBuffer>(std::move(vb), frame_size);
  ws_pending_write_original_len_ = buf_len;

  int rv = transport_socket_->Write(
      ws_pending_write_buf_.get(), ws_pending_write_buf_->BytesRemaining(),
      base::BindOnce(&LeafClientSocket::OnWsTransportWrite,
                     weak_factory_.GetWeakPtr()),
      traffic_annotation);
  if (rv != ERR_IO_PENDING) {
    if (rv > 0) {
      ws_pending_write_buf_->DidConsume(rv);
      if (ws_pending_write_buf_->BytesRemaining() > 0) {
        // Partial synchronous write. We must NOT return to the user yet
        // because we haven't finished the frame. Defer to the async handler.
        base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
            FROM_HERE, base::BindOnce(&LeafClientSocket::OnWsTransportWrite,
                                     weak_factory_.GetWeakPtr(), OK));
        ws_pending_write_callback_ = std::move(callback);
        return ERR_IO_PENDING;
      }
    }
    // Done or error
    ws_pending_write_buf_ = nullptr;
    return rv > 0 ? buf_len : rv;
  }
  ws_pending_write_callback_ = std::move(callback);
  return ERR_IO_PENDING;
}

void LeafClientSocket::OnWsTransportWrite(int result) {
  if (result > 0) {
    ws_pending_write_buf_->DidConsume(result);
    if (ws_pending_write_buf_->BytesRemaining() > 0) {
      int rv = transport_socket_->Write(
          ws_pending_write_buf_.get(), ws_pending_write_buf_->BytesRemaining(),
          base::BindOnce(&LeafClientSocket::OnWsTransportWrite,
                         weak_factory_.GetWeakPtr()),
          traffic_annotation_);
      if (rv != ERR_IO_PENDING) {
        OnWsTransportWrite(rv);
      }
      return;
    }
  }

  ws_pending_write_buf_ = nullptr;
  int original_len = ws_pending_write_original_len_;
  if (!ws_pending_write_callback_.is_null()) {
    std::move(ws_pending_write_callback_)
        .Run(result > 0 ? original_len : result);
  }
}

#if BUILDFLAG(ENABLE_CHROMIUM_LEAF)
void LeafClientSocket::OnVmessTransportRead(scoped_refptr<IOBuffer> user_buf,
                                              int user_buf_len,
                                              int result) {
  DCHECK(!pending_read_callback_.is_null());
  if (result <= 0) {
    std::move(pending_read_callback_)
        .Run(result == 0 ? ERR_CONNECTION_CLOSED : result);
    return;
  }
  std::vector<uint8_t> dec;
  if (!vmess_engine_->FeedCipherText(
          base::span<const uint8_t>(
              reinterpret_cast<const uint8_t*>(vmess_read_buf_->data()),
              static_cast<size_t>(result)),
          &dec)) {
    LOG(ERROR) << "[LEAF_PROXY_DEBUG] VMess FeedCipherText failed "
               << "(async OnVmessTransportRead) ct_len=" << result;
    std::move(pending_read_callback_).Run(ERR_FAILED);
    return;
  }
  vmess_rx_plain_.insert(vmess_rx_plain_.end(), dec.begin(), dec.end());
  if (vmess_rx_plain_.empty()) {
    int rv = transport_socket_->Read(
        vmess_read_buf_.get(), kWsReadChunk,
        base::BindOnce(&LeafClientSocket::OnVmessTransportRead,
                       weak_factory_.GetWeakPtr(), std::move(user_buf),
                       user_buf_len));
    if (rv != ERR_IO_PENDING) {
      OnVmessTransportRead(std::move(user_buf), user_buf_len, rv);
    }
    return;
  }
  const int n = std::min(
      user_buf_len, static_cast<int>(vmess_rx_plain_.size()));
  memcpy(user_buf->data(), vmess_rx_plain_.data(), static_cast<size_t>(n));
  vmess_rx_plain_.erase(
      vmess_rx_plain_.begin(),
      vmess_rx_plain_.begin() + static_cast<ptrdiff_t>(n));
  std::move(pending_read_callback_).Run(n);
}

int LeafClientSocket::FlushVmessCipherWrites(
    const NetworkTrafficAnnotationTag& traffic_annotation) {
  while (true) {
    if (!vmess_pending_write_buf_.get()) {
      std::vector<uint8_t> chunk;
      if (!vmess_engine_->ProduceCipherText(&chunk)) {
        LOG(ERROR) << "[LEAF_PROXY_DEBUG] VMess ProduceCipherText failed "
                   << "(flush write)";
        return ERR_FAILED;
      }
      if (chunk.empty()) {
        return OK;
      }
      std::vector<uint8_t> wire = ws_framing_
                                      ? BuildMaskedWsBinaryFrame(chunk)
                                      : std::move(chunk);
      if (ws_framing_ && wire.empty()) {
        return ERR_INVALID_ARGUMENT;
      }
      auto vb = base::MakeRefCounted<VectorIOBuffer>(std::move(wire));
      const int sz = static_cast<int>(vb->size());
      vmess_pending_write_buf_ =
          base::MakeRefCounted<DrainableIOBuffer>(std::move(vb), sz);
    }
    int rv =
        transport_socket_->Write(vmess_pending_write_buf_.get(),
                                 vmess_pending_write_buf_->BytesRemaining(),
                                 base::BindOnce(&LeafClientSocket::OnVmessTransportWrite,
                                                weak_factory_.GetWeakPtr()),
                                 traffic_annotation);
    if (rv == ERR_IO_PENDING) {
      return ERR_IO_PENDING;
    }
    if (rv < 0) {
      vmess_pending_write_buf_ = nullptr;
      return rv;
    }
    vmess_pending_write_buf_->DidConsume(rv);
    if (vmess_pending_write_buf_->BytesRemaining() > 0) {
      base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE,
          base::BindOnce(&LeafClientSocket::OnVmessTransportWrite,
                         weak_factory_.GetWeakPtr(), OK));
      return ERR_IO_PENDING;
    }
    vmess_pending_write_buf_ = nullptr;
  }
}

void LeafClientSocket::OnVmessTransportWrite(int result) {
  if (result > 0) {
    vmess_pending_write_buf_->DidConsume(result);
    if (vmess_pending_write_buf_->BytesRemaining() > 0) {
      int rv = transport_socket_->Write(
          vmess_pending_write_buf_.get(),
          vmess_pending_write_buf_->BytesRemaining(),
          base::BindOnce(&LeafClientSocket::OnVmessTransportWrite,
                         weak_factory_.GetWeakPtr()),
          traffic_annotation_);
      if (rv != ERR_IO_PENDING) {
        OnVmessTransportWrite(rv);
      }
      return;
    }
  }

  vmess_pending_write_buf_ = nullptr;

  if (result <= 0) {
    if (!vmess_pending_write_callback_.is_null()) {
      std::move(vmess_pending_write_callback_).Run(result);
    }
    return;
  }

  int rv = FlushVmessCipherWrites(traffic_annotation_);
  if (rv == ERR_IO_PENDING) {
    return;
  }
  const int original = vmess_pending_user_write_len_;
  if (!vmess_pending_write_callback_.is_null()) {
    std::move(vmess_pending_write_callback_).Run(rv == OK ? original : rv);
  }
}
#endif  // BUILDFLAG(ENABLE_CHROMIUM_LEAF)

int LeafClientSocket::SetReceiveBufferSize(int32_t size) {
  if (!transport_socket_) {
    return ERR_UNEXPECTED;
  }
  return transport_socket_->SetReceiveBufferSize(size);
}

int LeafClientSocket::SetSendBufferSize(int32_t size) {
  if (!transport_socket_) {
    return ERR_UNEXPECTED;
  }
  return transport_socket_->SetSendBufferSize(size);
}

int LeafClientSocket::GetPeerAddress(IPEndPoint* address) const {
  if (!transport_socket_) {
    return ERR_UNEXPECTED;
  }
  return transport_socket_->GetPeerAddress(address);
}

int LeafClientSocket::GetLocalAddress(IPEndPoint* address) const {
  if (!transport_socket_) {
    return ERR_UNEXPECTED;
  }
  return transport_socket_->GetLocalAddress(address);
}

}  // namespace net
