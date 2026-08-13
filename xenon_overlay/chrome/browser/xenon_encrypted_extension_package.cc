#include "xenon_overlay/chrome/browser/xenon_encrypted_extension_package.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <set>
#include <string_view>

#include "base/base64.h"
#include "base/files/file.h"
#include "base/files/file_util.h"
#include "base/files/important_file_writer.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "components/crx_file/id_util.h"
#include "crypto/hash.h"
#include "crypto/keypair.h"
#include "crypto/sign.h"
#include "extensions/common/extension.h"
#include "extensions/common/manifest_constants.h"
#include "third_party/zlib/google/zip_reader.h"
#include "url/gurl.h"

namespace xenon::internal {

namespace {

constexpr size_t kMaxArchiveBytes = 10 * 1024 * 1024;
constexpr size_t kMaxUncompressedBytes = 256 * 1024 * 1024;
constexpr size_t kMaxUpdateMetadataBytes = 64 * 1024;
constexpr std::string_view kUpdateSignatureContext =
    "xenon-encrypted-extension-update-v1";

void SetError(std::string* error, std::string message) {
  if (error) {
    *error = std::move(message);
  }
}

bool IsCanonicalSha256(const std::string& sha256) {
  std::vector<uint8_t> digest;
  return sha256.size() == crypto::hash::kSha256Size * 2 &&
         base::HexStringToBytes(sha256, &digest) &&
         digest.size() == crypto::hash::kSha256Size &&
         sha256 == base::HexEncodeLower(digest);
}

std::optional<std::string> CalculateFileSha256(const base::FilePath& path) {
  base::File file(path, base::File::FLAG_OPEN | base::File::FLAG_READ);
  if (!file.IsValid()) {
    return std::nullopt;
  }

  std::array<uint8_t, crypto::hash::kSha256Size> digest;
  if (!crypto::hash::HashFile(crypto::hash::kSha256, &file, digest)) {
    return std::nullopt;
  }
  return base::HexEncodeLower(digest);
}

const std::string* GetPublicKey(const base::DictValue& manifest) {
  return manifest.FindString(extensions::manifest_keys::kPublicKey);
}

bool ManifestMatchesExpectedId(const base::DictValue& manifest,
                               const std::string& expected_extension_id) {
  const std::string* raw_key = GetPublicKey(manifest);
  std::string public_key_bytes;
  return raw_key &&
         extensions::Extension::ParsePEMKeyBytes(*raw_key, &public_key_bytes) &&
         crx_file::id_util::GenerateId(public_key_bytes) ==
             expected_extension_id;
}

std::optional<EncryptedExtensionUpdateMetadata> ReadUpdateMetadata(
    const base::FilePath& path,
    std::string* error) {
  std::string contents;
  if (!base::ReadFileToStringWithMaxSize(path, &contents,
                                         kMaxUpdateMetadataBytes)) {
    SetError(error, "cannot read encrypted extension update metadata");
    return std::nullopt;
  }

  std::optional<base::DictValue> value =
      base::JSONReader::ReadDict(contents, base::JSON_PARSE_RFC);
  if (!value) {
    SetError(error, "invalid encrypted extension update metadata");
    return std::nullopt;
  }

  const std::string* version = value->FindString("version");
  const std::string* url = value->FindString("url");
  const std::string* sha256 = value->FindString("sha256");
  const std::string* signature = value->FindString("signature");
  const std::string* zip_filename = value->FindString("zip");
  if (!version || !url || !sha256 || !signature || !zip_filename) {
    SetError(error, "encrypted extension update metadata is incomplete");
    return std::nullopt;
  }

  base::FilePath filename = base::FilePath::FromUTF8Unsafe(*zip_filename);
  if (filename.empty() || filename.BaseName() != filename) {
    SetError(error, "encrypted extension update filename is unsafe");
    return std::nullopt;
  }

  EncryptedExtensionUpdateMetadata metadata;
  metadata.version = *version;
  metadata.url = *url;
  metadata.sha256 = *sha256;
  metadata.signature = *signature;
  metadata.zip_filename = *zip_filename;
  return metadata;
}

}  // namespace

EncryptedExtensionUpdateMetadata::EncryptedExtensionUpdateMetadata() = default;
EncryptedExtensionUpdateMetadata::EncryptedExtensionUpdateMetadata(
    const EncryptedExtensionUpdateMetadata& other) = default;
EncryptedExtensionUpdateMetadata::EncryptedExtensionUpdateMetadata(
    EncryptedExtensionUpdateMetadata&& other) = default;
EncryptedExtensionUpdateMetadata& EncryptedExtensionUpdateMetadata::operator=(
    const EncryptedExtensionUpdateMetadata& other) = default;
EncryptedExtensionUpdateMetadata& EncryptedExtensionUpdateMetadata::operator=(
    EncryptedExtensionUpdateMetadata&& other) = default;
EncryptedExtensionUpdateMetadata::~EncryptedExtensionUpdateMetadata() = default;

EncryptedExtensionPackage::EncryptedExtensionPackage() = default;
EncryptedExtensionPackage::EncryptedExtensionPackage(
    EncryptedExtensionPackage&& other) = default;
EncryptedExtensionPackage& EncryptedExtensionPackage::operator=(
    EncryptedExtensionPackage&& other) = default;
EncryptedExtensionPackage::~EncryptedExtensionPackage() = default;

EncryptedExtensionSelection::EncryptedExtensionSelection() = default;
EncryptedExtensionSelection::EncryptedExtensionSelection(
    EncryptedExtensionSelection&& other) = default;
EncryptedExtensionSelection& EncryptedExtensionSelection::operator=(
    EncryptedExtensionSelection&& other) = default;
EncryptedExtensionSelection::~EncryptedExtensionSelection() = default;

std::string BuildUpdateSignaturePayload(
    const EncryptedExtensionUpdateMetadata& metadata) {
  return base::StrCat({kUpdateSignatureContext, "\n", metadata.version, "\n",
                       metadata.url, "\n", metadata.sha256});
}

bool VerifyUpdateMetadataSignature(
    const EncryptedExtensionUpdateMetadata& metadata,
    const std::string& public_key) {
  const GURL update_url(metadata.url);
  if (!base::Version(metadata.version).IsValid() || !update_url.is_valid() ||
      !update_url.SchemeIsHTTPOrHTTPS() ||
      !IsCanonicalSha256(metadata.sha256)) {
    return false;
  }

  std::optional<std::vector<uint8_t>> signature =
      base::Base64Decode(metadata.signature);
  std::string public_key_bytes;
  if (!signature ||
      !extensions::Extension::ParsePEMKeyBytes(public_key, &public_key_bytes)) {
    return false;
  }

  std::optional<crypto::keypair::PublicKey> key =
      crypto::keypair::PublicKey::FromSubjectPublicKeyInfo(
          base::as_byte_span(public_key_bytes));
  if (!key || !key->IsRsa()) {
    return false;
  }

  const std::string payload = BuildUpdateSignaturePayload(metadata);
  return crypto::sign::Verify(crypto::sign::RSA_PKCS1_SHA256, *key,
                              base::as_byte_span(payload), *signature);
}

std::optional<EncryptedExtensionPackage> ReadEncryptedExtensionPackage(
    const base::FilePath& zip_path,
    const std::string& password,
    const std::string& expected_extension_id,
    std::string* error) {
  std::string archive;
  if (!base::ReadFileToStringWithMaxSize(zip_path, &archive,
                                         kMaxArchiveBytes)) {
    SetError(error, "cannot read encrypted component extension ZIP");
    return std::nullopt;
  }

  zip::ZipReader reader;
  reader.SetPassword(password);
  if (!reader.OpenFromString(archive)) {
    SetError(error, "cannot open encrypted component extension ZIP");
    return std::nullopt;
  }

  std::vector<std::pair<base::FilePath, std::string>> resources;
  std::set<base::FilePath> resource_paths;
  size_t total_size = 0;
  while (const zip::ZipReader::Entry* entry = reader.Next()) {
    if (entry->is_directory) {
      continue;
    }
    if (entry->is_unsafe || entry->is_symbolic_link || !entry->is_encrypted ||
        entry->uses_aes_encryption) {
      SetError(error,
               "encrypted component extension ZIP contains an unsafe, "
               "unencrypted, or unsupported entry");
      return std::nullopt;
    }
    if (entry->original_size < 0 ||
        static_cast<uint64_t>(entry->original_size) >
            kMaxUncompressedBytes - total_size) {
      SetError(error, "encrypted component extension ZIP exceeds memory limit");
      return std::nullopt;
    }

    base::FilePath resource_path = entry->path.NormalizePathSeparators();
    if (resource_path.empty() || !resource_paths.insert(resource_path).second) {
      SetError(error,
               "encrypted component extension ZIP contains duplicate paths");
      return std::nullopt;
    }
    if (resources.size() == std::numeric_limits<uint16_t>::max()) {
      SetError(error,
               "encrypted component extension ZIP contains too many entries");
      return std::nullopt;
    }

    std::string contents;
    if (!reader.ExtractCurrentEntryToString(kMaxUncompressedBytes - total_size,
                                            &contents)) {
      SetError(error, "cannot decrypt component extension entry");
      return std::nullopt;
    }
    total_size += contents.size();
    resources.emplace_back(std::move(resource_path), std::move(contents));
  }
  if (!reader.ok()) {
    SetError(error, "cannot enumerate encrypted component extension ZIP");
    return std::nullopt;
  }

  auto manifest_entry = std::ranges::find_if(resources, [](const auto& item) {
    return item.first == base::FilePath(FILE_PATH_LITERAL("manifest.json"));
  });
  if (manifest_entry == resources.end()) {
    SetError(error, "component extension manifest is missing from ZIP");
    return std::nullopt;
  }

  std::optional<base::DictValue> manifest = base::JSONReader::ReadDict(
      manifest_entry->second, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
  if (!manifest) {
    SetError(error, "component extension manifest is invalid");
    return std::nullopt;
  }
  if (!ManifestMatchesExpectedId(*manifest, expected_extension_id)) {
    SetError(error, "component extension id does not match configuration");
    return std::nullopt;
  }

  const std::string* version_string = manifest->FindString("version");
  base::Version version(version_string ? *version_string : std::string());
  if (!version.IsValid()) {
    SetError(error, "component extension version is invalid");
    return std::nullopt;
  }

  EncryptedExtensionPackage package;
  package.archive_sha256 = base::HexEncodeLower(crypto::hash::Sha256(archive));
  package.manifest = std::move(*manifest);
  package.version = std::move(version);
  package.resources = std::move(resources);
  return package;
}

EncryptedExtensionSelection LoadBestEncryptedExtensionPackage(
    const base::FilePath& builtin_zip_path,
    const base::FilePath& update_directory,
    const std::string& password,
    const std::string& expected_extension_id) {
  EncryptedExtensionSelection result;
  std::string builtin_error;
  std::optional<EncryptedExtensionPackage> builtin =
      ReadEncryptedExtensionPackage(builtin_zip_path, password,
                                    expected_extension_id, &builtin_error);
  if (!builtin) {
    result.error = base::StrCat(
        {"cannot load built-in encrypted extension: ", builtin_error});
    return result;
  }

  const std::string* trusted_public_key = GetPublicKey(builtin->manifest);
  if (update_directory.empty() || !trusted_public_key) {
    result.package = std::move(builtin);
    return result;
  }

  const base::FilePath metadata_path =
      update_directory.AppendASCII("update.json");
  if (!base::PathExists(metadata_path)) {
    result.package = std::move(builtin);
    return result;
  }

  std::string update_error;
  std::optional<EncryptedExtensionUpdateMetadata> metadata =
      ReadUpdateMetadata(metadata_path, &update_error);
  if (!metadata ||
      !VerifyUpdateMetadataSignature(*metadata, *trusted_public_key)) {
    result.error = metadata ? "encrypted extension update signature is invalid"
                            : std::move(update_error);
    result.package = std::move(builtin);
    return result;
  }

  base::Version update_version(metadata->version);
  if (update_version <= builtin->version) {
    result.package = std::move(builtin);
    return result;
  }

  const base::FilePath update_path =
      update_directory.AppendASCII(metadata->zip_filename);
  if (!FileMatchesSha256(update_path, metadata->sha256)) {
    result.error = "encrypted extension update hash is invalid";
    result.package = std::move(builtin);
    return result;
  }

  std::optional<EncryptedExtensionPackage> update =
      ReadEncryptedExtensionPackage(update_path, password,
                                    expected_extension_id, &update_error);
  if (!update || update->version != update_version) {
    result.error =
        update ? "encrypted extension update metadata does not match package"
               : std::move(update_error);
    result.package = std::move(builtin);
    return result;
  }

  result.package = std::move(update);
  result.is_update = true;
  return result;
}

bool FileMatchesSha256(const base::FilePath& path,
                       const std::string& expected_sha256) {
  if (!IsCanonicalSha256(expected_sha256)) {
    return false;
  }
  std::optional<std::string> actual_sha256 = CalculateFileSha256(path);
  return actual_sha256 && *actual_sha256 == expected_sha256;
}

bool PrepareEncryptedExtensionUpdate(const base::FilePath& downloaded_zip_path,
                                     const base::FilePath& update_directory,
                                     const std::string& password,
                                     const std::string& expected_extension_id,
                                     const std::string& trusted_public_key,
                                     EncryptedExtensionUpdateMetadata metadata,
                                     std::string* error) {
  if (!VerifyUpdateMetadataSignature(metadata, trusted_public_key)) {
    SetError(error, "encrypted extension update signature is invalid");
    base::DeleteFile(downloaded_zip_path);
    return false;
  }
  if (!FileMatchesSha256(downloaded_zip_path, metadata.sha256)) {
    SetError(error, "downloaded extension package hash is invalid");
    base::DeleteFile(downloaded_zip_path);
    return false;
  }

  std::string package_error;
  std::optional<EncryptedExtensionPackage> package =
      ReadEncryptedExtensionPackage(downloaded_zip_path, password,
                                    expected_extension_id, &package_error);
  if (!package || package->version != base::Version(metadata.version)) {
    SetError(error, package
                        ? "downloaded extension package does not match metadata"
                        : std::move(package_error));
    base::DeleteFile(downloaded_zip_path);
    return false;
  }

  if (!base::CreateDirectory(update_directory)) {
    SetError(error, "cannot create encrypted extension update directory");
    base::DeleteFile(downloaded_zip_path);
    return false;
  }

  std::string old_filename;
  std::string ignored_error;
  if (std::optional<EncryptedExtensionUpdateMetadata> old_metadata =
          ReadUpdateMetadata(update_directory.AppendASCII("update.json"),
                             &ignored_error)) {
    old_filename = old_metadata->zip_filename;
    const base::Version old_version(old_metadata->version);
    if (VerifyUpdateMetadataSignature(*old_metadata, trusted_public_key) &&
        old_version.IsValid() &&
        old_version >= base::Version(metadata.version) &&
        FileMatchesSha256(
            update_directory.AppendASCII(old_metadata->zip_filename),
            old_metadata->sha256)) {
      base::DeleteFile(downloaded_zip_path);
      return true;
    }
  }

  metadata.zip_filename = base::StrCat({"extension-", metadata.sha256, ".zip"});
  const base::FilePath final_zip =
      update_directory.AppendASCII(metadata.zip_filename);
  base::FilePath staged_zip;
  if (!base::CreateTemporaryFileInDir(update_directory, &staged_zip) ||
      !base::CopyFile(downloaded_zip_path, staged_zip)) {
    SetError(error, "cannot stage encrypted extension update");
    base::DeleteFile(staged_zip);
    base::DeleteFile(downloaded_zip_path);
    return false;
  }
  base::DeleteFile(downloaded_zip_path);

  base::File::Error file_error = base::File::FILE_OK;
  if (!base::ReplaceFile(staged_zip, final_zip, &file_error)) {
    SetError(error, base::StrCat({"cannot store encrypted extension update: ",
                                  base::File::ErrorToString(file_error)}));
    base::DeleteFile(staged_zip);
    return false;
  }

  base::DictValue update;
  update.Set("zip", metadata.zip_filename);
  update.Set("version", metadata.version);
  update.Set("url", metadata.url);
  update.Set("sha256", metadata.sha256);
  update.Set("signature", metadata.signature);
  std::string serialized_update;
  if (!base::JSONWriter::Write(update, &serialized_update) ||
      !base::ImportantFileWriter::WriteFileAtomically(
          update_directory.AppendASCII("update.json"), serialized_update)) {
    SetError(error, "cannot store encrypted extension update metadata");
    if (old_filename != metadata.zip_filename) {
      base::DeleteFile(final_zip);
    }
    return false;
  }

  if (!old_filename.empty() && old_filename != metadata.zip_filename) {
    base::FilePath old_path = base::FilePath::FromUTF8Unsafe(old_filename);
    if (old_path.BaseName() == old_path) {
      base::DeleteFile(update_directory.Append(old_path));
    }
  }
  return true;
}

}  // namespace xenon::internal
