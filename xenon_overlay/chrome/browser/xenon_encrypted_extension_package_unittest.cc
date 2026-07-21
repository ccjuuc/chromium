#include "xenon_overlay/chrome/browser/xenon_encrypted_extension_package.h"

#include "base/base64.h"
#include "base/base_paths.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/path_service.h"
#include "base/strings/string_number_conversions.h"
#include "crypto/hash.h"
#include "crypto/keypair.h"
#include "crypto/sign.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace xenon::internal {

namespace {

constexpr char kExpectedExtensionId[] = "mkkhnfilihmphalmfjjbobdnaikhbeoi";
constexpr char kZipPassword[] = "xunlei@!@#$";

base::FilePath GetEncryptedExtensionPath() {
  base::FilePath source_root;
  CHECK(base::PathService::Get(base::DIR_SRC_TEST_DATA_ROOT, &source_root));
  return source_root.AppendASCII("xenon_overlay")
      .AppendASCII("resources")
      .AppendASCII("xenon_extension.zip");
}

TEST(XenonEncryptedExtensionPackageTest, ReadsEncryptedPackage) {
  std::string error;
  std::optional<EncryptedExtensionPackage> package =
      ReadEncryptedExtensionPackage(GetEncryptedExtensionPath(), kZipPassword,
                                    kExpectedExtensionId, &error);

  ASSERT_TRUE(package) << error;
  EXPECT_EQ(base::Version("1.0"), package->version);
  EXPECT_EQ(crypto::hash::kSha256Size * 2, package->archive_sha256.size());
  EXPECT_EQ(4u, package->resources.size());
}

TEST(XenonEncryptedExtensionPackageTest, RejectsWrongPassword) {
  std::string error;
  EXPECT_FALSE(ReadEncryptedExtensionPackage(GetEncryptedExtensionPath(),
                                             "wrong-password",
                                             kExpectedExtensionId, &error));
  EXPECT_FALSE(error.empty());
}

TEST(XenonEncryptedExtensionPackageTest, VerifiesSignedUpdateMetadata) {
  crypto::keypair::PrivateKey private_key =
      crypto::keypair::PrivateKey::GenerateRsa2048();
  const std::string public_key =
      base::Base64Encode(private_key.ToSubjectPublicKeyInfo());

  EncryptedExtensionUpdateMetadata metadata;
  metadata.version = "2.0";
  metadata.url = "https://updates.example.test/xenon-extension.zip";
  metadata.sha256 =
      base::HexEncodeLower(crypto::hash::Sha256("extension archive"));
  const std::string payload = BuildUpdateSignaturePayload(metadata);
  metadata.signature = base::Base64Encode(
      crypto::sign::Sign(crypto::sign::RSA_PKCS1_SHA256, private_key,
                         base::as_byte_span(payload)));

  EXPECT_TRUE(VerifyUpdateMetadataSignature(metadata, public_key));
  metadata.version = "2.1";
  EXPECT_FALSE(VerifyUpdateMetadataSignature(metadata, public_key));
}

TEST(XenonEncryptedExtensionPackageTest, FallsBackFromUnsignedUpdate) {
  base::ScopedTempDir update_directory;
  ASSERT_TRUE(update_directory.CreateUniqueTempDir());
  ASSERT_TRUE(base::WriteFile(
      update_directory.GetPath().AppendASCII("update.json"),
      R"({"zip":"extension.zip","version":"2.0",)"
      R"("url":"https://updates.example.test/extension.zip",)"
      R"("sha256":"0000000000000000000000000000000000000000000000000000000000000000",)"
      R"("signature":"invalid"})"));

  EncryptedExtensionSelection selection = LoadBestEncryptedExtensionPackage(
      GetEncryptedExtensionPath(), update_directory.GetPath(), kZipPassword,
      kExpectedExtensionId);

  ASSERT_TRUE(selection.package);
  EXPECT_FALSE(selection.is_update);
  EXPECT_EQ(base::Version("1.0"), selection.package->version);
  EXPECT_FALSE(selection.error.empty());
}

TEST(XenonEncryptedExtensionPackageTest, MatchesFileSha256) {
  base::ScopedTempDir directory;
  ASSERT_TRUE(directory.CreateUniqueTempDir());
  const base::FilePath path = directory.GetPath().AppendASCII("update.zip");
  ASSERT_TRUE(base::WriteFile(path, "contents"));
  const std::string digest =
      base::HexEncodeLower(crypto::hash::Sha256("contents"));

  EXPECT_TRUE(FileMatchesSha256(path, digest));
  EXPECT_FALSE(FileMatchesSha256(
      path,
      "0000000000000000000000000000000000000000000000000000000000000000"));
}

}  // namespace

}  // namespace xenon::internal
