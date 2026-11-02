// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// An observational microbenchmark, not a performance acceptance test. Run the
// same optimized binary on an otherwise idle machine when comparing results.
// Setup, buffer allocation, nonce construction and output verification are
// outside measured batches. Allocations performed inside a cryptographic API
// remain part of that operation's cost, as they are for its callers.

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <string_view>
#include <vector>

#include "third_party/boringssl/src/include/openssl/aead.h"
#include "third_party/boringssl/src/include/openssl/ec_key.h"
#include "third_party/boringssl/src/include/openssl/err.h"
#include "third_party/boringssl/src/include/openssl/mem.h"
#include "third_party/boringssl/src/include/openssl/nid.h"
#include "third_party/boringssl/src/include/openssl/rand.h"
#include "third_party/boringssl/src/include/openssl/sha.h"
#include "third_party/boringssl/src/include/openssl/sm2.h"
#include "third_party/boringssl/src/include/openssl/sm3.h"

namespace {

using Clock = std::chrono::steady_clock;
constexpr size_t kRecordSize = 16 * 1024;
constexpr size_t kBatchSize = 64;
constexpr size_t kSignBatchSize = 8;
constexpr double kWarmupSeconds = 0.1;
constexpr double kMeasurementSeconds = 0.35;

void Require(bool ok, std::string_view operation) {
  if (!ok) {
    std::cerr << "Crypto benchmark failed: " << operation << '\n';
    ERR_print_errors_fp(stderr);
    std::exit(1);
  }
}

double Elapsed(Clock::time_point start) {
  return std::chrono::duration<double>(Clock::now() - start).count();
}

struct Result {
  std::string_view name;
  uint64_t operations;
  double seconds;
  size_t bytes_per_operation;
};

// Each batch returns only the duration of the cryptographic calls; its setup
// and validation execute before/after the two clock reads.
template <typename Batch>
Result Measure(std::string_view name,
               size_t bytes_per_operation,
               size_t batch_size,
               Batch batch) {
  double warmup = 0;
  while (warmup < kWarmupSeconds) {
    warmup += batch();
  }
  Result result{name, 0, 0, bytes_per_operation};
  while (result.seconds < kMeasurementSeconds) {
    result.seconds += batch();
    result.operations += batch_size;
  }
  return result;
}

std::array<uint8_t, kRecordSize> MakePlaintext() {
  std::array<uint8_t, kRecordSize> data;
  for (size_t i = 0; i < data.size(); ++i) {
    data[i] = static_cast<uint8_t>(13 * i + 7);
  }
  return data;
}

struct Record {
  std::array<uint8_t, 12> nonce = {};
  std::array<uint8_t, kRecordSize + 16> ciphertext;
  std::array<uint8_t, kRecordSize> opened;
  size_t ciphertext_size = 0;
  size_t opened_size = 0;
};

class AeadBenchmark {
 public:
  AeadBenchmark(const EVP_AEAD* algorithm,
                const std::array<uint8_t, kRecordSize>& plaintext)
      : plaintext_(plaintext), records_(kBatchSize) {
    std::array<uint8_t, 16> key;
    Require(RAND_bytes(key.data(), key.size()) == 1, "random AEAD key");
    context_.reset(EVP_AEAD_CTX_new(algorithm, key.data(), key.size(), 16));
    OPENSSL_cleanse(key.data(), key.size());
    Require(context_ != nullptr, "AEAD initialization");
  }

  Result Seal(std::string_view name) {
    return Measure(name, kRecordSize, kBatchSize, [this] {
      PrepareNonces();
      const auto start = Clock::now();
      for (auto& record : records_) {
        SealRecord(record);
      }
      const double seconds = Elapsed(start);
      for (auto& record : records_) {
        OpenRecord(record);
      }
      VerifyPlaintext();
      return seconds;
    });
  }

  Result Open(std::string_view name) {
    return Measure(name, kRecordSize, kBatchSize, [this] {
      PrepareNonces();
      for (auto& record : records_) {
        SealRecord(record);
      }
      const auto start = Clock::now();
      for (auto& record : records_) {
        OpenRecord(record);
      }
      const double seconds = Elapsed(start);
      VerifyPlaintext();
      return seconds;
    });
  }

 private:
  void PrepareNonces() {
    for (auto& record : records_) {
      Require(next_nonce_ != std::numeric_limits<uint64_t>::max(),
              "AEAD nonce exhaustion");
      const uint64_t sequence = next_nonce_++;
      // The upper four bytes remain zero. The counter is unique for every
      // encryption under this temporary key, including warmup and open setup.
      for (size_t i = 0; i < 8; ++i) {
        record.nonce[11 - i] = static_cast<uint8_t>(sequence >> (8 * i));
      }
    }
  }

  void SealRecord(Record& record) {
    Require(EVP_AEAD_CTX_seal(context_.get(), record.ciphertext.data(),
                              &record.ciphertext_size, record.ciphertext.size(),
                              record.nonce.data(), record.nonce.size(),
                              plaintext_.data(), plaintext_.size(), aad_.data(),
                              aad_.size()) == 1 &&
                record.ciphertext_size == record.ciphertext.size(),
            "AEAD seal");
  }

  void OpenRecord(Record& record) {
    Require(EVP_AEAD_CTX_open(context_.get(), record.opened.data(),
                              &record.opened_size, record.opened.size(),
                              record.nonce.data(), record.nonce.size(),
                              record.ciphertext.data(), record.ciphertext_size,
                              aad_.data(), aad_.size()) == 1 &&
                record.opened_size == plaintext_.size(),
            "AEAD open");
  }

  void VerifyPlaintext() {
    for (const auto& record : records_) {
      Require(CRYPTO_memcmp(record.opened.data(), plaintext_.data(),
                            plaintext_.size()) == 0,
              "AEAD plaintext mismatch");
    }
  }

  const std::array<uint8_t, kRecordSize>& plaintext_;
  std::vector<Record> records_;
  bssl::UniquePtr<EVP_AEAD_CTX> context_;
  const std::array<uint8_t, 5> aad_ = {23, 3, 3, 0x40, 0x10};
  uint64_t next_nonce_ = 0;
};

using HashFunction = uint8_t* (*)(const uint8_t*, size_t, uint8_t*);

Result MeasureHash(std::string_view name,
                   HashFunction hash,
                   const std::array<uint8_t, kRecordSize>& plaintext) {
  std::array<uint8_t, 32> reference;
  Require(hash(plaintext.data(), plaintext.size(), reference.data()) ==
              reference.data(),
          "reference hash");
  std::array<std::array<uint8_t, 32>, kBatchSize> digests;
  return Measure(name, kRecordSize, kBatchSize, [&] {
    const auto start = Clock::now();
    for (auto& digest : digests) {
      Require(hash(plaintext.data(), plaintext.size(), digest.data()) ==
                  digest.data(),
              "hash");
    }
    const double seconds = Elapsed(start);
    for (const auto& digest : digests) {
      Require(
          CRYPTO_memcmp(reference.data(), digest.data(), digest.size()) == 0,
          "hash output mismatch");
    }
    return seconds;
  });
}

struct Signature {
  std::array<uint8_t, 72> bytes;
  size_t size = 0;
};

std::array<Result, 2> MeasureSm2(
    const std::array<uint8_t, kRecordSize>& plaintext) {
  bssl::UniquePtr<EC_KEY> key(EC_KEY_new_by_curve_name(NID_sm2));
  Require(key && EC_KEY_generate_key(key.get()), "random SM2 key");
  constexpr uint8_t kIdentity[] = "TLSv1.3+GM+Cipher+Suite";
  std::array<uint8_t, SM2_DIGEST_LENGTH> digest;
  Require(SM2_compute_digest(digest.data(), key.get(), kIdentity,
                             sizeof(kIdentity) - 1, plaintext.data(),
                             plaintext.size()) == 1,
          "SM2 identity-bound digest");
  std::array<Signature, kSignBatchSize> signatures;
  const auto sign_all = [&] {
    for (auto& signature : signatures) {
      Require(SM2_sign_digest(digest.data(), signature.bytes.data(),
                              &signature.size, signature.bytes.size(),
                              key.get()) == 1 &&
                  signature.size > 0 &&
                  signature.size <= signature.bytes.size(),
              "SM2 sign digest");
    }
  };
  const auto verify_all = [&] {
    for (const auto& signature : signatures) {
      Require(SM2_verify_digest(digest.data(), signature.bytes.data(),
                                signature.size, key.get()) == 1,
              "SM2 verify digest");
    }
  };
  Result sign = Measure("SM2-sign-digest", 0, kSignBatchSize, [&] {
    const auto start = Clock::now();
    sign_all();
    const double seconds = Elapsed(start);
    verify_all();
    return seconds;
  });
  // These signatures were generated and checked above. Reusing them for
  // verification does not repeat any encryption or signing nonce.
  Result verify = Measure("SM2-verify-digest", 0, kSignBatchSize, [&] {
    const auto start = Clock::now();
    verify_all();
    return Elapsed(start);
  });
  return {sign, verify};
}

void PrintResults(const std::array<Result, 10>& results) {
#if defined(OPENSSL_X86_64)
  constexpr std::string_view kArchitecture = "x64";
#elif defined(OPENSSL_AARCH64)
  constexpr std::string_view kArchitecture = "arm64";
#else
  constexpr std::string_view kArchitecture = "other";
#endif
  std::cout.imbue(std::locale::classic());
  std::cout << std::fixed << std::setprecision(9)
            << "{\n"
               "  \"schema_version\": 1,\n"
               "  \"architecture\": \""
            << kArchitecture
            << "\",\n"
               "  \"clock\": \"steady_clock\",\n"
               "  \"record_bytes\": "
            << kRecordSize
            << ",\n"
               "  \"aead_tag_bytes\": 16,\n"
               "  \"aead_aad_bytes\": 5,\n"
               "  \"warmup_seconds_per_item\": "
            << kWarmupSeconds << ",\n  \"minimum_measured_seconds_per_item\": "
            << kMeasurementSeconds
            << ",\n"
               "  \"setup_and_validation_timed\": false,\n"
               "  \"library_internal_allocations_timed\": true,\n"
               "  \"acceptance_threshold\": null,\n"
               "  \"results\": [\n";
  for (size_t i = 0; i < results.size(); ++i) {
    const auto& result = results[i];
    const double operations = static_cast<double>(result.operations);
    const double throughput = operations *
                              static_cast<double>(result.bytes_per_operation) /
                              result.seconds / (1024 * 1024);
    std::cout << "    {\"name\": \"" << result.name
              << "\", \"operations\": " << result.operations
              << ", \"measured_seconds\": " << result.seconds
              << ", \"mean_us\": " << result.seconds * 1000000 / operations
              << ", \"bytes_per_operation\": " << result.bytes_per_operation
              << ", \"mib_per_second\": ";
    if (result.bytes_per_operation) {
      std::cout << throughput;
    } else {
      std::cout << "null";
    }
    std::cout << "}" << (i + 1 == results.size() ? "" : ",") << '\n';
  }
  std::cout << "  ]\n}\n";
}

}  // namespace

int main() {
  const auto plaintext = MakePlaintext();
  std::array<Result, 10> results;
  size_t index = 0;
  {
    AeadBenchmark aes(EVP_aead_aes_128_gcm(), plaintext);
    results[index++] = aes.Seal("AES128-GCM-seal");
    results[index++] = aes.Open("AES128-GCM-open");
  }
  {
    AeadBenchmark sm4(EVP_aead_sm4_gcm(), plaintext);
    results[index++] = sm4.Seal("SM4-GCM-seal");
    results[index++] = sm4.Open("SM4-GCM-open");
  }
  {
    AeadBenchmark sm4(EVP_aead_sm4_ccm(), plaintext);
    results[index++] = sm4.Seal("SM4-CCM-seal");
    results[index++] = sm4.Open("SM4-CCM-open");
  }
  results[index++] = MeasureHash("SHA256", SHA256, plaintext);
  results[index++] = MeasureHash("SM3", SM3, plaintext);
  for (const auto& result : MeasureSm2(plaintext)) {
    results[index++] = result;
  }
  Require(index == results.size(), "complete result set");
  PrintResults(results);
  return 0;
}
