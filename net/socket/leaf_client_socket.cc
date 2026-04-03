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
#include "build/buildflag.h"
#include "net/base/io_buffer.h"
#include "net/base/net_errors.h"
#include "net/net_buildflags.h"
#include "net/socket/chromium_leaf_vless_handshake.h"
#include "net/socket/next_proto.h"
#include "net/socket/socket_tag.h"

namespace net {

namespace {

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
  LOG(ERROR) << "[LEAF_PROXY_DEBUG] LeafClientSocket::Connect protocol="
             << static_cast<int>(protocol_) << " dest=" << destination_.ToString()
             << " query_len=" << leaf_uri_query_.size()
             << " fragment_len=" << leaf_uri_fragment_.size();
#endif

  net_log_.BeginEvent(NetLogEventType::LEAF_PROXY_CONNECT);

  if (protocol_ != LeafOutboundProtocol::kVless) {
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

  handshake_vless_hdr_ =
      LeafVlessBuildRequestHeader(uuid, destination_.host(), destination_.port());
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
    handshake_ws_http_str_ = LeafVlessBuildWebSocketUpgradeRequest(
        path, ws_host, sec_key);
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
    }
  }
#if BUILDFLAG(ENABLE_CHROMIUM_LEAF)
  LOG(ERROR) << "[LEAF_PROXY_DEBUG] LeafClientSocket::Connect handshake "
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
    }
    handshake_state_ = HandshakeState::kNone;
#if BUILDFLAG(ENABLE_CHROMIUM_LEAF)
    LOG(ERROR) << "[LEAF_PROXY_DEBUG] LeafClientSocket handshake async done "
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
        // VLESS server won't reply until the destination server sends data
        // back, which requires the upper layer (TLS) to send its ClientHello
        // first.  Mark that we need to strip the 2-byte VLESS response prefix
        // on the first Read(), and declare the handshake complete now.
        need_strip_vless_response_ = true;
        return OK;
      }
      case HandshakeState::kTcpReadPrefix: {
        DCHECK_EQ(OK, rv);
        const int need = 2 - handshake_tcp_prefix_read_;
        handshake_read_buf_ = base::MakeRefCounted<IOBufferWithSize>(need);
        handshake_state_ = HandshakeState::kTcpReadPrefixComplete;
        rv = transport_socket_->Read(handshake_read_buf_.get(), need,
                                       handshake_io_callback_);
        break;
      }
      case HandshakeState::kTcpReadPrefixComplete: {
        if (rv < 0) {
          return rv;
        }
        if (rv == 0) {
          return ERR_CONNECTION_CLOSED;
        }
        memcpy(handshake_tcp_prefix_.data() + handshake_tcp_prefix_read_,
               handshake_read_buf_->data(), static_cast<size_t>(rv));
        handshake_tcp_prefix_read_ += rv;
        if (handshake_tcp_prefix_read_ < 2) {
          handshake_state_ = HandshakeState::kTcpReadPrefix;
          rv = OK;
          break;
        }
        if (handshake_tcp_prefix_read_ != 2) {
          return ERR_FAILED;
        }
        const uint8_t addon_len = handshake_tcp_prefix_[1];
        if (addon_len == 0) {
          return OK;
        }
        handshake_read_buf_ =
            base::MakeRefCounted<IOBufferWithSize>(static_cast<int>(addon_len));
        handshake_state_ = HandshakeState::kTcpReadAddonComplete;
        rv = transport_socket_->Read(handshake_read_buf_.get(),
                                     static_cast<int>(addon_len),
                                     handshake_io_callback_);
        break;
      }
      case HandshakeState::kTcpReadAddonComplete: {
        if (rv < 0) {
          return rv;
        }
        if (rv == 0) {
          return ERR_CONNECTION_CLOSED;
        }
        if (rv != handshake_read_buf_->size()) {
          return ERR_CONNECTION_CLOSED;
        }
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
        if (!base::EndsWith(handshake_http_headers_, "\r\n\r\n",
                           base::CompareCase::SENSITIVE)) {
          handshake_state_ = HandshakeState::kWsReadHeaders;
          rv = OK;
          break;
        }
        if (!LeafVlessHttpResponseFirstLineIs101(handshake_http_headers_)) {
#if BUILDFLAG(ENABLE_CHROMIUM_LEAF)
          LOG(ERROR) << "[LEAF_PROXY_DEBUG] Leaf ws not 101 snippet="
                     << handshake_http_headers_.substr(
                            0, std::min<size_t>(handshake_http_headers_.size(),
                                               400u));
#endif
          return ERR_FAILED;
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
        // Same as TCP path: the VLESS server doesn't reply until the
        // destination has data.  Complete the handshake now and defer
        // stripping the VLESS response to the first ReadWithWsFraming().
        need_strip_vless_response_ = true;
        return OK;
      }

      case HandshakeState::kWsReadFirstFrame: {
        DCHECK_EQ(OK, rv);
        handshake_read_buf_ =
            base::MakeRefCounted<IOBufferWithSize>(kWsReadChunk);
        handshake_state_ = HandshakeState::kWsReadFirstFrameComplete;
        rv = transport_socket_->Read(handshake_read_buf_.get(), kWsReadChunk,
                                     handshake_io_callback_);
        break;
      }
      case HandshakeState::kWsReadFirstFrameComplete: {
        if (rv < 0) {
          return rv;
        }
        if (rv == 0) {
          return ERR_CONNECTION_CLOSED;
        }
        ws_rx_accumulator_.insert(
            ws_rx_accumulator_.end(), handshake_read_buf_->data(),
            handshake_read_buf_->data() + rv);
        bool need_more = false;
        while (true) {
          std::vector<uint8_t> payload;
          bool fin = false;
          uint8_t opcode = 0;
          if (!PopOneWebSocketFrame(&ws_rx_accumulator_, &payload, &fin,
                                    &opcode)) {
            handshake_state_ = HandshakeState::kWsReadFirstFrame;
            rv = OK;
            need_more = true;
            break;
          }
          if (opcode == 0x9 || opcode == 0xA) {
            continue;
          }
          if (opcode != 0x2) {
            return ERR_FAILED;
          }
          if (!LeafVlessStripServerResponse(&payload)) {
            return ERR_FAILED;
          }
          pending_plaintext_ = std::move(payload);
          return OK;
        }
        if (!need_more) {
          NOTREACHED();
        }
        break;
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
  handshake_state_ = HandshakeState::kNone;
  user_connect_callback_.Reset();
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
  ws_user_read_callback_.Reset();
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
        pending_plaintext_.insert(pending_plaintext_.end(), payload.begin(),
                                  payload.end());
        return true;
      }
      ws_message_fragment_ = std::move(payload);
      ws_in_fragment_message_ = true;
      continue;
    }
    if (opcode == 0x0 && ws_in_fragment_message_) {
      ws_message_fragment_.insert(ws_message_fragment_.end(), payload.begin(),
                                  payload.end());
      if (fin) {
        pending_plaintext_.swap(ws_message_fragment_);
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
  CHECK(ws_user_read_callback_);

  if (result <= 0) {
    std::move(ws_user_read_callback_).Run(result);
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
        std::move(ws_user_read_callback_).Run(ERR_FAILED);
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
    std::move(ws_user_read_callback_).Run(n);
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

int LeafClientSocket::ReadWithoutFraming(IOBuffer* buf,
                                           int buf_len,
                                           CompletionOnceCallback callback) {
  // On the very first read after handshake, the VLESS server prepends a
  // 2-byte response header (version + addon-length) to the stream.  We
  // must read and discard it before delivering application data.
  if (need_strip_vless_response_) {
    // We haven't stripped the prefix yet.  Read the 2-byte prefix first.
    const int need = 2 - handshake_tcp_prefix_read_;
    if (need > 0) {
      if (!handshake_read_buf_) {
        handshake_read_buf_ = base::MakeRefCounted<IOBufferWithSize>(need);
      }
      int rv = transport_socket_->Read(
          handshake_read_buf_.get(), need,
          base::BindOnce(
              &LeafClientSocket::OnTcpStripVlessResponseComplete,
              weak_factory_.GetWeakPtr(), scoped_refptr<IOBuffer>(buf),
              buf_len));
      if (rv == ERR_IO_PENDING) {
        ws_user_read_callback_ = std::move(callback);
        return ERR_IO_PENDING;
      }
      if (rv <= 0) {
        return rv == 0 ? ERR_CONNECTION_CLOSED : rv;
      }
      handshake_tcp_prefix_read_ += rv;
      if (handshake_tcp_prefix_read_ < 2) {
        // Didn't get both bytes yet – recurse.
        handshake_read_buf_ = nullptr;
        return ReadWithoutFraming(buf, buf_len, std::move(callback));
      }
    }
    // Got the 2-byte prefix.  Check for addon data.
    // Byte 0 = version (ignored), byte 1 = addon length.
    // For simplicity skip addon bytes (very rare, usually 0).
    need_strip_vless_response_ = false;
    handshake_read_buf_ = nullptr;
    handshake_tcp_prefix_read_ = 0;
  }
  return transport_socket_->Read(buf, buf_len, std::move(callback));
}

void LeafClientSocket::OnTcpStripVlessResponseComplete(
    scoped_refptr<IOBuffer> user_buf,
    int user_buf_len,
    int result) {
  if (result <= 0) {
    std::move(ws_user_read_callback_)
        .Run(result == 0 ? ERR_CONNECTION_CLOSED : result);
    return;
  }
  handshake_tcp_prefix_read_ += result;
  if (handshake_tcp_prefix_read_ < 2) {
    // Need more bytes for the 2-byte prefix.
    const int need = 2 - handshake_tcp_prefix_read_;
    handshake_read_buf_ = base::MakeRefCounted<IOBufferWithSize>(need);
    int rv = transport_socket_->Read(
        handshake_read_buf_.get(), need,
        base::BindOnce(
            &LeafClientSocket::OnTcpStripVlessResponseComplete,
            weak_factory_.GetWeakPtr(), user_buf, user_buf_len));
    if (rv != ERR_IO_PENDING) {
      OnTcpStripVlessResponseComplete(user_buf, user_buf_len, rv);
    }
    return;
  }
  // Got the full 2-byte prefix — done stripping.
  need_strip_vless_response_ = false;
  handshake_read_buf_ = nullptr;
  handshake_tcp_prefix_read_ = 0;
  // Now do the real read for the application data.
  // Use an intermediate callback so we can handle synchronous completion.
  int rv = transport_socket_->Read(
      user_buf.get(), user_buf_len,
      base::BindOnce(&LeafClientSocket::OnTcpStripFinalReadComplete,
                     weak_factory_.GetWeakPtr()));
  if (rv != ERR_IO_PENDING) {
    std::move(ws_user_read_callback_).Run(rv);
  }
}

void LeafClientSocket::OnTcpStripFinalReadComplete(int result) {
  std::move(ws_user_read_callback_).Run(result);
}

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
      ws_user_read_callback_ = std::move(callback);
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
  return transport_socket_->ReadIfReady(buf, buf_len, std::move(callback));
}

int LeafClientSocket::CancelReadIfReady() {
  if (!completed_handshake_ || !transport_socket_) {
    return ERR_UNEXPECTED;
  }
  if (ws_framing_) {
    return ERR_READ_IF_READY_NOT_IMPLEMENTED;
  }
  return transport_socket_->CancelReadIfReady();
}

int LeafClientSocket::Write(IOBuffer* buf,
                            int buf_len,
                            CompletionOnceCallback callback,
                            const NetworkTrafficAnnotationTag& traffic_annotation) {
  if (!completed_handshake_ || !transport_socket_) {
    return ERR_UNEXPECTED;
  }
  if (!ws_framing_) {
    return transport_socket_->Write(buf, buf_len, std::move(callback),
                                     traffic_annotation);
  }
  if (buf_len <= 0) {
    return ERR_INVALID_ARGUMENT;
  }
  auto frame = BuildMaskedWsBinaryFrame(base::span(
      reinterpret_cast<const uint8_t*>(buf->data()),
      static_cast<size_t>(buf_len)));
  if (frame.empty()) {
    return ERR_INVALID_ARGUMENT;
  }
  // Store the frame buffer as a member so it survives async writes.
  ws_pending_write_buf_ =
      base::MakeRefCounted<IOBufferWithSize>(static_cast<int>(frame.size()));
  memcpy(ws_pending_write_buf_->data(), frame.data(), frame.size());
  ws_pending_write_original_len_ = buf_len;
  int rv = transport_socket_->Write(
      ws_pending_write_buf_.get(), static_cast<int>(frame.size()),
      base::BindOnce(&LeafClientSocket::OnWsTransportWrite,
                     weak_factory_.GetWeakPtr()),
      traffic_annotation);
  if (rv != ERR_IO_PENDING) {
    ws_pending_write_buf_ = nullptr;
    // Report application-level bytes on success, not frame-level bytes.
    return rv > 0 ? buf_len : rv;
  }
  ws_pending_write_callback_ = std::move(callback);
  return ERR_IO_PENDING;
}

void LeafClientSocket::OnWsTransportWrite(int result) {
  ws_pending_write_buf_ = nullptr;
  int original_len = ws_pending_write_original_len_;
  std::move(ws_pending_write_callback_)
      .Run(result > 0 ? original_len : result);
}

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
