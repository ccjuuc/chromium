// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// VMess AEAD client (ported from Leaf leaf/src/proxy/vmess). SHAKE128
// permutation adapted from tiny_sha3 by Markku-Juhani O. Saarinen (see
// https://github.com/mjosaarinen/tiny_sha3).

#include "net/socket/leaf_vmess_engine.h"

#include <string.h>

#include <array>
#include <functional>
#include <limits>
#include <memory>
#include <string_view>
#include <vector>

#include "base/check.h"
#include "base/containers/span.h"
#include "base/logging.h"
#include "base/notreached.h"
#include "base/time/time.h"
#include "crypto/random.h"
#include "net/base/ip_address.h"
#include "third_party/boringssl/src/include/openssl/aead.h"
#include "third_party/boringssl/src/include/openssl/aes.h"
#include "third_party/boringssl/src/include/openssl/hmac.h"
#include "third_party/boringssl/src/include/openssl/md5.h"
#include "third_party/boringssl/src/include/openssl/mem.h"
#include "third_party/boringssl/src/include/openssl/sha.h"
#include "third_party/zlib/zlib.h"

namespace net::chromium_leaf {
namespace {

#define ROTL64(x, y) (((x) << (y)) | ((x) >> (64 - (y))))
#define KECCAKF_ROUNDS 24

constexpr uint8_t kKdfSaltVmessAead[] = "VMess AEAD KDF";
constexpr uint8_t kKdfSaltAuthIdEnc[] = "AES Auth ID Encryption";
constexpr uint8_t kKdfSaltAeadRespHdrLenKey[] = "AEAD Resp Header Len Key";
constexpr uint8_t kKdfSaltAeadRespHdrLenIv[] = "AEAD Resp Header Len IV";
constexpr uint8_t kKdfSaltAeadRespHdrPayloadKey[] = "AEAD Resp Header Key";
constexpr uint8_t kKdfSaltAeadRespHdrPayloadIv[] = "AEAD Resp Header IV";
constexpr uint8_t kKdfSaltVmessHdrPayloadAeadKey[] = "VMess Header AEAD Key";
constexpr uint8_t kKdfSaltVmessHdrPayloadAeadIv[] = "VMess Header AEAD Nonce";
constexpr uint8_t kKdfSaltVmessHdrLenAeadKey[] =
    "VMess Header AEAD Key_Length";
constexpr uint8_t kKdfSaltVmessHdrLenAeadIv[] =
    "VMess Header AEAD Nonce_Length";

constexpr uint8_t kRequestOptionChunkStream = 0x01;
constexpr uint8_t kRequestOptionChunkMasking = 0x04;
constexpr uint8_t kRequestOptionGlobalPadding = 0x08;
constexpr uint8_t kRequestCommandTcp = 0x01;

constexpr uint32_t kFnvOffsetBasis = 2166136261u;
constexpr uint32_t kFnvPrime = 16777619u;
constexpr char kUuidMd5Literal[] = "c48619fe-8f02-49e0-b9e9-edf763e17e21";

void sha3_keccakf(base::span<uint64_t, 25> st) {
  constexpr std::array<uint64_t, 24> keccakf_rndc = {
      0x0000000000000001, 0x0000000000008082, 0x800000000000808a,
      0x8000000080008000, 0x000000000000808b, 0x0000000080000001,
      0x8000000080008081, 0x8000000000008009, 0x000000000000008a,
      0x0000000000000088, 0x0000000080008009, 0x000000008000000a,
      0x000000008000808b, 0x800000000000008b, 0x8000000000008089,
      0x8000000000008003, 0x8000000000008002, 0x8000000000000080,
      0x000000000000800a, 0x800000008000000a, 0x8000000080008081,
      0x8000000000008080, 0x0000000080000001, 0x8000000080008008};
  constexpr std::array<int, 24> keccakf_rotc = {
      1,  3,  6,  10, 15, 21, 28, 36, 45, 55, 2,  14,
      27, 41, 56, 8,  25, 43, 62, 18, 39, 61, 20, 44};
  constexpr std::array<int, 24> keccakf_piln = {
      10, 7,  11, 17, 18, 3,  5,  16, 8,  21, 24, 4,
      15, 23, 19, 13, 12, 2,  20, 14, 22, 9,  6,  1};
  std::array<uint64_t, 5> bc{};
  for (int r = 0; r < KECCAKF_ROUNDS; r++) {
    for (int i = 0; i < 5; i++) {
      bc[i] = st[i] ^ st[i + 5] ^ st[i + 10] ^ st[i + 15] ^ st[i + 20];
    }
    for (int i = 0; i < 5; i++) {
      uint64_t t = bc[(i + 4) % 5] ^ ROTL64(bc[(i + 1) % 5], 1);
      for (int j = 0; j < 25; j += 5) {
        st[j + i] ^= t;
      }
    }
    uint64_t t = st[1];
    for (int i = 0; i < 24; i++) {
      int j = keccakf_piln[i];
      bc[0] = st[j];
      st[j] = ROTL64(t, keccakf_rotc[i]);
      t = bc[0];
    }
    for (int j = 0; j < 25; j += 5) {
      for (int i = 0; i < 5; i++) {
        bc[i] = st[j + i];
      }
      for (int i = 0; i < 5; i++) {
        st[j + i] ^= (~bc[(i + 1) % 5]) & bc[(i + 2) % 5];
      }
    }
    st[0] ^= keccakf_rndc[r];
  }
}

struct sha3_ctx_t {
  std::array<uint64_t, 25> q{};
  int pt = 0;
  int rsiz = 0;
  int mdlen = 0;
};

void sha3_init_shake128(sha3_ctx_t* c) {
  c->q.fill(0);
  c->mdlen = 16;
  c->rsiz = 200 - 2 * c->mdlen;
  c->pt = 0;
}

void sha3_update(sha3_ctx_t* c, base::span<const uint8_t> data) {
  base::span<uint8_t> st_b = base::as_writable_bytes(base::span(c->q));
  int j = c->pt;
  for (uint8_t byte : data) {
    st_b[static_cast<size_t>(j++)] ^= byte;
    if (j >= c->rsiz) {
      sha3_keccakf(base::span(c->q));
      j = 0;
    }
  }
  c->pt = j;
}

void shake128_xof(sha3_ctx_t* c) {
  base::span<uint8_t> st_b = base::as_writable_bytes(base::span(c->q));
  st_b[static_cast<size_t>(c->pt)] ^= 0x1F;
  st_b[static_cast<size_t>(c->rsiz - 1)] ^= 0x80;
  sha3_keccakf(base::span(c->q));
  c->pt = 0;
}

void shake128_read(sha3_ctx_t* c, base::span<uint8_t> out) {
  base::span<uint8_t> st_b = base::as_writable_bytes(base::span(c->q));
  int j = c->pt;
  for (uint8_t& byte : out) {
    if (j >= c->rsiz) {
      sha3_keccakf(base::span(c->q));
      j = 0;
    }
    byte = st_b[static_cast<size_t>(j++)];
  }
  c->pt = j;
}

// --- VMess AEAD KDF (matches v2fly v2ray-core proxy/vmess/aead/kdf.go) ---

class VmessPrimHash {
 public:
  virtual ~VmessPrimHash() = default;
  virtual void Reset() = 0;
  virtual void Update(base::span<const uint8_t> data) = 0;
  virtual void SumInto(base::span<uint8_t> out) = 0;
  virtual size_t DigestSize() const = 0;
};

class VmessSha256Hash : public VmessPrimHash {
 public:
  void Reset() override { SHA256_Init(&ctx_); }
  void Update(base::span<const uint8_t> data) override {
    SHA256_Update(&ctx_, data.data(), data.size());
  }
  void SumInto(base::span<uint8_t> out) override {
    CHECK_EQ(out.size(), 32u);
    SHA256_Final(out.data(), &ctx_);
  }
  size_t DigestSize() const override { return 32; }

 private:
  SHA256_CTX ctx_{};
};

// crypto/hmac behavior used by v2ray nested KDF: New(h parent.Create, key).
class VmessGoHmac : public VmessPrimHash {
 public:
  VmessGoHmac(std::function<std::unique_ptr<VmessPrimHash>()> h,
              base::span<const uint8_t> key)
      : h_factory_(std::move(h)), key_(key.begin(), key.end()) {
    static constexpr size_t kBlock = 64;
    if (key_.size() > kBlock) {
      auto tmp = h_factory_();
      tmp->Reset();
      tmp->Update(base::span(key_));
      std::array<uint8_t, 32> digest{};
      tmp->SumInto(base::span(digest));
      key_.assign(digest.begin(), digest.end());
    }
    ipad_.fill(0x36);
    opad_.fill(0x5c);
    for (size_t i = 0; i < key_.size(); ++i) {
      ipad_[i] ^= key_[i];
      opad_[i] ^= key_[i];
    }
    Reset();
  }

  void Reset() override {
    inner_ = h_factory_();
    outer_ = h_factory_();
    inner_->Reset();
    outer_->Reset();
    inner_->Update(base::span(ipad_));
  }

  void Update(base::span<const uint8_t> data) override { inner_->Update(data); }

  void SumInto(base::span<uint8_t> out) override {
    CHECK_EQ(out.size(), DigestSize());
    std::array<uint8_t, 32> inner_digest{};
    inner_->SumInto(base::span(inner_digest));
    outer_->Reset();
    outer_->Update(base::span(opad_));
    outer_->Update(base::span(inner_digest));
    outer_->SumInto(out);
  }

  size_t DigestSize() const override { return 32; }

 private:
  std::function<std::unique_ptr<VmessPrimHash>()> h_factory_;
  std::vector<uint8_t> key_;
  std::array<uint8_t, 64> ipad_{};
  std::array<uint8_t, 64> opad_{};
  std::unique_ptr<VmessPrimHash> inner_;
  std::unique_ptr<VmessPrimHash> outer_;
};

std::function<std::unique_ptr<VmessPrimHash>()> VmessAeadKdfBuildFactory(
    const std::vector<base::span<const uint8_t>>& path,
    size_t depth) {
  const base::span<const uint8_t> kRoot =
      base::span(kKdfSaltVmessAead).first(sizeof(kKdfSaltVmessAead) - 1);
  if (depth == 0) {
    return [kRoot]() {
      return std::make_unique<VmessGoHmac>(
          []() { return std::make_unique<VmessSha256Hash>(); }, kRoot);
    };
  }
  std::function<std::unique_ptr<VmessPrimHash>()> inner =
      VmessAeadKdfBuildFactory(path, depth - 1);
  base::span<const uint8_t> seg = path[depth - 1];
  return [inner, seg]() {
    return std::make_unique<VmessGoHmac>(inner, seg);
  };
}

void VmessAeadKdf(base::span<const uint8_t> key,
                  const std::vector<base::span<const uint8_t>>& path,
                  base::span<uint8_t, 32> out) {
  CHECK(!path.empty());
  std::unique_ptr<VmessPrimHash> h =
      VmessAeadKdfBuildFactory(path, path.size())();
  h->Reset();
  h->Update(key);
  h->SumInto(out);
}

void VmessAeadKdf16(base::span<const uint8_t> key,
                    const std::vector<base::span<const uint8_t>>& path,
                    base::span<uint8_t, 16> out) {
  std::array<uint8_t, 32> full{};
  VmessAeadKdf(key, path, full);
  out.copy_from(base::span(full).first<16>());
}

struct LeafVmessShakeMask {
  sha3_ctx_t ctx{};

  explicit LeafVmessShakeMask(base::span<const uint8_t> nonce) {
    sha3_init_shake128(&ctx);
    sha3_update(&ctx, nonce);
    shake128_xof(&ctx);
  }

  uint16_t NextMaskU16() {
    std::array<uint8_t, 2> b{};
    shake128_read(&ctx, b);
    return (static_cast<uint16_t>(b[0]) << 8) | b[1];
  }

  uint16_t DecodeLength(base::span<const uint8_t> wire2) {
    CHECK_GE(wire2.size(), 2u);
    uint16_t mask = NextMaskU16();
    uint16_t w =
        (static_cast<uint16_t>(wire2[0]) << 8) | static_cast<uint16_t>(wire2[1]);
    return mask ^ w;
  }

  void EncodeLength(uint16_t payload_len, base::span<uint8_t, 2> wire2) {
    uint16_t mask = NextMaskU16();
    uint16_t enc = mask ^ payload_len;
    wire2[0] = static_cast<uint8_t>(enc >> 8);
    wire2[1] = static_cast<uint8_t>(enc & 0xff);
  }

  uint16_t NextPaddingLen() { return NextMaskU16() % 64; }
};

}  // namespace

bool LeafVmessBuildClientRequest(
    const std::array<uint8_t, 16>& uuid,
    std::string_view dest_host,
    uint16_t dest_port,
    std::string_view security_cipher_name,
    std::vector<uint8_t>* handshake_out,
    VmessSessionMaterial* session_out) {
  if (!handshake_out || !session_out || dest_host.empty()) {
    return false;
  }
  handshake_out->clear();

  std::string cipher(security_cipher_name);
  for (char& c : cipher) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  VmessSecurityCipher sec = VmessSecurityCipher::kChacha20Poly1305;
  if (cipher == "aes-128-gcm") {
    sec = VmessSecurityCipher::kAes128Gcm;
  } else if (cipher == "chacha20-poly1305" ||
             cipher == "chacha20-ietf-poly1305") {
    sec = VmessSecurityCipher::kChacha20Poly1305;
  } else if (!cipher.empty()) {
    return false;
  }

  VmessSessionMaterial sess;
  crypto::RandBytes(sess.request_body_key);
  crypto::RandBytes(sess.request_body_iv);
  sess.response_header = 0;
  crypto::RandBytes(base::byte_span_from_ref(sess.response_header));
  sess.security = sec;

  SHA256_CTX s256;
  SHA256_Init(&s256);
  SHA256_Update(&s256, sess.request_body_key.data(), 16);
  std::array<uint8_t, 32> rk_full{};
  SHA256_Final(rk_full.data(), &s256);
  base::span(sess.response_body_key).copy_from(base::span(rk_full).first<16>());

  SHA256_Init(&s256);
  SHA256_Update(&s256, sess.request_body_iv.data(), 16);
  std::array<uint8_t, 32> riv_full{};
  SHA256_Final(riv_full.data(), &s256);
  base::span(sess.response_body_iv).copy_from(base::span(riv_full).first<16>());

  std::array<uint8_t, 16> header_key{};
  MD5_CTX md5;
  MD5_Init(&md5);
  MD5_Update(&md5, uuid.data(), 16);
  MD5_Update(&md5, kUuidMd5Literal, strlen(kUuidMd5Literal));
  MD5_Final(header_key.data(), &md5);

  std::vector<uint8_t> plain;
  plain.push_back(1);
  plain.insert(plain.end(), sess.request_body_iv.begin(),
               sess.request_body_iv.end());
  plain.insert(plain.end(), sess.request_body_key.begin(),
               sess.request_body_key.end());
  plain.push_back(sess.response_header);
  uint8_t option = kRequestOptionChunkStream | kRequestOptionChunkMasking |
                   kRequestOptionGlobalPadding;
  plain.push_back(option);
  uint8_t padding_len = 0;
  crypto::RandBytes(base::byte_span_from_ref(padding_len));
  padding_len = static_cast<uint8_t>(padding_len % 16);
  plain.push_back(static_cast<uint8_t>((padding_len << 4) |
                                        static_cast<unsigned>(sec)));
  plain.push_back(0);
  plain.push_back(kRequestCommandTcp);

  IPAddress ip;
  if (ip.AssignFromIPLiteral(dest_host)) {
    if (ip.IsIPv4()) {
      plain.push_back(static_cast<uint8_t>(dest_port >> 8));
      plain.push_back(static_cast<uint8_t>(dest_port & 0xff));
      plain.push_back(1);
      plain.insert(plain.end(), ip.bytes().begin(), ip.bytes().end());
    } else {
      plain.push_back(static_cast<uint8_t>(dest_port >> 8));
      plain.push_back(static_cast<uint8_t>(dest_port & 0xff));
      plain.push_back(3);
      plain.insert(plain.end(), ip.bytes().begin(), ip.bytes().end());
    }
  } else {
    if (dest_host.size() > 255) {
      return false;
    }
    plain.push_back(static_cast<uint8_t>(dest_port >> 8));
    plain.push_back(static_cast<uint8_t>(dest_port & 0xff));
    plain.push_back(2);
    plain.push_back(static_cast<uint8_t>(dest_host.size()));
    plain.insert(plain.end(), dest_host.begin(), dest_host.end());
  }

  if (padding_len) {
    std::vector<uint8_t> pad(padding_len);
    crypto::RandBytes(pad);
    plain.insert(plain.end(), pad.begin(), pad.end());
  }

  uint32_t fnv = kFnvOffsetBasis;
  for (size_t i = 0; i < plain.size(); ++i) {
    fnv ^= plain[i];
    fnv *= kFnvPrime;
  }
  plain.push_back(static_cast<uint8_t>(fnv >> 24));
  plain.push_back(static_cast<uint8_t>(fnv >> 16));
  plain.push_back(static_cast<uint8_t>(fnv >> 8));
  plain.push_back(static_cast<uint8_t>(fnv & 0xff));

  const base::span<const uint8_t, 16> uuid_key(header_key);

  int64_t ts = static_cast<int64_t>(base::Time::Now().ToTimeT());
  std::array<uint8_t, 16> auth_raw{};
  for (int i = 0; i < 8; ++i) {
    auth_raw[i] = static_cast<uint8_t>(
        (static_cast<uint64_t>(ts) >> (56 - 8 * i)) & 0xff);
  }
  crypto::RandBytes(base::span(auth_raw).subspan(8u, 4u));
  uint32_t c = crc32(0L, auth_raw.data(), 12);
  auth_raw[12] = static_cast<uint8_t>(c >> 24);
  auth_raw[13] = static_cast<uint8_t>(c >> 16);
  auth_raw[14] = static_cast<uint8_t>(c >> 8);
  auth_raw[15] = static_cast<uint8_t>(c & 0xff);

  std::array<uint8_t, 16> aes_key{};
  VmessAeadKdf16(
      uuid_key,
      {base::span(kKdfSaltAuthIdEnc).first(sizeof(kKdfSaltAuthIdEnc) - 1)},
      aes_key);
  AES_KEY ak;
  AES_set_encrypt_key(aes_key.data(), 128, &ak);
  AES_encrypt(auth_raw.data(), auth_raw.data(), &ak);

  std::array<uint8_t, 8> conn_nonce{};
  crypto::RandBytes(conn_nonce);

  uint16_t payload_len_u16 = static_cast<uint16_t>(plain.size());
  std::array<uint8_t, 2> len_plain = {
      static_cast<uint8_t>(payload_len_u16 >> 8),
      static_cast<uint8_t>(payload_len_u16 & 0xff)};

  std::array<uint8_t, 16> k_len_key{};
  VmessAeadKdf16(
      uuid_key,
      {base::span(kKdfSaltVmessHdrLenAeadKey)
           .first(sizeof(kKdfSaltVmessHdrLenAeadKey) - 1),
       base::span(auth_raw), base::span(conn_nonce)},
      k_len_key);
  std::array<uint8_t, 32> k_len_iv{};
  VmessAeadKdf(
      uuid_key,
      {base::span(kKdfSaltVmessHdrLenAeadIv)
           .first(sizeof(kKdfSaltVmessHdrLenAeadIv) - 1),
       base::span(auth_raw), base::span(conn_nonce)},
      k_len_iv);

  EVP_AEAD_CTX len_ctx;
  EVP_AEAD_CTX_zero(&len_ctx);
  const EVP_AEAD* len_aead = EVP_aead_aes_128_gcm();
  if (!EVP_AEAD_CTX_init(&len_ctx, len_aead, k_len_key.data(), 16,
                         EVP_AEAD_DEFAULT_TAG_LENGTH, nullptr)) {
    return false;
  }
  size_t len_ct_len = 0;
  std::vector<uint8_t> len_ct(2 + EVP_AEAD_max_overhead(len_aead));
  if (!EVP_AEAD_CTX_seal(&len_ctx, len_ct.data(), &len_ct_len, len_ct.size(),
                         k_len_iv.data(), 12, len_plain.data(), len_plain.size(),
                         auth_raw.data(), auth_raw.size())) {
    EVP_AEAD_CTX_cleanup(&len_ctx);
    return false;
  }
  EVP_AEAD_CTX_cleanup(&len_ctx);

  std::array<uint8_t, 16> k_hdr_key{};
  VmessAeadKdf16(
      uuid_key,
      {base::span(kKdfSaltVmessHdrPayloadAeadKey)
           .first(sizeof(kKdfSaltVmessHdrPayloadAeadKey) - 1),
       base::span(auth_raw), base::span(conn_nonce)},
      k_hdr_key);
  std::array<uint8_t, 32> k_hdr_iv{};
  VmessAeadKdf(
      uuid_key,
      {base::span(kKdfSaltVmessHdrPayloadAeadIv)
           .first(sizeof(kKdfSaltVmessHdrPayloadAeadIv) - 1),
       base::span(auth_raw), base::span(conn_nonce)},
      k_hdr_iv);

  EVP_AEAD_CTX hdr_ctx;
  EVP_AEAD_CTX_zero(&hdr_ctx);
  if (!EVP_AEAD_CTX_init(&hdr_ctx, EVP_aead_aes_128_gcm(), k_hdr_key.data(), 16,
                         EVP_AEAD_DEFAULT_TAG_LENGTH, nullptr)) {
    return false;
  }
  size_t hdr_ct_len = 0;
  std::vector<uint8_t> hdr_ct(plain.size() + EVP_AEAD_max_overhead(
                                                 EVP_aead_aes_128_gcm()));
  if (!EVP_AEAD_CTX_seal(&hdr_ctx, hdr_ct.data(), &hdr_ct_len, hdr_ct.size(),
                         k_hdr_iv.data(), 12, plain.data(), plain.size(),
                         auth_raw.data(), auth_raw.size())) {
    EVP_AEAD_CTX_cleanup(&hdr_ctx);
    return false;
  }
  EVP_AEAD_CTX_cleanup(&hdr_ctx);

  handshake_out->reserve(16 + len_ct_len + 8 + hdr_ct_len);
  handshake_out->insert(handshake_out->end(), auth_raw.begin(), auth_raw.end());
  handshake_out->insert(handshake_out->end(), len_ct.begin(),
                        len_ct.begin() + len_ct_len);
  handshake_out->insert(handshake_out->end(), conn_nonce.begin(),
                        conn_nonce.end());
  handshake_out->insert(handshake_out->end(), hdr_ct.begin(),
                        hdr_ct.begin() + hdr_ct_len);

  *session_out = sess;
  return true;
}

LeafVmessStreamEngine::LeafVmessStreamEngine(VmessSessionMaterial session)
    : session_(session),
      enc_shake_(new LeafVmessShakeMask(base::span(session_.request_body_iv))),
      dec_shake_(
          new LeafVmessShakeMask(base::span(session_.response_body_iv))) {}

LeafVmessStreamEngine::~LeafVmessStreamEngine() {
  delete static_cast<LeafVmessShakeMask*>(enc_shake_.get());
  enc_shake_ = nullptr;
  delete static_cast<LeafVmessShakeMask*>(dec_shake_.get());
  dec_shake_ = nullptr;
}

std::array<uint8_t, 12> LeafVmessStreamEngine::NextEncNonce() {
  std::array<uint8_t, 12> n{};
  base::span(n).copy_from(base::span(session_.request_body_iv).first<12>());
  enc_chunk_nonce_ = enc_chunk_nonce_ + 1;
  n[0] = static_cast<uint8_t>(enc_chunk_nonce_ >> 8);
  n[1] = static_cast<uint8_t>(enc_chunk_nonce_ & 0xff);
  return n;
}

std::array<uint8_t, 12> LeafVmessStreamEngine::NextDecNonce() {
  std::array<uint8_t, 12> n{};
  base::span(n).copy_from(base::span(session_.response_body_iv).first<12>());
  dec_chunk_nonce_ = dec_chunk_nonce_ + 1;
  n[0] = static_cast<uint8_t>(dec_chunk_nonce_ >> 8);
  n[1] = static_cast<uint8_t>(dec_chunk_nonce_ & 0xff);
  return n;
}

bool LeafVmessStreamEngine::AeadSealBody(std::vector<uint8_t>* buf) {
  auto nonce = NextEncNonce();
  const EVP_AEAD* aead =
      session_.security == VmessSecurityCipher::kAes128Gcm
          ? EVP_aead_aes_128_gcm()
          : EVP_aead_chacha20_poly1305();
  const size_t key_len = EVP_AEAD_key_length(aead);
  std::array<uint8_t, 32> key_buf{};
  if (session_.security == VmessSecurityCipher::kAes128Gcm) {
    base::span(key_buf).first<16>().copy_from(session_.request_body_key);
  } else {
    std::array<uint8_t, 16> half{};
    MD5(session_.request_body_key.data(), 16, half.data());
    base::span(key_buf).first<16>().copy_from(half);
    MD5(half.data(), 16, half.data());
    base::span(key_buf).subspan<16>().copy_from(half);
  }
  EVP_AEAD_CTX ctx;
  EVP_AEAD_CTX_zero(&ctx);
  if (!EVP_AEAD_CTX_init(&ctx, aead, key_buf.data(), key_len,
                         EVP_AEAD_DEFAULT_TAG_LENGTH, nullptr)) {
    return false;
  }
  const size_t in_len = buf->size();
  std::vector<uint8_t> in_copy = *buf;
  buf->resize(in_len + EVP_AEAD_max_overhead(aead));
  size_t out_len = 0;
  const bool ok =
      EVP_AEAD_CTX_seal(&ctx, buf->data(), &out_len, buf->size(), nonce.data(),
                        12, in_copy.data(), in_len, nullptr, 0);
  EVP_AEAD_CTX_cleanup(&ctx);
  if (!ok) {
    return false;
  }
  buf->resize(out_len);
  return true;
}

bool LeafVmessStreamEngine::AeadOpenBody(std::vector<uint8_t>* buf) {
  if (buf->size() < kTagLen) {
    return false;
  }
  auto nonce = NextDecNonce();
  const EVP_AEAD* aead =
      session_.security == VmessSecurityCipher::kAes128Gcm
          ? EVP_aead_aes_128_gcm()
          : EVP_aead_chacha20_poly1305();
  const size_t key_len = EVP_AEAD_key_length(aead);
  std::array<uint8_t, 32> key_buf{};
  if (session_.security == VmessSecurityCipher::kAes128Gcm) {
    base::span(key_buf).first<16>().copy_from(session_.response_body_key);
  } else {
    std::array<uint8_t, 16> half{};
    MD5(session_.response_body_key.data(), 16, half.data());
    base::span(key_buf).first<16>().copy_from(half);
    MD5(half.data(), 16, half.data());
    base::span(key_buf).subspan<16>().copy_from(half);
  }
  EVP_AEAD_CTX ctx;
  EVP_AEAD_CTX_zero(&ctx);
  if (!EVP_AEAD_CTX_init(&ctx, aead, key_buf.data(), key_len,
                         EVP_AEAD_DEFAULT_TAG_LENGTH, nullptr)) {
    return false;
  }
  size_t out_len = 0;
  const bool ok = EVP_AEAD_CTX_open(
      &ctx, buf->data(), &out_len, buf->size() - kTagLen, nonce.data(), 12,
      buf->data(), buf->size(), nullptr, 0);
  EVP_AEAD_CTX_cleanup(&ctx);
  if (!ok) {
    return false;
  }
  buf->resize(out_len);
  return true;
}

bool LeafVmessStreamEngine::DecryptResponseHeader() {
  if (resp_hdr_phase_ == 0) {
    if (rx_queue_.size() < 18) {
      return false;
    }
    std::array<uint8_t, 16> k1{};
    VmessAeadKdf16(
        session_.response_body_key,
        {base::span(kKdfSaltAeadRespHdrLenKey)
             .first(sizeof(kKdfSaltAeadRespHdrLenKey) - 1)},
        k1);
    std::array<uint8_t, 32> iv1{};
    VmessAeadKdf(
        session_.response_body_iv,
        {base::span(kKdfSaltAeadRespHdrLenIv)
             .first(sizeof(kKdfSaltAeadRespHdrLenIv) - 1)},
        iv1);

    std::vector<uint8_t> blk(rx_queue_.begin(), rx_queue_.begin() + 18);
    rx_queue_.erase(rx_queue_.begin(), rx_queue_.begin() + 18);

    EVP_AEAD_CTX ctx;
    EVP_AEAD_CTX_zero(&ctx);
    if (!EVP_AEAD_CTX_init(&ctx, EVP_aead_aes_128_gcm(), k1.data(), 16,
                           EVP_AEAD_DEFAULT_TAG_LENGTH, nullptr)) {
      read_failed_ = true;
      return false;
    }
    size_t out_len = 0;
    if (!EVP_AEAD_CTX_open(&ctx, blk.data(), &out_len, blk.size(), iv1.data(),
                           12, blk.data(), blk.size(), nullptr, 0)) {
      EVP_AEAD_CTX_cleanup(&ctx);
      read_failed_ = true;
      resp_hdr_phase_ = 0;
      LOG(ERROR) << "[LEAF_PROXY_DEBUG] VMess DecryptResponseHeader phase0 "
                    "AEAD open failed (rx_queue="
                 << rx_queue_.size() << ")";
      return false;
    }
    EVP_AEAD_CTX_cleanup(&ctx);
    blk.resize(out_len);
    if (blk.size() < 2) {
      read_failed_ = true;
      return false;
    }
    resp_inner_len_ = (static_cast<size_t>(blk[0]) << 8) | blk[1];
    resp_hdr_phase_ = 1;
  }

  if (resp_hdr_phase_ == 1) {
    const size_t need = resp_inner_len_ + 16;
    if (rx_queue_.size() < need) {
      return false;
    }
    std::array<uint8_t, 16> k2{};
    VmessAeadKdf16(
        session_.response_body_key,
        {base::span(kKdfSaltAeadRespHdrPayloadKey)
             .first(sizeof(kKdfSaltAeadRespHdrPayloadKey) - 1)},
        k2);
    std::array<uint8_t, 32> iv2{};
    VmessAeadKdf(
        session_.response_body_iv,
        {base::span(kKdfSaltAeadRespHdrPayloadIv)
             .first(sizeof(kKdfSaltAeadRespHdrPayloadIv) - 1)},
        iv2);

    std::vector<uint8_t> blk(rx_queue_.begin(), rx_queue_.begin() + need);
    rx_queue_.erase(rx_queue_.begin(), rx_queue_.begin() + need);

    EVP_AEAD_CTX ctx2;
    EVP_AEAD_CTX_zero(&ctx2);
    if (!EVP_AEAD_CTX_init(&ctx2, EVP_aead_aes_128_gcm(), k2.data(), 16,
                           EVP_AEAD_DEFAULT_TAG_LENGTH, nullptr)) {
      read_failed_ = true;
      resp_hdr_phase_ = 0;
      return false;
    }
    size_t out_len = 0;
    if (!EVP_AEAD_CTX_open(&ctx2, blk.data(), &out_len, blk.size(), iv2.data(),
                           12, blk.data(), blk.size(), nullptr, 0)) {
      EVP_AEAD_CTX_cleanup(&ctx2);
      read_failed_ = true;
      resp_hdr_phase_ = 0;
      LOG(ERROR) << "[LEAF_PROXY_DEBUG] VMess DecryptResponseHeader phase1 "
                    "AEAD open failed inner_len="
                 << resp_inner_len_;
      return false;
    }
    EVP_AEAD_CTX_cleanup(&ctx2);
    blk.resize(out_len);
    if (blk.empty() || blk[0] != session_.response_header) {
      read_failed_ = true;
      resp_hdr_phase_ = 0;
      LOG(ERROR) << "[LEAF_PROXY_DEBUG] VMess DecryptResponseHeader bad magic "
                 << "expect=" << static_cast<int>(session_.response_header)
                 << " got="
                 << (blk.empty() ? -1 : static_cast<int>(blk[0]));
      return false;
    }
    resp_hdr_phase_ = 0;
    resp_inner_len_ = 0;
    response_header_done_ = true;
    read_state_ = ReadState::kChunkLength;
    return true;
  }
  return false;
}

bool LeafVmessStreamEngine::FeedCipherText(base::span<const uint8_t> data,
                                           std::vector<uint8_t>* out_plain) {
  if (read_failed_) {
    return false;
  }
  rx_queue_.insert(rx_queue_.end(), data.begin(), data.end());
  while (true) {
    switch (read_state_) {
      case ReadState::kResponseHeader:
        if (!DecryptResponseHeader()) {
          return !read_failed_;
        }
        break;
      case ReadState::kChunkLength: {
        if (rx_queue_.size() < 2) {
          return true;
        }
        std::array<uint8_t, 2> w = {rx_queue_[0], rx_queue_[1]};
        rx_queue_.erase(rx_queue_.begin(), rx_queue_.begin() + 2);
        // Match crypto.AuthenticationReader.readSize (Xray): NextPaddingLen, then
        // DecodeLength; order must match the writer's SHAKE consumption sequence.
        chunk_padding_ = static_cast<LeafVmessShakeMask*>(dec_shake_.get())
                               ->NextPaddingLen() %
                           64;
        chunk_length_pending_ =
            static_cast<LeafVmessShakeMask*>(dec_shake_.get())
                ->DecodeLength(w);
        chunk_got_ = 0;
        chunk_buf_.clear();
        read_state_ = ReadState::kChunkData;
        break;
      }
      case ReadState::kChunkData: {
        const size_t need = chunk_length_pending_;
        const size_t avail = rx_queue_.size();
        const size_t take = std::min(need - chunk_got_, avail);
        chunk_buf_.insert(chunk_buf_.end(), rx_queue_.begin(),
                          rx_queue_.begin() + take);
        rx_queue_.erase(rx_queue_.begin(), rx_queue_.begin() + take);
        chunk_got_ += take;
        if (chunk_got_ < need) {
          return true;
        }
        if (chunk_buf_.size() < chunk_padding_) {
          read_failed_ = true;
          return false;
        }
        chunk_buf_.resize(chunk_buf_.size() - chunk_padding_);
        if (!AeadOpenBody(&chunk_buf_)) {
          read_failed_ = true;
          LOG(ERROR) << "[LEAF_PROXY_DEBUG] VMess AeadOpenBody failed "
                     << "chunk_ct_len=" << chunk_buf_.size()
                     << " chunk_wire_len=" << chunk_length_pending_
                     << " padding=" << chunk_padding_;
          return false;
        }
        plain_drain_.insert(plain_drain_.end(), chunk_buf_.begin(),
                             chunk_buf_.end());
        chunk_buf_.clear();
        read_state_ = ReadState::kDrainPlain;
        break;
      }
      case ReadState::kDrainPlain:
        if (!plain_drain_.empty()) {
          out_plain->insert(out_plain->end(), plain_drain_.begin(),
                            plain_drain_.end());
          plain_drain_.clear();
        }
        read_state_ = ReadState::kChunkLength;
        break;
      default:
        NOTREACHED();
    }
  }
}

void LeafVmessStreamEngine::QueuePlainText(base::span<const uint8_t> data) {
  tx_plain_.insert(tx_plain_.end(), data.begin(), data.end());
}

bool LeafVmessStreamEngine::SealPayloadChunkWithPadding(
    base::span<const uint8_t> plain,
    uint16_t padding_size,
    std::vector<uint8_t>* frame) {
  frame->clear();
  const size_t max_payload = kMaxPayload - kTagLen - padding_size;
  if (plain.size() > max_payload) {
    return false;
  }
  const size_t payload_wire_len = plain.size() + kTagLen + padding_size;
  std::array<uint8_t, 2> len_wire{};
  static_cast<LeafVmessShakeMask*>(enc_shake_.get())
      ->EncodeLength(static_cast<uint16_t>(payload_wire_len), len_wire);
  frame->push_back(len_wire[0]);
  frame->push_back(len_wire[1]);
  std::vector<uint8_t> piece(plain.begin(), plain.end());
  if (!AeadSealBody(&piece)) {
    return false;
  }
  frame->insert(frame->end(), piece.begin(), piece.end());
  if (padding_size) {
    std::vector<uint8_t> pad(padding_size);
    crypto::RandBytes(pad);
    frame->insert(frame->end(), pad.begin(), pad.end());
  }
  return true;
}

bool LeafVmessStreamEngine::ProduceCipherText(std::vector<uint8_t>* out_cipher) {
  out_cipher->clear();
  if (tx_plain_.empty()) {
    return true;
  }
  const uint16_t padding_size =
      static_cast<LeafVmessShakeMask*>(enc_shake_.get())->NextPaddingLen() % 64;
  const size_t max_payload = kMaxPayload - kTagLen - padding_size;
  const size_t take = std::min(tx_plain_.size(), max_payload);
  if (!SealPayloadChunkWithPadding(base::span(tx_plain_).first(take),
                                   padding_size, out_cipher)) {
    return false;
  }
  tx_plain_.erase(tx_plain_.begin(), tx_plain_.begin() + take);
  return true;
}

}  // namespace net::chromium_leaf
