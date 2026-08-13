#ifndef XENON_OVERLAY_CHROME_BROWSER_XENON_ENCRYPTED_EXTENSION_PACKAGE_H_
#define XENON_OVERLAY_CHROME_BROWSER_XENON_ENCRYPTED_EXTENSION_PACKAGE_H_

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/files/file_path.h"
#include "base/values.h"
#include "base/version.h"

namespace xenon::internal {

struct EncryptedExtensionUpdateMetadata {
  EncryptedExtensionUpdateMetadata();
  EncryptedExtensionUpdateMetadata(
      const EncryptedExtensionUpdateMetadata& other);
  EncryptedExtensionUpdateMetadata(EncryptedExtensionUpdateMetadata&& other);
  EncryptedExtensionUpdateMetadata& operator=(
      const EncryptedExtensionUpdateMetadata& other);
  EncryptedExtensionUpdateMetadata& operator=(
      EncryptedExtensionUpdateMetadata&& other);
  ~EncryptedExtensionUpdateMetadata();

  std::string version;
  std::string url;
  std::string sha256;
  std::string signature;
  std::string zip_filename;
};

struct EncryptedExtensionPackage {
  EncryptedExtensionPackage();
  EncryptedExtensionPackage(EncryptedExtensionPackage&& other);
  EncryptedExtensionPackage& operator=(EncryptedExtensionPackage&& other);
  ~EncryptedExtensionPackage();

  EncryptedExtensionPackage(const EncryptedExtensionPackage&) = delete;
  EncryptedExtensionPackage& operator=(const EncryptedExtensionPackage&) =
      delete;

  std::string archive_sha256;
  base::DictValue manifest;
  base::Version version;
  std::vector<std::pair<base::FilePath, std::string>> resources;
};

struct EncryptedExtensionSelection {
  EncryptedExtensionSelection();
  EncryptedExtensionSelection(EncryptedExtensionSelection&& other);
  EncryptedExtensionSelection& operator=(EncryptedExtensionSelection&& other);
  ~EncryptedExtensionSelection();

  EncryptedExtensionSelection(const EncryptedExtensionSelection&) = delete;
  EncryptedExtensionSelection& operator=(const EncryptedExtensionSelection&) =
      delete;

  std::optional<EncryptedExtensionPackage> package;
  bool is_update = false;
  std::string error;
};

std::string BuildUpdateSignaturePayload(
    const EncryptedExtensionUpdateMetadata& metadata);

bool VerifyUpdateMetadataSignature(
    const EncryptedExtensionUpdateMetadata& metadata,
    const std::string& public_key);

std::optional<EncryptedExtensionPackage> ReadEncryptedExtensionPackage(
    const base::FilePath& zip_path,
    const std::string& password,
    const std::string& expected_extension_id,
    std::string* error);

EncryptedExtensionSelection LoadBestEncryptedExtensionPackage(
    const base::FilePath& builtin_zip_path,
    const base::FilePath& update_directory,
    const std::string& password,
    const std::string& expected_extension_id);

bool FileMatchesSha256(const base::FilePath& path,
                       const std::string& expected_sha256);

bool PrepareEncryptedExtensionUpdate(const base::FilePath& downloaded_zip_path,
                                     const base::FilePath& update_directory,
                                     const std::string& password,
                                     const std::string& expected_extension_id,
                                     const std::string& trusted_public_key,
                                     EncryptedExtensionUpdateMetadata metadata,
                                     std::string* error);

}  // namespace xenon::internal

#endif  // XENON_OVERLAY_CHROME_BROWSER_XENON_ENCRYPTED_EXTENSION_PACKAGE_H_
