// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Portions copyright (c) 2014 GitHub, Inc. under the MIT license.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/common/asar/archive.h"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/check_op.h"
#include "base/containers/span.h"
#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/numerics/byte_conversions.h"
#include "base/pickle.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/synchronization/lock.h"
#include "third_party/boringssl/src/include/openssl/aes.h"
#include "third_party/boringssl/src/include/openssl/bio.h"
#include "third_party/boringssl/src/include/openssl/md5.h"
#include "third_party/boringssl/src/include/openssl/mem.h"
#include "third_party/boringssl/src/include/openssl/pem.h"
#include "third_party/boringssl/src/include/openssl/rsa.h"

namespace xenon::asar {

struct ArchiveDecryptionKey {
  AES_KEY aes;
  ~ArchiveDecryptionKey() { OPENSSL_cleanse(&aes, sizeof(aes)); }
};

namespace {

#if BUILDFLAG(IS_WIN)
constexpr char kSeparators[] = "\\/";
#else
constexpr char kSeparators[] = "/";
#endif

constexpr int kMaxLinkDepth = 40;
constexpr uint32_t kMaxHeaderSize = 64 * 1024 * 1024;
constexpr size_t kReadChunkSize = 1024 * 1024;
constexpr base::FilePath::CharType kAsarExtension[] =
    FILE_PATH_LITERAL(".asar");

struct PathLess {
  bool operator()(const base::FilePath& left,
                  const base::FilePath& right) const {
#if BUILDFLAG(IS_WIN)
    return base::FilePath::CompareLessIgnoreCase(left.value(), right.value());
#else
    return left < right;
#endif
  }
};

using ArchiveMap = std::map<base::FilePath, std::shared_ptr<Archive>, PathLess>;

struct ArchiveRegistry {
  ArchiveMap cache;
  std::map<std::string, std::vector<ArchiveKeyConfig>> owners;
  uint64_t generation = 0;
};

ArchiveRegistry& GetArchiveRegistry() {
  static base::NoDestructor<ArchiveRegistry> registry;
  return *registry;
}

bool PathContains(const base::FilePath& root, const base::FilePath& path) {
  const auto root_parts = root.GetComponents();
  const auto path_parts = path.GetComponents();
  return root_parts.size() <= path_parts.size() &&
         std::equal(root_parts.begin(), root_parts.end(), path_parts.begin(),
                    [](const auto& a, const auto& b) {
#if BUILDFLAG(IS_WIN)
                      return base::FilePath::CompareEqualIgnoreCase(a, b);
#else
                      return a == b;
#endif
                    });
}

bssl::UniquePtr<RSA> ReadPublicKey(const std::string& pem) {
  if (pem.empty() || pem.size() > 16384) {
    return nullptr;
  }
  // The PEM reader itself skips unrelated blocks and trailing data. A trusted
  // configuration should nevertheless contain exactly one public key, never
  // an accidentally copied private key or an ambiguous bundle.
  const std::string_view block = base::TrimWhitespaceASCII(pem, base::TRIM_ALL);
  constexpr std::string_view kSpkiBegin = "-----BEGIN PUBLIC KEY-----";
  constexpr std::string_view kSpkiEnd = "-----END PUBLIC KEY-----";
  constexpr std::string_view kPkcs1Begin = "-----BEGIN RSA PUBLIC KEY-----";
  constexpr std::string_view kPkcs1End = "-----END RSA PUBLIC KEY-----";
  const bool pkcs1 = block.starts_with(kPkcs1Begin);
  const auto begin = pkcs1 ? kPkcs1Begin : kSpkiBegin;
  const auto end = pkcs1 ? kPkcs1End : kSpkiEnd;
  if (!block.starts_with(begin) || !block.ends_with(end) ||
      block.find("-----BEGIN", begin.size()) != std::string_view::npos ||
      block.find("-----END") != block.size() - end.size()) {
    return nullptr;
  }
  bssl::UniquePtr<BIO> bio(BIO_new_mem_buf(block.data(), block.size()));
  bssl::UniquePtr<RSA> key(
      pkcs1 ? PEM_read_bio_RSAPublicKey(bio.get(), nullptr, nullptr, nullptr)
            : PEM_read_bio_RSA_PUBKEY(bio.get(), nullptr, nullptr, nullptr));
  if (!key || RSA_bits(key.get()) != 2048) {
    return nullptr;
  }
  return key;
}

void DecryptBlocks(const ArchiveDecryptionKey& key, base::span<uint8_t> bytes) {
  CHECK_EQ(bytes.size() % AES_BLOCK_SIZE, 0u);
  for (size_t offset = 0; offset < bytes.size(); offset += AES_BLOCK_SIZE) {
    auto block = bytes.subspan(offset).first<AES_BLOCK_SIZE>();
    AES_decrypt(block.data(), block.data(), &key.aes);
  }
}

bool ReadInChunks(base::File& file,
                  uint64_t offset,
                  base::span<uint8_t> output) {
  constexpr uint64_t kMaxFileOffset = std::numeric_limits<int64_t>::max();
  if (offset > kMaxFileOffset || output.size() > kMaxFileOffset - offset) {
    return false;
  }
  while (!output.empty()) {
    const size_t count = std::min(output.size(), kReadChunkSize);
    if (!file.ReadAndCheck(static_cast<int64_t>(offset), output.first(count))) {
      return false;
    }
    offset += count;
    output = output.subspan(count);
  }
  return true;
}

bool HasExactPickleLength(base::span<const uint8_t> bytes) {
  return bytes.size() >= 4 &&
         base::U32FromLittleEndian(bytes.first<4>()) == bytes.size() - 4;
}

base::Lock& GetArchiveCacheLock() {
  static base::NoDestructor<base::Lock> lock;
  return *lock;
}

const base::DictValue* GetNodeFromPath(std::string path,
                                       const base::DictValue& root,
                                       int depth);

const base::DictValue* GetFilesNode(const base::DictValue& root,
                                    const base::DictValue& directory,
                                    int depth) {
  const std::string* link = directory.FindString("link");
  if (!link) {
    return directory.FindDict("files");
  }
  if (depth >= kMaxLinkDepth) {
    return nullptr;
  }
  const base::DictValue* linked_node = GetNodeFromPath(*link, root, depth + 1);
  return linked_node ? linked_node->FindDict("files") : nullptr;
}

const base::DictValue* GetChildNode(const base::DictValue& root,
                                    const std::string& name,
                                    const base::DictValue& directory,
                                    int depth) {
  if (name.empty()) {
    return &root;
  }
  const base::DictValue* files = GetFilesNode(root, directory, depth);
  return files ? files->FindDict(name) : nullptr;
}

const base::DictValue* GetNodeFromPath(std::string path,
                                       const base::DictValue& root,
                                       int depth) {
  if (path.empty()) {
    return &root;
  }

  const base::DictValue* directory = &root;
  for (size_t separator = path.find_first_of(kSeparators);
       separator != std::string::npos;
       separator = path.find_first_of(kSeparators)) {
    directory =
        GetChildNode(root, path.substr(0, separator), *directory, depth);
    if (!directory) {
      return nullptr;
    }
    path.erase(0, separator + 1);
  }
  return GetChildNode(root, path, *directory, depth);
}

bool ReadNonNegativeInteger(const base::DictValue& node,
                            std::string_view key,
                            uint64_t* result) {
  const base::Value* value = node.Find(key);
  if (!value) {
    return false;
  }
  if (value->is_int()) {
    const int integer = value->GetInt();
    if (integer < 0) {
      return false;
    }
    *result = static_cast<uint64_t>(integer);
    return true;
  }
  if (value->is_double()) {
    const double number = value->GetDouble();
    constexpr double kMaxExactJsonInteger = 9007199254740991.0;
    if (number < 0 || number > kMaxExactJsonInteger ||
        number != static_cast<uint64_t>(number)) {
      return false;
    }
    *result = static_cast<uint64_t>(number);
    return true;
  }
  return false;
}

bool FillFileInfo(const base::DictValue& node,
                  uint32_t header_size,
                  uint64_t archive_size,
                  Archive::FileInfo* info) {
  *info = Archive::FileInfo();
  if (!ReadNonNegativeInteger(node, "size", &info->size)) {
    return false;
  }

  info->unpacked = node.FindBool("unpacked").value_or(false);
  info->encrypted = node.FindBool("encrypted").value_or(false);
  if ((node.Find("unpacked") && !node.FindBool("unpacked")) ||
      (node.Find("encrypted") && !node.FindBool("encrypted"))) {
    return false;
  }
  if (info->unpacked) {
    // Unpacked contents are stored as ordinary files by this format.
    return true;
  }
  if (info->encrypted && info->size % AES_BLOCK_SIZE != 0) {
    return false;
  }

  const std::string* offset = node.FindString("offset");
  uint64_t relative_offset = 0;
  if (!offset || !base::StringToUint64(*offset, &relative_offset) ||
      relative_offset > std::numeric_limits<uint64_t>::max() - header_size) {
    return false;
  }

  info->offset = relative_offset + header_size;
  return info->offset <= archive_size &&
         info->size <= archive_size - info->offset;
}

bool IsSafeRelativePath(const base::FilePath& path) {
  return !path.empty() && !path.IsAbsolute() && !path.ReferencesParent();
}

bool ValidateDirectory(const base::DictValue& directory,
                       uint32_t header_size,
                       uint64_t archive_size,
                       bool has_key,
                       int depth = 0) {
  const auto* files = directory.FindDict("files");
  if (!files || depth > 128) {
    return false;
  }
  for (const auto [name, value] : *files) {
    const auto path = base::FilePath::FromUTF8Unsafe(name);
    if (!IsSafeRelativePath(path) || path.BaseName() != path || name == "." ||
        !value.is_dict()) {
      return false;
    }
    const auto& node = value.GetDict();
    if (const auto* link = node.FindString("link")) {
      if (!IsSafeRelativePath(base::FilePath::FromUTF8Unsafe(*link))) {
        return false;
      }
    } else if (node.Find("files")) {
      if (!ValidateDirectory(node, header_size, archive_size, has_key,
                             depth + 1)) {
        return false;
      }
    } else {
      Archive::FileInfo info;
      if (!FillFileInfo(node, header_size, archive_size, &info) ||
          (info.encrypted && !info.unpacked && !has_key)) {
        return false;
      }
    }
  }
  return true;
}

const base::DictValue* ResolveNode(const base::FilePath& path,
                                   const base::DictValue& header) {
  auto* node = GetNodeFromPath(path.AsUTF8Unsafe(), header, 0);
  for (int depth = 0; node && depth < kMaxLinkDepth; ++depth) {
    const std::string* link = node->FindString("link");
    if (!link) {
      return node;
    }
    const auto target = base::FilePath::FromUTF8Unsafe(*link);
    if (!IsSafeRelativePath(target)) {
      return nullptr;
    }
    node = GetNodeFromPath(*link, header, depth + 1);
  }
  return nullptr;
}

}  // namespace

Archive::Archive(const base::FilePath& path)
    : path_(path),
      file_(path,
            base::File::FLAG_OPEN | base::File::FLAG_READ |
                base::File::FLAG_WIN_SHARE_DELETE) {}

Archive::~Archive() = default;

bool Archive::Init() {
  return InitWithPublicKey({});
}

bool Archive::InitWithPublicKey(const std::string& public_key_pem) {
  CHECK(!initialized_);
  initialized_ = true;
  if (!file_.IsValid()) {
    LOG(WARNING) << "Unable to open ASAR archive " << path_;
    return false;
  }

  const int64_t length = file_.GetLength();
  if (length < 8) {
    return false;
  }
  archive_size_ = static_cast<uint64_t>(length);

  std::vector<uint8_t> size_buffer(8);
  if (!ReadInChunks(file_, 0, size_buffer)) {
    return false;
  }

  uint32_t serialized_header_size = 0;
  auto size_pickle = base::PickleIterator::WithData(size_buffer);
  if (!HasExactPickleLength(size_buffer) ||
      !size_pickle.ReadUInt32(&serialized_header_size) ||
      !size_pickle.ReachedEnd() || serialized_header_size < 8 ||
      serialized_header_size > kMaxHeaderSize ||
      serialized_header_size > archive_size_ - 8) {
    return false;
  }

  std::vector<uint8_t> header_buffer(serialized_header_size);
  if (!ReadInChunks(file_, 8, header_buffer)) {
    return false;
  }

  std::string json_header;
  auto header_pickle = base::PickleIterator::WithData(header_buffer);
  header_size_ = 8 + serialized_header_size;
  if (!HasExactPickleLength(header_buffer) ||
      !header_pickle.ReadString(&json_header) || !header_pickle.ReachedEnd()) {
    // Legacy security-asar stores an RSA-2048 signature-style envelope here,
    // followed by the AES-encrypted JSON. Only explicitly configured keys are
    // considered; a failed decode never falls back to serving ciphertext.
    if (serialized_header_size != 256) {
      return false;
    }
    auto rsa = ReadPublicKey(public_key_pem);
    std::array<uint8_t, 256> envelope;
    size_t envelope_size = 0;
    if (!rsa || !RSA_verify_raw(rsa.get(), &envelope_size, envelope.data(),
                                envelope.size(), header_buffer.data(),
                                header_buffer.size(), RSA_PKCS1_PADDING)) {
      return false;
    }
    auto envelope_pickle = base::PickleIterator::WithData(
        base::span(envelope).first(envelope_size));
    uint32_t ciphertext_size = 0;
    std::string expected_md5;
    std::string aes_key;
    if (!HasExactPickleLength(base::span(envelope).first(envelope_size)) ||
        !envelope_pickle.ReadUInt32(&ciphertext_size) ||
        !envelope_pickle.ReadString(&expected_md5) ||
        !envelope_pickle.ReadString(&aes_key) ||
        !envelope_pickle.ReachedEnd() || aes_key.size() != 16 ||
        ciphertext_size == 0 || ciphertext_size % AES_BLOCK_SIZE != 0 ||
        ciphertext_size > kMaxHeaderSize ||
        ciphertext_size > archive_size_ - header_size_) {
      OPENSSL_cleanse(envelope.data(), envelope.size());
      OPENSSL_cleanse(aes_key.data(), aes_key.size());
      return false;
    }
    std::vector<uint8_t> ciphertext(ciphertext_size);
    std::array<uint8_t, MD5_DIGEST_LENGTH> expected_digest;
    std::array<uint8_t, MD5_DIGEST_LENGTH> digest;
    auto key = std::make_shared<ArchiveDecryptionKey>();
    // MD5 is part of this legacy on-disk format, not a new authenticity claim.
    const bool valid =
        base::HexStringToSpan(expected_md5, expected_digest) &&
        ReadInChunks(file_, header_size_, ciphertext) &&
        MD5(ciphertext.data(), ciphertext.size(), digest.data()) &&
        CRYPTO_memcmp(digest.data(), expected_digest.data(), digest.size()) ==
            0 &&
        AES_set_decrypt_key(reinterpret_cast<const uint8_t*>(aes_key.data()),
                            128, &key->aes) == 0;
    OPENSSL_cleanse(envelope.data(), envelope.size());
    OPENSSL_cleanse(aes_key.data(), aes_key.size());
    if (!valid) {
      return false;
    }
    DecryptBlocks(*key, ciphertext);
    json_header.assign(reinterpret_cast<const char*>(ciphertext.data()),
                       ciphertext.size());
    header_size_ += ciphertext_size;
    key_ = std::move(key);
  }

  std::optional<base::Value> value =
      base::JSONReader::Read(json_header, base::JSON_PARSE_RFC);
  if (!value || !value->is_dict() ||
      !ValidateDirectory(value->GetDict(), header_size_, archive_size_,
                         !!key_)) {
    return false;
  }

  header_ = std::move(*value).TakeDict();
  return true;
}

bool Archive::GetFileInfo(const base::FilePath& path, FileInfo* info) const {
  if (!header_ || !info || !IsSafeRelativePath(path)) {
    return false;
  }

  const base::DictValue* node = ResolveNode(path, *header_);
  if (!node) {
    return false;
  }

  return FillFileInfo(*node, header_size_, archive_size_, info);
}

bool Archive::GetUnpackedPath(const base::FilePath& path,
                              base::FilePath* unpacked_path) const {
  base::FilePath resolved;
  if (!unpacked_path || path.empty() || !ResolvePath(path, &resolved)) {
    return false;
  }
  *unpacked_path =
      path_.AddExtension(FILE_PATH_LITERAL("unpacked")).Append(resolved);
  return true;
}

bool Archive::ResolvePath(const base::FilePath& path,
                          base::FilePath* canonical_path) const {
  if (!header_ || !canonical_path) {
    return false;
  }
  if (path.empty()) {
    *canonical_path = base::FilePath();
    return true;
  }
  if (!IsSafeRelativePath(path)) {
    return false;
  }
  auto resolved = path;
  for (int depth = 0; depth < kMaxLinkDepth; ++depth) {
    const auto components = resolved.GetComponents();
    base::FilePath prefix;
    bool followed_link = false;
    for (size_t index = 0; index < components.size(); ++index) {
      prefix = prefix.Append(components[index]);
      const auto* node = GetNodeFromPath(prefix.AsUTF8Unsafe(), *header_, 0);
      if (!node) {
        return false;
      }
      if (const auto* link = node->FindString("link")) {
        resolved = base::FilePath::FromUTF8Unsafe(*link);
        if (!IsSafeRelativePath(resolved)) {
          return false;
        }
        for (++index; index < components.size(); ++index) {
          resolved = resolved.Append(components[index]);
        }
        followed_link = true;
        break;
      }
    }
    if (!followed_link) {
      *canonical_path = std::move(prefix);
      return true;
    }
  }
  return false;
}

bool Archive::StatPath(const base::FilePath& path,
                       FileInfo* info,
                       bool* is_directory) const {
  if (!header_ || !is_directory) {
    return false;
  }
  if (path.empty()) {
    *is_directory = true;
    if (info) {
      *info = FileInfo();
    }
    return true;
  }
  if (!IsSafeRelativePath(path)) {
    return false;
  }

  const base::DictValue* node = ResolveNode(path, *header_);
  if (!node) {
    return false;
  }
  if (node->FindDict("files")) {
    *is_directory = true;
    if (info) {
      *info = FileInfo();
    }
    return true;
  }

  *is_directory = false;
  FileInfo local_info;
  FileInfo* out = info ? info : &local_info;
  return FillFileInfo(*node, header_size_, archive_size_, out);
}

bool Archive::ReadFile(const base::FilePath& path,
                       std::string* contents) const {
  if (!contents) {
    return false;
  }
  auto reader = CreateReader(path);
  if (!reader || reader->size() > std::numeric_limits<size_t>::max()) {
    return false;
  }
  contents->assign(static_cast<size_t>(reader->size()), '\0');
  return reader->Read(0, base::as_writable_byte_span(*contents)) ==
         contents->size();
}

std::unique_ptr<Archive::EntryReader> Archive::CreateReader(
    const base::FilePath& path) const {
  FileInfo info;
  bool is_directory = false;
  if (!StatPath(path, &info, &is_directory) || is_directory) {
    return nullptr;
  }
  base::File file;
  if (info.unpacked) {
    base::FilePath unpacked_path;
    if (!GetUnpackedPath(path, &unpacked_path)) {
      return nullptr;
    }
    file.Initialize(unpacked_path, base::File::FLAG_OPEN |
                                       base::File::FLAG_READ |
                                       base::File::FLAG_WIN_SHARE_DELETE);
    const int64_t length = file.GetLength();
    if (length < 0) {
      return nullptr;
    }
    info.size = length;
    info.offset = 0;
    info.encrypted = false;
  } else {
    file = DuplicateFile();
  }
  if (!file.IsValid() || (info.encrypted && !key_)) {
    return nullptr;
  }
  return std::unique_ptr<EntryReader>(
      new EntryReader(std::move(file), info.offset, info.size,
                      info.encrypted ? key_ : nullptr));
}

Archive::EntryReader::EntryReader(
    base::File file,
    uint64_t offset,
    uint64_t size,
    std::shared_ptr<const ArchiveDecryptionKey> key)
    : file_(std::move(file)),
      offset_(offset),
      size_(size),
      key_(std::move(key)) {}

Archive::EntryReader::~EntryReader() = default;

std::optional<size_t> Archive::EntryReader::Read(
    uint64_t offset,
    base::span<uint8_t> output) const {
  if (offset >= size_ || output.empty()) {
    return 0;
  }
  const size_t count =
      static_cast<size_t>(std::min<uint64_t>(output.size(), size_ - offset));
  base::AutoLock lock(lock_);
  output = output.first(count);
  if (!key_) {
    return ReadInChunks(file_, offset_ + offset, output)
               ? std::make_optional(count)
               : std::nullopt;
  }

  // ECB blocks are independent. Only the partial first and last blocks need
  // scratch space. The middle is read directly into the caller's buffer in
  // bounded I/O chunks, including requests larger than base::File's int limit.
  // At most 30 extra bytes are read for the complete logical request.
  std::array<uint8_t, AES_BLOCK_SIZE> partial;
  const size_t prefix = static_cast<size_t>(offset % AES_BLOCK_SIZE);
  if (prefix != 0) {
    if (!ReadInChunks(file_, offset_ + offset - prefix, partial)) {
      return std::nullopt;
    }
    DecryptBlocks(*key_, partial);
    const size_t copied = std::min(output.size(), partial.size() - prefix);
    output.first(copied).copy_from(base::span(partial).subspan(prefix, copied));
    output = output.subspan(copied);
    offset += copied;
  }
  const size_t whole_bytes = output.size() / AES_BLOCK_SIZE * AES_BLOCK_SIZE;
  if (whole_bytes != 0) {
    auto bytes = output.first(whole_bytes);
    if (!ReadInChunks(file_, offset_ + offset, bytes)) {
      return std::nullopt;
    }
    DecryptBlocks(*key_, bytes);
    output = output.subspan(whole_bytes);
    offset += whole_bytes;
  }
  if (!output.empty()) {
    if (!ReadInChunks(file_, offset_ + offset, partial)) {
      return std::nullopt;
    }
    DecryptBlocks(*key_, partial);
    output.copy_from(base::span(partial).first(output.size()));
  }
  return count;
}

base::File Archive::DuplicateFile() const {
  return file_.Duplicate();
}

bool GetAsarArchivePath(const base::FilePath& full_path,
                        base::FilePath* asar_path,
                        base::FilePath* relative_path,
                        bool allow_root) {
  if (!asar_path || !relative_path) {
    return false;
  }

  using StringType = base::FilePath::StringType;
  const StringType& value = full_path.value();
  size_t end = value.size();
  while (end > 0 && base::FilePath::IsSeparator(value[end - 1])) {
    --end;
  }

  size_t component_end = end;
  bool leaf = true;
  while (component_end > 0) {
    size_t start = component_end;
    while (start > 0 && !base::FilePath::IsSeparator(value[start - 1])) {
      --start;
    }
    const base::FilePath::StringViewType component =
        base::FilePath::StringViewType{value}.substr(start,
                                                     component_end - start);
    if (component.size() >= 5 &&
        base::FilePath(component).MatchesExtension(kAsarExtension)) {
      base::FilePath candidate =
          leaf ? full_path : base::FilePath(value.substr(0, component_end));
      if (!base::DirectoryExists(candidate)) {
        if (leaf && !allow_root) {
          return false;
        }
        base::FilePath tail;
        size_t position = component_end;
        while (position < end) {
          while (position < end &&
                 base::FilePath::IsSeparator(value[position])) {
            ++position;
          }
          size_t next = position;
          while (next < end && !base::FilePath::IsSeparator(value[next])) {
            ++next;
          }
          if (next > position) {
            tail = tail.Append(base::FilePath::StringViewType{value}.substr(
                position, next - position));
          }
          position = next;
        }
        if (!allow_root && !IsSafeRelativePath(tail)) {
          return false;
        }
        *asar_path = std::move(candidate);
        *relative_path = std::move(tail);
        return true;
      }
    }

    if (start == 0) {
      return false;
    }
    leaf = false;
    component_end = start;
    while (component_end > 0 &&
           base::FilePath::IsSeparator(value[component_end - 1])) {
      --component_end;
    }
  }
  return false;
}

std::shared_ptr<Archive> GetOrCreateAsarArchive(const base::FilePath& path) {
  const auto absolute = base::MakeAbsoluteFilePath(path);
  if (absolute.empty()) {
    return nullptr;
  }
  const auto normalized = absolute.NormalizePathSeparators();
  for (;;) {
    std::string public_key;
    uint64_t generation;
    {
      base::AutoLock lock(GetArchiveCacheLock());
      auto& registry = GetArchiveRegistry();
      const auto cached = registry.cache.find(normalized);
      if (cached != registry.cache.end()) {
        return cached->second;
      }
      generation = registry.generation;
      for (const auto& [owner, configs] : registry.owners) {
        for (const auto& config : configs) {
          if (PathContains(config.root_path, normalized)) {
            public_key = config.public_key_pem;
          }
        }
      }
    }
    // Opening, RSA/AES decoding, and validating the index can block. None of
    // them runs while holding the process-wide registry/cache lock.
    auto archive = std::make_shared<Archive>(normalized);
    const bool initialized = archive->InitWithPublicKey(public_key);
    base::AutoLock lock(GetArchiveCacheLock());
    auto& registry = GetArchiveRegistry();
    if (generation != registry.generation) {
      continue;
    }
    if (!initialized) {
      return nullptr;
    }
    return registry.cache.try_emplace(normalized, std::move(archive))
        .first->second;
  }
}

bool SetArchivePublicKeys(const std::string& owner_id,
                          const std::vector<ArchiveKeyConfig>& configs) {
  if (owner_id.empty()) {
    return false;
  }
  std::vector<ArchiveKeyConfig> normalized;
  for (const auto& config : configs) {
    auto key = ReadPublicKey(config.public_key_pem);
    if (!config.root_path.IsAbsolute() || config.root_path.ReferencesParent() ||
        !key) {
      return false;
    }
    auto absolute = base::MakeAbsoluteFilePath(config.root_path);
    if (absolute.empty()) {
      return false;
    }
    bssl::UniquePtr<BIO> canonical(BIO_new(BIO_s_mem()));
    const uint8_t* bytes = nullptr;
    size_t size = 0;
    if (!PEM_write_bio_RSA_PUBKEY(canonical.get(), key.get()) ||
        !BIO_mem_contents(canonical.get(), &bytes, &size)) {
      return false;
    }
    normalized.push_back(
        {absolute.NormalizePathSeparators().StripTrailingSeparators(),
         std::string(reinterpret_cast<const char*>(bytes), size)});
  }
  auto same_config = [](const auto& first, const auto& second) {
    return !PathLess()(first.root_path, second.root_path) &&
           !PathLess()(second.root_path, first.root_path) &&
           first.public_key_pem == second.public_key_pem;
  };
  std::sort(normalized.begin(), normalized.end(),
            [](const auto& first, const auto& second) {
              if (PathLess()(first.root_path, second.root_path)) {
                return true;
              }
              if (PathLess()(second.root_path, first.root_path)) {
                return false;
              }
              return first.public_key_pem < second.public_key_pem;
            });
  normalized.erase(
      std::unique(normalized.begin(), normalized.end(), same_config),
      normalized.end());
  base::AutoLock lock(GetArchiveCacheLock());
  auto& registry = GetArchiveRegistry();
  const auto previous_owner = registry.owners.find(owner_id);
  if (previous_owner == registry.owners.end() && normalized.empty()) {
    return true;
  }
  if (previous_owner != registry.owners.end() &&
      std::equal(normalized.begin(), normalized.end(),
                 previous_owner->second.begin(), previous_owner->second.end(),
                 same_config)) {
    return true;
  }
  auto conflicts = [](const auto& first, const auto& second) {
    return first.public_key_pem != second.public_key_pem &&
           (PathContains(first.root_path, second.root_path) ||
            PathContains(second.root_path, first.root_path));
  };
  for (const auto& candidate : normalized) {
    for (const auto& other : normalized) {
      if (conflicts(candidate, other)) {
        return false;
      }
    }
    for (const auto& [owner, previous] : registry.owners) {
      if (owner == owner_id) {
        continue;
      }
      for (const auto& other : previous) {
        if (conflicts(candidate, other)) {
          return false;
        }
      }
    }
  }
  if (normalized.empty()) {
    registry.owners.erase(owner_id);
  } else {
    registry.owners.insert_or_assign(owner_id, std::move(normalized));
  }
  registry.cache.clear();
  ++registry.generation;
  return true;
}

void RemoveArchivePublicKeys(const std::string& owner_id) {
  base::AutoLock lock(GetArchiveCacheLock());
  auto& registry = GetArchiveRegistry();
  if (registry.owners.erase(owner_id)) {
    registry.cache.clear();
    ++registry.generation;
  }
}

}  // namespace xenon::asar
