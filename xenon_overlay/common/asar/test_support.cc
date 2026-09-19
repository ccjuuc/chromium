// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/common/asar/test_support.h"

#include <array>
#include <cstdint>
#include <utility>

#include "base/check.h"
#include "base/check_op.h"
#include "base/containers/span.h"
#include "base/files/file_util.h"
#include "base/json/json_writer.h"
#include "base/pickle.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/values.h"
#include "third_party/boringssl/src/include/openssl/aes.h"
#include "third_party/boringssl/src/include/openssl/bio.h"
#include "third_party/boringssl/src/include/openssl/bn.h"
#include "third_party/boringssl/src/include/openssl/md5.h"
#include "third_party/boringssl/src/include/openssl/pem.h"
#include "third_party/boringssl/src/include/openssl/rand.h"
#include "third_party/boringssl/src/include/openssl/rsa.h"

namespace xenon::asar {
namespace {

std::string Encrypt(std::string data, const std::array<uint8_t, 16>& key) {
  data.resize((data.size() + 15) / 16 * 16, ' ');
  AES_KEY aes;
  CHECK_EQ(AES_set_encrypt_key(key.data(), 128, &aes), 0);
  auto bytes = base::as_writable_byte_span(data);
  for (size_t offset = 0; offset < bytes.size(); offset += AES_BLOCK_SIZE) {
    auto block = bytes.subspan(offset).first<AES_BLOCK_SIZE>();
    AES_encrypt(block.data(), block.data(), &aes);
  }
  return data;
}

void AppendPickle(std::string& target, const base::Pickle& pickle) {
  target.append(reinterpret_cast<const char*>(pickle.data()), pickle.size());
}

}  // namespace

struct TestArchiveBuilder::Impl {
  bssl::UniquePtr<RSA> rsa{RSA_new()};
  std::array<uint8_t, 16> aes_key;
  std::string public_key;
};

TestArchiveBuilder::TestArchiveBuilder() : impl_(std::make_unique<Impl>()) {
  bssl::UniquePtr<BIGNUM> exponent(BN_new());
  CHECK(BN_set_word(exponent.get(), RSA_F4));
  CHECK(RSA_generate_key_ex(impl_->rsa.get(), 2048, exponent.get(), nullptr));
  CHECK(RAND_bytes(impl_->aes_key.data(), impl_->aes_key.size()));
  bssl::UniquePtr<BIO> bio(BIO_new(BIO_s_mem()));
  CHECK(PEM_write_bio_RSA_PUBKEY(bio.get(), impl_->rsa.get()));
  const uint8_t* bytes = nullptr;
  size_t size = 0;
  CHECK(BIO_mem_contents(bio.get(), &bytes, &size));
  impl_->public_key.assign(reinterpret_cast<const char*>(bytes), size);
}

TestArchiveBuilder::~TestArchiveBuilder() = default;

const std::string& TestArchiveBuilder::public_key_pem() const {
  return impl_->public_key;
}

bool TestArchiveBuilder::Write(const base::FilePath& path,
                               const std::vector<TestArchiveEntry>& entries,
                               bool encrypted_header) const {
  base::DictValue header;
  header.Set("files", base::DictValue());
  std::string payload;
  for (const auto& entry : entries) {
    const auto relative = base::FilePath::FromUTF8Unsafe(entry.path);
    if (relative.empty() || relative.IsAbsolute() ||
        relative.ReferencesParent() || (entry.encrypted && !encrypted_header)) {
      return false;
    }
    const auto parts = relative.GetComponents();
    auto* files = header.FindDict("files");
    for (size_t index = 0; index + 1 < parts.size(); ++index) {
      const auto name = base::FilePath(parts[index]).AsUTF8Unsafe();
      if (!files->FindDict(name)) {
        base::DictValue directory;
        directory.Set("files", base::DictValue());
        files->Set(name, std::move(directory));
      }
      files = files->FindDict(name)->FindDict("files");
      if (!files) {
        return false;
      }
    }
    base::DictValue node;
    if (!entry.link.empty()) {
      const auto target = base::FilePath::FromUTF8Unsafe(entry.link);
      if (target.IsAbsolute() || target.ReferencesParent()) {
        return false;
      }
      node.Set("link", entry.link);
      files->Set(base::FilePath(parts.back()).AsUTF8Unsafe(), std::move(node));
      continue;
    }
    std::string bytes = entry.encrypted && !entry.unpacked
                            ? Encrypt(entry.contents, impl_->aes_key)
                            : entry.contents;
    node.Set("size", static_cast<double>(bytes.size()));
    if (entry.unpacked) {
      node.Set("unpacked", true);
      const auto unpacked =
          path.AddExtension(FILE_PATH_LITERAL("unpacked")).Append(relative);
      if (!base::CreateDirectory(unpacked.DirName()) ||
          !base::WriteFile(unpacked, bytes)) {
        return false;
      }
    } else {
      node.Set("offset", base::NumberToString(payload.size()));
      if (entry.encrypted) {
        node.Set("encrypted", true);
      }
      payload.append(bytes);
    }
    files->Set(base::FilePath(parts.back()).AsUTF8Unsafe(), std::move(node));
  }
  const auto json = base::WriteJson(header);
  if (!json) {
    return false;
  }
  std::string archive;
  base::Pickle length;
  if (encrypted_header) {
    const auto ciphertext = Encrypt(*json, impl_->aes_key);
    std::array<uint8_t, MD5_DIGEST_LENGTH> digest;
    MD5(reinterpret_cast<const uint8_t*>(ciphertext.data()), ciphertext.size(),
        digest.data());
    base::Pickle envelope;
    envelope.WriteUInt32(ciphertext.size());
    envelope.WriteString(base::ToLowerASCII(base::HexEncode(digest)));
    envelope.WriteString(
        std::string_view(reinterpret_cast<const char*>(impl_->aes_key.data()),
                         impl_->aes_key.size()));
    std::array<uint8_t, 256> signature;
    size_t signature_size = 0;
    CHECK(RSA_sign_raw(impl_->rsa.get(), &signature_size, signature.data(),
                       signature.size(), envelope.data(), envelope.size(),
                       RSA_PKCS1_PADDING));
    CHECK_EQ(signature_size, signature.size());
    length.WriteUInt32(signature.size());
    AppendPickle(archive, length);
    archive.append(reinterpret_cast<const char*>(signature.data()),
                   signature.size());
    archive.append(ciphertext);
  } else {
    base::Pickle index;
    index.WriteString(*json);
    length.WriteUInt32(index.size());
    AppendPickle(archive, length);
    AppendPickle(archive, index);
  }
  archive.append(payload);
  return base::WriteFile(path, archive);
}

}  // namespace xenon::asar
