// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_electron_safe_storage.h"

#include <memory>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/test/bind.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "build/build_config.h"
#include "components/os_crypt/async/browser/test_utils.h"
#include "components/os_crypt/async/common/algorithm.mojom.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace xenon::ipc {
namespace {

class NamedTestEncryptor final : public os_crypt_async::Encryptor {
 public:
  explicit NamedTestEncryptor(std::string name)
      : Encryptor(MakeKeys(name), name) {}

 private:
  ~NamedTestEncryptor() override = default;
  static KeyRing MakeKeys(const std::string& name) {
    KeyRing keys;
    const std::vector<uint8_t> bytes(Key::kAES256GCMKeySize, 42);
    keys.emplace(name,
                 Key(bytes, os_crypt_async::mojom::Algorithm::kAES256GCM));
    return keys;
  }
};

class XenonElectronSafeStorageTest : public testing::Test {
 protected:
  XenonElectronSafeStorageTest()
      : storage_(base::BindRepeating(
            [](XenonElectronSafeStorageTest* self,
               ElectronSafeStorage::EncryptorReady ready) {
              ++self->load_count_;
              self->ready_ = std::move(ready);
            },
            base::Unretained(this))) {}

  ElectronSafeStorage::Result Call(const std::string& method,
                                   const std::string& owner = "app:1") {
    return storage_.Call(owner,
                         base::Value(base::DictValue().Set("method", method)));
  }
  ElectronSafeStorage::Result Encrypt(const std::string& text,
                                      const std::string& owner = "app:1") {
    return storage_.Call(owner, base::Value(base::DictValue()
                                                .Set("method", "encryptString")
                                                .Set("text", text)));
  }
  ElectronSafeStorage::Result Decrypt(const std::string& ciphertext) {
    return storage_.Call("app:1",
                         base::Value(base::DictValue()
                                         .Set("method", "decryptString")
                                         .Set("ciphertext", ciphertext)));
  }
  void Ready(scoped_refptr<os_crypt_async::Encryptor> encryptor =
                 os_crypt_async::GetTestEncryptorForTesting()) {
    storage_.Initialize();
    ASSERT_TRUE(ready_);
    std::move(ready_).Run(std::move(encryptor));
  }

  base::test::TaskEnvironment environment_;
  int load_count_ = 0;
  ElectronSafeStorage::EncryptorReady ready_;
  ElectronSafeStorage storage_;
};

TEST_F(XenonElectronSafeStorageTest,
       PendingInitializationNeverClaimsAvailability) {
  EXPECT_FALSE(Call("isEncryptionAvailable")->GetBool());
  EXPECT_FALSE(Call("isEncryptionAvailable")->GetBool());
  EXPECT_EQ(load_count_, 1);
  auto pending = Encrypt("secret");
  ASSERT_FALSE(pending.has_value());
  EXPECT_TRUE(pending.error().starts_with("ERR_NOT_READY:"));
  Ready();
  EXPECT_TRUE(Call("isEncryptionAvailable")->GetBool());
}

TEST_F(XenonElectronSafeStorageTest,
       RealEncryptorRoundTripsUnicodeAndRejectsCorruption) {
  Ready();
  const std::string plaintext("Unicode \xe4\xb8\xad\xe6\x96\x87\0tail", 19);
  auto first = Encrypt(plaintext);
  auto second = Encrypt(plaintext);
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_NE(first->GetString(), second->GetString());
  auto recovered = Decrypt(first->GetString());
  ASSERT_TRUE(recovered.has_value());
  EXPECT_EQ(recovered->GetString(), plaintext);
  std::string corrupted = first->GetString();
  corrupted[corrupted.size() / 2] =
      corrupted[corrupted.size() / 2] == 'A' ? 'B' : 'A';
  EXPECT_FALSE(Decrypt(corrupted).has_value());
  EXPECT_FALSE(Decrypt("not base64!").has_value());
  EXPECT_EQ(Decrypt(Encrypt("")->GetString())->GetString(), "");
}

TEST_F(XenonElectronSafeStorageTest,
       NoKeysDoesNotSucceedEvenForEmptyPlaintext) {
  Ready(os_crypt_async::GetTestEncryptorWithoutKeysForTesting());
  EXPECT_FALSE(Call("isEncryptionAvailable")->GetBool());
  EXPECT_FALSE(Encrypt("").has_value());
  EXPECT_FALSE(Decrypt("").has_value());
}

TEST_F(XenonElectronSafeStorageTest, DifferentHostKeyCannotDecryptCiphertext) {
  Ready();
  auto encrypted = Encrypt("host-scoped test secret");
  ASSERT_TRUE(encrypted.has_value());
  ElectronSafeStorage other(
      base::BindRepeating([](ElectronSafeStorage::EncryptorReady ready) {
        std::move(ready).Run(os_crypt_async::GetTestEncryptorForTesting());
      }));
  auto decrypted = other.Call(
      "app:1", base::Value(base::DictValue()
                               .Set("method", "decryptString")
                               .Set("ciphertext", encrypted->GetString())));
  ASSERT_FALSE(decrypted.has_value());
  EXPECT_TRUE(decrypted.error().starts_with("ERR_DECRYPTION_FAILED:"));
}

TEST_F(XenonElectronSafeStorageTest, DestructionCancelsPendingRequests) {
  ElectronSafeStorage::EncryptorReady delayed;
  auto transient = std::make_unique<ElectronSafeStorage>(base::BindRepeating(
      [](ElectronSafeStorage::EncryptorReady* delayed,
         ElectronSafeStorage::EncryptorReady ready) {
        *delayed = std::move(ready);
      },
      &delayed));
  base::test::TestFuture<ElectronSafeStorage::Result> pending;
  transient->CallAsync("app:1",
                       base::Value(base::DictValue().Set(
                           "method", "isAsyncEncryptionAvailable")),
                       pending.GetCallback());
  EXPECT_FALSE(pending.IsReady());
  transient.reset();
  auto result = pending.Take();
  ASSERT_FALSE(result.has_value());
  EXPECT_TRUE(result.error().starts_with("ERR_ABORTED:"));
  // The late keychain response is bound through a WeakPtr and cannot revive
  // the destroyed broker or deliver the promise a second time.
  std::move(delayed).Run(os_crypt_async::GetTestEncryptorForTesting());
}

TEST_F(XenonElectronSafeStorageTest, AsyncOperationsWaitForTheActualEncryptor) {
  base::test::TestFuture<ElectronSafeStorage::Result> available, encrypted;
  storage_.CallAsync("app:1",
                     base::Value(base::DictValue().Set(
                         "method", "isAsyncEncryptionAvailable")),
                     available.GetCallback());
  storage_.CallAsync("app:1",
                     base::Value(base::DictValue()
                                     .Set("method", "encryptStringAsync")
                                     .Set("text", "secret")),
                     encrypted.GetCallback());
  EXPECT_FALSE(available.IsReady());
  EXPECT_FALSE(encrypted.IsReady());
  Ready();
  EXPECT_TRUE(available.Get()->GetBool());
  auto ciphertext = encrypted.Take();
  ASSERT_TRUE(ciphertext.has_value());
  base::test::TestFuture<ElectronSafeStorage::Result> decrypted;
  storage_.CallAsync(
      "app:1",
      base::Value(base::DictValue()
                      .Set("method", "decryptStringAsync")
                      .Set("ciphertext", ciphertext->GetString())),
      decrypted.GetCallback());
  auto result = decrypted.Take();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result->GetDict().FindString("result"), "secret");
  EXPECT_EQ(result->GetDict().FindBool("shouldReEncrypt"), false);
}

TEST_F(XenonElectronSafeStorageTest,
       ExitingOwnerCancelsOnlyItsPendingGeneration) {
  base::test::TestFuture<ElectronSafeStorage::Result> old_owner, new_owner;
  storage_.CallAsync("app:1",
                     base::Value(base::DictValue().Set(
                         "method", "isAsyncEncryptionAvailable")),
                     old_owner.GetCallback());
  storage_.CallAsync("app:2",
                     base::Value(base::DictValue().Set(
                         "method", "isAsyncEncryptionAvailable")),
                     new_owner.GetCallback());
  storage_.ReleaseOwner("app:1");
  auto cancelled = old_owner.Take();
  ASSERT_FALSE(cancelled.has_value());
  EXPECT_TRUE(cancelled.error().starts_with("ERR_ABORTED:"));
  EXPECT_FALSE(new_owner.IsReady());
  Ready();
  EXPECT_TRUE(new_owner.Get()->GetBool());
}

TEST_F(XenonElectronSafeStorageTest, ReplyCanRetireAnotherPendingOwner) {
  base::test::TestFuture<ElectronSafeStorage::Result> retired;
  storage_.CallAsync(
      "app:1",
      base::Value(
          base::DictValue().Set("method", "isAsyncEncryptionAvailable")),
      base::BindLambdaForTesting([&](ElectronSafeStorage::Result result) {
        EXPECT_TRUE(result->GetBool());
        storage_.ReleaseOwner("app:2");
      }));
  storage_.CallAsync("app:2",
                     base::Value(base::DictValue().Set(
                         "method", "isAsyncEncryptionAvailable")),
                     retired.GetCallback());
  Ready();
  EXPECT_FALSE(retired.Get().has_value());
}

TEST_F(XenonElectronSafeStorageTest,
       OnlyLinuxPosixFallbackRequiresPerOwnerOptIn) {
  Ready(base::MakeRefCounted<NamedTestEncryptor>("v10"));
#if BUILDFLAG(IS_LINUX)
  EXPECT_FALSE(Call("isEncryptionAvailable")->GetBool());
  EXPECT_EQ(Call("getSelectedStorageBackend")->GetString(), "basic_text");
  ASSERT_TRUE(storage_.Call(
      "app:1", base::Value(base::DictValue()
                               .Set("method", "setUsePlainTextEncryption")
                               .Set("allow", true))));
  EXPECT_TRUE(Call("isEncryptionAvailable")->GetBool());
  EXPECT_FALSE(Call("isEncryptionAvailable", "app:2")->GetBool());
  EXPECT_TRUE(Encrypt("secret").has_value());
  storage_.ReleaseOwner("app:1");
  EXPECT_FALSE(Call("isEncryptionAvailable")->GetBool());
#else
  // Both DPAPI and Keychain may legitimately use v10. Linux's fallback
  // classification must never downgrade these native user-key providers.
  EXPECT_TRUE(Call("isEncryptionAvailable")->GetBool());
  EXPECT_TRUE(Encrypt("secret").has_value());
  EXPECT_FALSE(Call("getSelectedStorageBackend").has_value());
#endif
}

TEST_F(XenonElectronSafeStorageTest, InvalidRequestsDoNotStartInitialization) {
  EXPECT_FALSE(storage_.Call("", base::Value()));
  EXPECT_FALSE(storage_.Call("app:1", base::Value()));
  EXPECT_FALSE(Call("invented"));
  EXPECT_FALSE(storage_.Call(
      "app:1", base::Value(base::DictValue()
                               .Set("method", "setUsePlainTextEncryption")
                               .Set("allow", "yes"))));
  EXPECT_EQ(load_count_, 0);
}

}  // namespace
}  // namespace xenon::ipc
