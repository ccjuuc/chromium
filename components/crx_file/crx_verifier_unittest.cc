// Copyright 2017 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/crx_file/crx_verifier.h"

#include <string>
#include <vector>

#include "base/base_paths.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/numerics/byte_conversions.h"
#include "base/path_service.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_view_util.h"
#include "components/crx_file/crx3.pb.h"
#include "components/crx_file/crx_file.h"
#include "crypto/hash.h"
#include "crypto/keypair.h"
#include "crypto/sign.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace {

base::FilePath TestFile(const std::string& file) {
  base::FilePath path;
  base::PathService::Get(base::DIR_SRC_TEST_DATA_ROOT, &path);
  return path.AppendASCII("components")
      .AppendASCII("test")
      .AppendASCII("data")
      .AppendASCII("crx_file")
      .AppendASCII(file);
}

constexpr char kOjjHash[] = "ojjgnpkioondelmggbekfhllhdaimnho";
constexpr char kOjjKey[] =
    "MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEA230uN7vYDEhdDlb4/"
    "+pg2pfL8p0FFzCF/O146NB3D5dPKuLbnNphn0OUzOrDzR/Z1XLVDlDyiA6xnb+qeRp7H8n7Wk/"
    "/gvVDNArZyForlVqWdaHLhl4dyZoNJwPKsggf30p/"
    "MxCbNfy2rzFujzn2nguOrJKzWvNt0BFqssrBpzOQl69blBezE2ZYGOnYW8mPgQV29ekIgOfJk2"
    "GgXoJBQQRRsjoPmUY7GDuEKudEB/"
    "CmWh3+"
    "mCsHBHFWbqtGhSN4YCAw3DYQzwdTcIVaIA8f2Uo4AZ4INKkrEPRL8o9mZDYtO2YHIQg8pMSRMa"
    "6AawBNYi9tZScnmgl5L1qE6z5oIwIDAQAB";

constexpr char kJlnHash[] = "jlnmailbicnpnbggmhfebbomaddckncf";
constexpr char kJlnKey[] =
    "MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAtYd4M8wBjlPsc/wxS1/uXKMD6GtI7/"
    "0GLxRe6UXTYtDk91u/"
    "FdJRK9jHws+"
    "FO6Yi3XcJGtMvuojiB1j4bdiYBfvRgfTD4b7krWtWM2udKPBtHI9ikAT5aom5Bda8rCPNyaqXC"
    "6Ax+KTgQpeeJglYu7TTd/"
    "AePyvlRHtCKNkcvRQLY0b6hccALqoTzyTueDX12c8Htg76syEPbz7hSIPPfq6KEGvuVSxWAejy"
    "/y6EhwAdXRLpegul9KmL94OY1G6dpycUKwyKeXOcB6Qj5iKNcOqJAaSLxoOZby4G3cI1BcQpp/"
    "3vYccJ4qouDMfaanLe8CvFlLp4VOn833aJ8PYpLQIDAQAB";

}  // namespace

namespace crx_file {

using CrxVerifierTest = ::testing::Test;

// Add a fresh publisher proof to a package with a valid developer proof. No
// publisher private key is stored in the source tree or shared between tests.
class CustomPublisherCrxVerifierTest : public CrxVerifierTest {
 protected:
  void WritePackage(bool include_developer = true,
                    bool tamper_archive = false) {
    ASSERT_TRUE(temp_dir_.CreateUniqueTempDir());
    package_path_ = temp_dir_.GetPath().AppendASCII("custom_publisher.crx3");

    std::string original;
    ASSERT_TRUE(
        base::ReadFileToString(TestFile("valid_no_publisher.crx3"), &original));
    ASSERT_GE(original.size(), 12u);
    const uint32_t header_size =
        base::U32FromLittleEndian(base::as_byte_span(original).subspan<8, 4>());
    ASSERT_LE(header_size, original.size() - 12);
    CrxFileHeader header;
    ASSERT_TRUE(header.ParseFromString(original.substr(12, header_size)));
    std::string archive = original.substr(12 + header_size);
    if (!include_developer) {
      header.clear_sha256_with_rsa();
      header.clear_sha256_with_ecdsa();
    }

    auto publisher_key = crypto::keypair::PrivateKey::GenerateEcP256();
    const auto publisher_spki = publisher_key.ToSubjectPublicKeyInfo();
    const auto publisher_hash = crypto::hash::Sha256(publisher_spki);
    accepted_publishers_ = {GetProductionPublisherKeyHash(),
                            {publisher_hash.begin(), publisher_hash.end()}};

    crypto::sign::Signer signer(crypto::sign::SignatureKind::ECDSA_SHA256,
                                publisher_key);
    signer.Update(base::as_byte_span(kSignatureContext));
    signer.Update(base::I32ToLittleEndian(header.signed_header_data().size()));
    signer.Update(base::as_byte_span(header.signed_header_data()));
    signer.Update(base::as_byte_span(archive));
    auto* proof = header.add_sha256_with_ecdsa();
    proof->set_public_key(base::as_string_view(publisher_spki));
    proof->set_signature(base::as_string_view(signer.Finish()));

    const std::string serialized_header = header.SerializeAsString();
    const auto size_bytes = base::I32ToLittleEndian(serialized_header.size());
    std::string package = original.substr(0, 8);
    package.append(base::as_string_view(size_bytes));
    package.append(serialized_header);
    package.append(archive);
    if (tamper_archive) {
      package.push_back('x');
    }
    ASSERT_TRUE(base::WriteFile(package_path_, package));
  }

  VerifierResult VerifyWithPublishers(const base::FilePath& path) {
    return Verify(path, VerifierFormat::CRX3_WITH_PUBLISHER_PROOF,
                  /*required_key_hashes=*/{}, /*required_file_hash=*/{},
                  /*public_key=*/nullptr, /*crx_id=*/nullptr,
                  /*compressed_verified_contents=*/nullptr,
                  accepted_publishers_);
  }

  base::ScopedTempDir temp_dir_;
  base::FilePath package_path_;
  std::vector<std::vector<uint8_t>> accepted_publishers_;
};

TEST_F(CustomPublisherCrxVerifierTest, AcceptsProductionOrCustomPublisher) {
  WritePackage();
  ASSERT_FALSE(HasFatalFailure());
  EXPECT_EQ(VerifierResult::OK_FULL,
            VerifyWithPublishers(TestFile("valid_publisher.crx3")));
  EXPECT_EQ(VerifierResult::OK_FULL, VerifyWithPublishers(package_path_));

  // The default Google policy remains unchanged for the same custom package.
  EXPECT_EQ(VerifierResult::ERROR_REQUIRED_PROOF_MISSING,
            Verify(package_path_, VerifierFormat::CRX3_WITH_PUBLISHER_PROOF,
                   /*required_key_hashes=*/{}, /*required_file_hash=*/{},
                   /*public_key=*/nullptr, /*crx_id=*/nullptr,
                   /*compressed_verified_contents=*/nullptr));
}

TEST_F(CustomPublisherCrxVerifierTest, RejectsMissingAndTestPublisher) {
  WritePackage();
  ASSERT_FALSE(HasFatalFailure());
  EXPECT_EQ(VerifierResult::ERROR_REQUIRED_PROOF_MISSING,
            VerifyWithPublishers(TestFile("valid_no_publisher.crx3")));
  EXPECT_EQ(VerifierResult::ERROR_REQUIRED_PROOF_MISSING,
            VerifyWithPublishers(TestFile("valid_test_publisher.crx3")));

  // An explicit publisher set also requires a publisher proof in plain CRX3.
  EXPECT_EQ(
      VerifierResult::ERROR_REQUIRED_PROOF_MISSING,
      Verify(TestFile("valid_no_publisher.crx3"), VerifierFormat::CRX3,
             /*required_key_hashes=*/{}, /*required_file_hash=*/{},
             /*public_key=*/nullptr, /*crx_id=*/nullptr,
             /*compressed_verified_contents=*/nullptr, accepted_publishers_));
}

TEST_F(CustomPublisherCrxVerifierTest, StillRequiresDeveloperProof) {
  WritePackage(/*include_developer=*/false);
  ASSERT_FALSE(HasFatalFailure());
  EXPECT_EQ(VerifierResult::ERROR_REQUIRED_PROOF_MISSING,
            VerifyWithPublishers(package_path_));
}

TEST_F(CustomPublisherCrxVerifierTest, RejectsTamperedArchive) {
  WritePackage(/*include_developer=*/true, /*tamper_archive=*/true);
  ASSERT_FALSE(HasFatalFailure());
  EXPECT_EQ(VerifierResult::ERROR_SIGNATURE_VERIFICATION_FAILED,
            VerifyWithPublishers(package_path_));
}

TEST_F(CustomPublisherCrxVerifierTest, PublisherSetDoesNotRelaxRequiredKeys) {
  WritePackage();
  ASSERT_FALSE(HasFatalFailure());
  const std::vector<std::vector<uint8_t>> required_keys = {
      std::vector<uint8_t>(crypto::hash::kSha256Size, 0)};
  EXPECT_EQ(
      VerifierResult::ERROR_REQUIRED_PROOF_MISSING,
      Verify(package_path_, VerifierFormat::CRX3_WITH_PUBLISHER_PROOF,
             required_keys, /*required_file_hash=*/{},
             /*public_key=*/nullptr, /*crx_id=*/nullptr,
             /*compressed_verified_contents=*/nullptr, accepted_publishers_));
}

TEST_F(CrxVerifierTest, ValidFullCrx3) {
  const std::vector<std::vector<uint8_t>> keys;
  const std::vector<uint8_t> hash;
  std::string public_key = "UNSET";
  std::string crx_id = "UNSET";

  EXPECT_EQ(VerifierResult::OK_FULL,
            Verify(TestFile("valid_no_publisher.crx3"), VerifierFormat::CRX3,
                   keys, hash, &public_key, &crx_id,
                   /*compressed_verified_contents=*/nullptr));
  EXPECT_EQ(std::string(kOjjHash), crx_id);
  EXPECT_EQ(std::string(kOjjKey), public_key);

  public_key = "UNSET";
  crx_id = "UNSET";
  EXPECT_EQ(VerifierResult::OK_FULL,
            Verify(TestFile("valid_no_publisher.crx3"), VerifierFormat::CRX3,
                   keys, hash, &public_key, &crx_id,
                   /*compressed_verified_contents=*/nullptr));
  EXPECT_EQ(std::string(kOjjHash), crx_id);
  EXPECT_EQ(std::string(kOjjKey), public_key);
}

TEST_F(CrxVerifierTest, Crx3RejectsCrx2) {
  const std::vector<std::vector<uint8_t>> keys;
  const std::vector<uint8_t> hash;
  std::string public_key = "UNSET";
  std::string crx_id = "UNSET";

  EXPECT_EQ(
      VerifierResult::ERROR_HEADER_INVALID,
      Verify(TestFile("valid.crx2"), VerifierFormat::CRX3, keys, hash,
             &public_key, &crx_id, /*compressed_verified_contents=*/nullptr));
  EXPECT_EQ("UNSET", crx_id);
  EXPECT_EQ("UNSET", public_key);
}

TEST_F(CrxVerifierTest, VerifiesFileHash) {
  const std::vector<std::vector<uint8_t>> keys;
  std::vector<uint8_t> hash;
  EXPECT_TRUE(base::HexStringToBytes(
      "d033c510f9e4ee081ccb60ea2bf530dc2e5cb0e71085b55503c8b13b74515fe4",
      &hash));
  std::string public_key = "UNSET";
  std::string crx_id = "UNSET";

  EXPECT_EQ(VerifierResult::OK_FULL,
            Verify(TestFile("valid_no_publisher.crx3"), VerifierFormat::CRX3,
                   keys, hash, &public_key, &crx_id,
                   /*compressed_verified_contents=*/nullptr));
  EXPECT_EQ(std::string(kOjjHash), crx_id);
  EXPECT_EQ(std::string(kOjjKey), public_key);

  hash.clear();
  EXPECT_TRUE(base::HexStringToBytes(std::string(32, '0'), &hash));
  public_key = "UNSET";
  crx_id = "UNSET";
  EXPECT_EQ(VerifierResult::ERROR_EXPECTED_HASH_INVALID,
            Verify(TestFile("valid_no_publisher.crx3"), VerifierFormat::CRX3,
                   keys, hash, &public_key, &crx_id,
                   /*compressed_verified_contents=*/nullptr));
  EXPECT_EQ("UNSET", crx_id);
  EXPECT_EQ("UNSET", public_key);

  hash.clear();
  EXPECT_TRUE(base::HexStringToBytes(std::string(64, '0'), &hash));
  public_key = "UNSET";
  crx_id = "UNSET";
  EXPECT_EQ(VerifierResult::ERROR_FILE_HASH_FAILED,
            Verify(TestFile("valid_no_publisher.crx3"), VerifierFormat::CRX3,
                   keys, hash, &public_key, &crx_id,
                   /*compressed_verified_contents=*/nullptr));
  EXPECT_EQ("UNSET", crx_id);
  EXPECT_EQ("UNSET", public_key);
}

TEST_F(CrxVerifierTest, ChecksRequiredKeyHashes) {
  const std::vector<uint8_t> hash;

  std::vector<uint8_t> good_key;
  EXPECT_TRUE(base::HexStringToBytes(
      "e996dfa8eed34bc6614a57bb7308cd7e519bcc690841e1969f7cb173ef16800a",
      &good_key));
  const std::vector<std::vector<uint8_t>> good_keys = {good_key};
  std::string public_key = "UNSET";
  std::string crx_id = "UNSET";
  EXPECT_EQ(VerifierResult::OK_FULL,
            Verify(TestFile("valid_no_publisher.crx3"), VerifierFormat::CRX3,
                   good_keys, hash, &public_key, &crx_id,
                   /*compressed_verified_contents=*/nullptr));
  EXPECT_EQ(std::string(kOjjHash), crx_id);
  EXPECT_EQ(std::string(kOjjKey), public_key);

  std::vector<uint8_t> bad_key;
  EXPECT_TRUE(base::HexStringToBytes(std::string(64, '0'), &bad_key));
  const std::vector<std::vector<uint8_t>> bad_keys = {bad_key};
  public_key = "UNSET";
  crx_id = "UNSET";
  EXPECT_EQ(VerifierResult::ERROR_REQUIRED_PROOF_MISSING,
            Verify(TestFile("valid_no_publisher.crx3"), VerifierFormat::CRX3,
                   bad_keys, hash, &public_key, &crx_id,
                   /*compressed_verified_contents=*/nullptr));
  EXPECT_EQ("UNSET", crx_id);
  EXPECT_EQ("UNSET", public_key);
}

TEST_F(CrxVerifierTest, ChecksPinnedKey) {
  const std::vector<uint8_t> hash;
  const std::vector<std::vector<uint8_t>> keys;
  std::string public_key = "UNSET";
  std::string crx_id = "UNSET";
  EXPECT_EQ(
      VerifierResult::OK_FULL,
      Verify(TestFile("valid_publisher.crx3"),
             VerifierFormat::CRX3_WITH_PUBLISHER_PROOF, keys, hash, &public_key,
             &crx_id, /*compressed_verified_contents=*/nullptr));
  EXPECT_EQ(std::string(kOjjHash), crx_id);
  EXPECT_EQ(std::string(kOjjKey), public_key);

  public_key = "UNSET";
  crx_id = "UNSET";
  EXPECT_EQ(
      VerifierResult::ERROR_REQUIRED_PROOF_MISSING,
      Verify(TestFile("valid_test_publisher.crx3"),
             VerifierFormat::CRX3_WITH_PUBLISHER_PROOF, keys, hash, &public_key,
             &crx_id, /*compressed_verified_contents=*/nullptr));
  EXPECT_EQ("UNSET", crx_id);
  EXPECT_EQ("UNSET", public_key);

  public_key = "UNSET";
  crx_id = "UNSET";
  EXPECT_EQ(
      VerifierResult::ERROR_REQUIRED_PROOF_MISSING,
      Verify(TestFile("valid_no_publisher.crx3"),
             VerifierFormat::CRX3_WITH_PUBLISHER_PROOF, keys, hash, &public_key,
             &crx_id, /*compressed_verified_contents=*/nullptr));
  EXPECT_EQ("UNSET", crx_id);
  EXPECT_EQ("UNSET", public_key);
}

TEST_F(CrxVerifierTest, ChecksPinnedKeyAcceptsTest) {
  const std::vector<uint8_t> hash;
  const std::vector<std::vector<uint8_t>> keys;
  std::string public_key = "UNSET";
  std::string crx_id = "UNSET";
  EXPECT_EQ(
      VerifierResult::OK_FULL,
      Verify(TestFile("valid_publisher.crx3"),
             VerifierFormat::CRX3_WITH_TEST_PUBLISHER_PROOF, keys, hash,
             &public_key, &crx_id, /*compressed_verified_contents=*/nullptr));
  EXPECT_EQ(std::string(kOjjHash), crx_id);
  EXPECT_EQ(std::string(kOjjKey), public_key);

  public_key = "UNSET";
  crx_id = "UNSET";
  EXPECT_EQ(
      VerifierResult::OK_FULL,
      Verify(TestFile("valid_test_publisher.crx3"),
             VerifierFormat::CRX3_WITH_TEST_PUBLISHER_PROOF, keys, hash,
             &public_key, &crx_id, /*compressed_verified_contents=*/nullptr));
  EXPECT_EQ(std::string(kJlnHash), crx_id);
  EXPECT_EQ(std::string(kJlnKey), public_key);

  public_key = "UNSET";
  crx_id = "UNSET";
  EXPECT_EQ(
      VerifierResult::ERROR_REQUIRED_PROOF_MISSING,
      Verify(TestFile("valid_no_publisher.crx3"),
             VerifierFormat::CRX3_WITH_TEST_PUBLISHER_PROOF, keys, hash,
             &public_key, &crx_id, /*compressed_verified_contents=*/nullptr));
  EXPECT_EQ("UNSET", crx_id);
  EXPECT_EQ("UNSET", public_key);
}

TEST_F(CrxVerifierTest, NullptrSafe) {
  const std::vector<uint8_t> hash;
  const std::vector<std::vector<uint8_t>> keys;
  EXPECT_EQ(VerifierResult::OK_FULL,
            Verify(TestFile("valid_publisher.crx3"),
                   VerifierFormat::CRX3_WITH_PUBLISHER_PROOF, keys, hash,
                   nullptr, nullptr, /*compressed_verified_contents=*/nullptr));
}

TEST_F(CrxVerifierTest, RequiresDeveloperKey) {
  const std::vector<uint8_t> hash;
  const std::vector<std::vector<uint8_t>> keys;
  std::string public_key = "UNSET";
  std::string crx_id = "UNSET";
  EXPECT_EQ(
      VerifierResult::ERROR_REQUIRED_PROOF_MISSING,
      Verify(TestFile("unsigned.crx3"), VerifierFormat::CRX3, keys, hash,
             &public_key, &crx_id, /*compressed_verified_contents=*/nullptr));
  EXPECT_EQ("UNSET", crx_id);
  EXPECT_EQ("UNSET", public_key);
}

// Verifies that `compressed_verified_contents` is not set when verified
// contents are not present in the header of the CRX.
TEST_F(CrxVerifierTest, ChecksCompressedVerifiedContentsEmpty) {
  const std::vector<uint8_t> hash;
  const std::vector<std::vector<uint8_t>> keys;
  std::string public_key = "UNSET";
  std::string crx_id = "UNSET";
  std::vector<uint8_t> compressed_verified_contents;
  EXPECT_EQ(VerifierResult::OK_FULL,
            Verify(TestFile("valid_test_publisher.crx3"),
                   VerifierFormat::CRX3_WITH_TEST_PUBLISHER_PROOF, keys, hash,
                   &public_key, &crx_id, &compressed_verified_contents));
  EXPECT_EQ(std::string(kJlnHash), crx_id);
  EXPECT_EQ(std::string(kJlnKey), public_key);
  EXPECT_TRUE(compressed_verified_contents.empty());
}

// Tests that we properly reject a crx file that includes an EOCD64 Record
// token. Regression test for https://crbug.com/538715523.
TEST_F(CrxVerifierTest, RejectsEocd64RecordInHeader) {
  const std::vector<std::vector<uint8_t>> keys;
  const std::vector<uint8_t> hash;
  std::string public_key = "UNSET";
  std::string crx_id = "UNSET";

  std::string crx_contents;
  ASSERT_TRUE(base::ReadFileToString(TestFile("valid_no_publisher.crx3"),
                                     &crx_contents));

  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());

  // Insert PK\x06\x06 (Zip64 EOCD Record) into header_bytes (starts at offset
  // 12).
  std::string crx_with_eocd64_record = crx_contents;
  ASSERT_GT(crx_with_eocd64_record.size(), 20u);
  crx_with_eocd64_record[12] = 'P';
  crx_with_eocd64_record[13] = 'K';
  crx_with_eocd64_record[14] = 0x06;
  crx_with_eocd64_record[15] = 0x06;

  base::FilePath test_file =
      temp_dir.GetPath().AppendASCII("eocd64record.crx3");
  ASSERT_TRUE(base::WriteFile(test_file, crx_with_eocd64_record));

  EXPECT_EQ(VerifierResult::ERROR_HEADER_INVALID,
            Verify(test_file, VerifierFormat::CRX3, keys, hash, &public_key,
                   &crx_id, /*compressed_verified_contents=*/nullptr));
  EXPECT_EQ("UNSET", crx_id);
  EXPECT_EQ("UNSET", public_key);
}

}  // namespace crx_file
