// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// VMess AEAD client request + chunked stream (aligned with Leaf
// leaf/src/proxy/vmess). C++ implementation using BoringSSL / zlib CRC32.

#ifndef NET_SOCKET_LEAF_VMESS_ENGINE_H_
#define NET_SOCKET_LEAF_VMESS_ENGINE_H_

#include <stdint.h>

#include <array>
#include <cstddef>
#include <string_view>
#include <vector>

#include "base/containers/span.h"
#include "base/memory/raw_ptr.h"
#include "net/base/net_export.h"

namespace net::chromium_leaf {

enum class VmessSecurityCipher : uint8_t {
  kAes128Gcm = 0x03,
  kChacha20Poly1305 = 0x04,
};

struct VmessSessionMaterial {
  std::array<uint8_t, 16> request_body_key{};
  std::array<uint8_t, 16> request_body_iv{};
  std::array<uint8_t, 16> response_body_key{};
  std::array<uint8_t, 16> response_body_iv{};
  uint8_t response_header = 0;
  VmessSecurityCipher security = VmessSecurityCipher::kChacha20Poly1305;
};

// Builds VMess AEAD request bytes (first wire payload). `security` selects body
// cipher (default Leaf outbound: chacha20-poly1305). Returns false on failure.
bool LeafVmessBuildClientRequest(
    const std::array<uint8_t, 16>& uuid,
    std::string_view dest_host,
    uint16_t dest_port,
    std::string_view security_cipher_name,
    std::vector<uint8_t>* handshake_out,
    VmessSessionMaterial* session_out);

class NET_EXPORT_PRIVATE LeafVmessStreamEngine {
 public:
  explicit LeafVmessStreamEngine(VmessSessionMaterial session);
  LeafVmessStreamEngine(const LeafVmessStreamEngine&) = delete;
  LeafVmessStreamEngine& operator=(const LeafVmessStreamEngine&) = delete;
  ~LeafVmessStreamEngine();

  // Feeds post-handshake ciphertext from the transport. Appends any decrypted
  // plaintext to |out_plain|.
  bool FeedCipherText(base::span<const uint8_t> data,
                      std::vector<uint8_t>* out_plain);

  void QueuePlainText(base::span<const uint8_t> data);
  bool ProduceCipherText(std::vector<uint8_t>* out_cipher);
  bool response_header_done() const { return response_header_done_; }

 private:
  enum class ReadState {
    kResponseHeader,
    kChunkLength,
    kChunkData,
    kDrainPlain
  };
  bool DecryptResponseHeader();
  bool SealPayloadChunkWithPadding(base::span<const uint8_t> plain,
                                   uint16_t padding_size,
                                   std::vector<uint8_t>* frame);

  bool AeadSealBody(std::vector<uint8_t>* in_out_plain_then_cipher);
  bool AeadOpenBody(std::vector<uint8_t>* in_out);

  std::array<uint8_t, 12> NextEncNonce();
  std::array<uint8_t, 12> NextDecNonce();

  VmessSessionMaterial session_;
  std::vector<uint8_t> rx_queue_;
  ReadState read_state_ = ReadState::kResponseHeader;
  uint16_t chunk_length_pending_ = 0;
  size_t chunk_padding_ = 0;
  size_t chunk_got_ = 0;
  std::vector<uint8_t> chunk_buf_;
  std::vector<uint8_t> plain_drain_;
  bool response_header_done_ = false;
  bool read_failed_ = false;

  std::vector<uint8_t> tx_plain_;
  uint16_t enc_chunk_nonce_ = 0xffff;
  uint16_t dec_chunk_nonce_ = 0xffff;

  // Opaque shake state (LeafVmessShakeMask in .cc); void* until type is local to
  // the .cc translation unit.
  raw_ptr<void> enc_shake_;
  raw_ptr<void> dec_shake_;

  int resp_hdr_phase_ = 0;
  size_t resp_inner_len_ = 0;

  static constexpr size_t kTagLen = 16;
  static constexpr size_t kMaxPayload = 0x4000;
};

}  // namespace net::chromium_leaf

#endif  // NET_SOCKET_LEAF_VMESS_ENGINE_H_
