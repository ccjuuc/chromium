// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_electron_cookie_encryption.h"

#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/task/sequenced_task_runner.h"
#include "base/test/bind.h"
#include "base/test/test_future.h"
#include "build/build_config.h"
#include "components/os_crypt/async/browser/key_provider.h"
#include "components/os_crypt/async/browser/os_crypt_async.h"
#include "components/os_crypt/async/common/algorithm.mojom.h"
#include "components/os_crypt/async/common/encryptor.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/pref_service.h"
#include "content/public/test/browser_task_environment.h"
#include "content/public/test/test_browser_context.h"
#include "crypto/random.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "services/network/public/mojom/cookie_encryption_provider.mojom.h"
#include "services/network/public/mojom/network_context.mojom.h"
#include "sql/database.h"
#include "sql/statement.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace xenon::ipc {
namespace {

constexpr char kTestKey[] = "encryption.test_key";

class TestKeyProvider final : public os_crypt_async::KeyProvider {
 public:
  explicit TestKeyProvider(std::vector<uint8_t> key) : key_(std::move(key)) {}

  void GetKey(KeyCallback callback) override {
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(
            std::move(callback), "test",
            os_crypt_async::Encryptor::Key(
                key_, os_crypt_async::mojom::Algorithm::kAES256GCM)));
  }
  bool UseForEncryption() override { return true; }

 private:
  const std::vector<uint8_t> key_;
};

void RegisterTestPrefs(PrefRegistrySimple* registry) {
  registry->RegisterStringPref(kTestKey, "");
}

std::unique_ptr<os_crypt_async::OSCryptAsync> CreateTestProviders(
    PrefService* prefs) {
  if (!prefs->HasPrefPath(kTestKey)) {
    prefs->SetString(kTestKey,
                     base::Base64Encode(crypto::RandBytesAsVector(32)));
  }
  auto key = base::Base64Decode(prefs->GetString(kTestKey));
  if (!key || key->size() != 32) {
    return nullptr;
  }
  std::vector<std::pair<size_t, std::unique_ptr<os_crypt_async::KeyProvider>>>
      providers;
  providers.emplace_back(10u,
                         std::make_unique<TestKeyProvider>(std::move(*key)));
  return std::make_unique<os_crypt_async::OSCryptAsync>(std::move(providers));
}

using Created =
    base::test::TestFuture<std::unique_ptr<ElectronCookieEncryption>,
                           std::string>;

class ElectronCookieEncryptionTest : public testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(root_.CreateUniqueTempDir()); }

  base::FilePath Path(const std::string& name = "app") const {
    return root_.GetPath().AppendASCII(name);
  }
  base::FilePath StatePath(const std::string& name = "app") const {
    return Path(name).AppendASCII("Local State");
  }
  void Start(const base::FilePath& path, Created* future) {
    ElectronCookieEncryption::CreateForTesting(
        path, base::BindOnce(&RegisterTestPrefs),
        base::BindOnce(&CreateTestProviders), future->GetCallback());
  }
  std::unique_ptr<ElectronCookieEncryption> Create(
      const std::string& name = "app") {
    EXPECT_TRUE(base::CreateDirectory(Path(name)));
    Created future;
    Start(Path(name), &future);
    auto [encryption, error] = future.Take();
    EXPECT_TRUE(error.empty()) << error;
    EXPECT_TRUE(encryption);
    return std::move(encryption);
  }
  scoped_refptr<os_crypt_async::Encryptor> GetEncryptor(
      ElectronCookieEncryption* encryption) {
    mojo::Remote<network::mojom::CookieEncryptionProvider> remote(
        encryption->BindNewRemote());
    base::test::TestFuture<scoped_refptr<os_crypt_async::Encryptor>> future;
    remote->GetEncryptor(future.GetCallback());
    return future.Take();
  }
  std::string Read(const base::FilePath& file) {
    std::string data;
    EXPECT_TRUE(base::ReadFileToString(file, &data));
    return data;
  }
  void WriteCookie(const base::FilePath& partition,
                   base::span<const uint8_t> encrypted) {
    const auto dir = partition.AppendASCII("Network");
    ASSERT_TRUE(base::CreateDirectory(dir));
    sql::Database db("Cookie");
    ASSERT_TRUE(db.Open(dir.AppendASCII("Cookies")));
    ASSERT_TRUE(db.Execute("CREATE TABLE cookies(encrypted_value BLOB)"));
    sql::Statement insert(
        db.GetUniqueStatement("INSERT INTO cookies VALUES (?)"));
    insert.BindBlob(0, encrypted);
    ASSERT_TRUE(insert.Run());
  }
  void ExpectFailurePreservesState(const std::string& original) {
    ASSERT_TRUE(base::CreateDirectory(Path()));
    ASSERT_TRUE(base::WriteFile(StatePath(), original));
    Created future;
    Start(Path(), &future);
    EXPECT_FALSE(future.Get<0>());
    EXPECT_FALSE(future.Get<1>().empty());
    EXPECT_EQ(Read(StatePath()), original);
    EXPECT_FALSE(base::PathExists(StatePath().AddExtensionASCII("bad")));
  }

  content::BrowserTaskEnvironment environment_;
  base::ScopedTempDir root_;
};

TEST_F(ElectronCookieEncryptionTest,
       KeyIsPersistedBeforePublicationAndSurvivesIndependentCreation) {
  auto first = Create();
  ASSERT_TRUE(first);
  ASSERT_TRUE(base::PathExists(StatePath()));
  const std::string saved = Read(StatePath());
  auto encrypted =
      GetEncryptor(first.get())->EncryptString("persistent cookie");
  ASSERT_TRUE(encrypted);
  WriteCookie(Path(), *encrypted);
  first.reset();

  auto next = Create();
  ASSERT_TRUE(next);
  EXPECT_EQ(GetEncryptor(next.get())->DecryptData(*encrypted),
            "persistent cookie");
  EXPECT_EQ(Read(StatePath()), saved);

  auto unrelated = Create("another-app");
  ASSERT_TRUE(unrelated);
  EXPECT_FALSE(GetEncryptor(unrelated.get())->DecryptData(*encrypted));
}

TEST_F(ElectronCookieEncryptionTest, MissingStateDoesNotReplaceExistingKey) {
  auto original = Create();
  auto encrypted = GetEncryptor(original.get())->EncryptString("persistent");
  ASSERT_TRUE(encrypted);
  WriteCookie(Path(), *encrypted);
  original.reset();
  ASSERT_TRUE(base::DeleteFile(StatePath()));
  const auto cookie_path = Path().AppendASCII("Network").AppendASCII("Cookies");
  const std::string cookie_bytes = Read(cookie_path);

  Created future;
  Start(Path(), &future);
  EXPECT_FALSE(future.Get<0>());
  EXPECT_NE(future.Get<1>().find("cannot be decrypted"), std::string::npos);
  EXPECT_FALSE(base::PathExists(StatePath()));
  EXPECT_EQ(Read(cookie_path), cookie_bytes);
}

TEST_F(ElectronCookieEncryptionTest,
       MissingKeyChecksNamedPersistentPartitions) {
  auto original = Create();
  auto encrypted = GetEncryptor(original.get())->EncryptString("partition");
  ASSERT_TRUE(encrypted);
  WriteCookie(Path()
                  .AppendASCII("Storage")
                  .AppendASCII("ext")
                  .AppendASCII("generic-domain")
                  .AppendASCII("generic-partition"),
              *encrypted);
  original.reset();
  ExpectFailurePreservesState("{\"unrelated_setting\":42}");
}

TEST_F(ElectronCookieEncryptionTest, WrongButWellFormedKeyDoesNotLoseCookies) {
  auto original = Create();
  auto encrypted = GetEncryptor(original.get())->EncryptString("original");
  ASSERT_TRUE(encrypted);
  WriteCookie(Path(), *encrypted);
  original.reset();
  auto unrelated = Create("unrelated");
  unrelated.reset();
  ExpectFailurePreservesState(Read(StatePath("unrelated")));
}

TEST_F(ElectronCookieEncryptionTest, CorruptJsonIsNotRenamedOrReset) {
  ExpectFailurePreservesState("{\"encryption\": truncated");
}

TEST_F(ElectronCookieEncryptionTest, CorruptPreferenceAncestorsAreNotReset) {
  ExpectFailurePreservesState("{\"encryption\":\"not a dictionary\"}");
}

TEST_F(ElectronCookieEncryptionTest, CorruptPreferenceTypesAreNotReset) {
  ExpectFailurePreservesState("{\"encryption\":{\"test_key\":12}}");
}

TEST_F(ElectronCookieEncryptionTest, UnreadableExistingKeyIsNotReset) {
  ExpectFailurePreservesState("{\"encryption\":{\"test_key\":\"bad key\"}}");
}

TEST_F(ElectronCookieEncryptionTest, UnsupportedCiphertextIsPreserved) {
  ASSERT_TRUE(base::CreateDirectory(Path()));
  const std::string unsupported = "v20fixture requiring another provider";
  WriteCookie(Path(), base::as_byte_span(unsupported));
  Created future;
  Start(Path(), &future);
  EXPECT_FALSE(future.Get<0>());
  EXPECT_FALSE(future.Get<1>().empty());
  EXPECT_FALSE(base::PathExists(StatePath()));
}

TEST_F(ElectronCookieEncryptionTest, UnavailableKeychainDoesNotPublishState) {
  ASSERT_TRUE(base::CreateDirectory(Path()));
  Created future;
  ElectronCookieEncryption::CreateForTesting(
      Path(), base::BindOnce(&RegisterTestPrefs),
      base::BindOnce([](PrefService*) {
        return std::make_unique<os_crypt_async::OSCryptAsync>(
            std::vector<std::pair<
                size_t, std::unique_ptr<os_crypt_async::KeyProvider>>>());
      }),
      future.GetCallback());
  EXPECT_FALSE(future.Get<0>());
  EXPECT_FALSE(future.Get<1>().empty());
  EXPECT_FALSE(base::PathExists(StatePath()));
}

TEST_F(ElectronCookieEncryptionTest, FailedCommitDoesNotPublishProvider) {
  ASSERT_TRUE(base::CreateDirectory(Path()));
  Created future;
  ElectronCookieEncryption::CreateForTesting(
      Path(), base::BindOnce(&RegisterTestPrefs),
      base::BindLambdaForTesting([this](PrefService* prefs) {
        EXPECT_TRUE(base::CreateDirectory(StatePath()));
        return CreateTestProviders(prefs);
      }),
      future.GetCallback());
  EXPECT_FALSE(future.Get<0>());
  EXPECT_NE(future.Get<1>().find("could not be persisted"), std::string::npos);
}

TEST_F(ElectronCookieEncryptionTest,
       PreservesUnrelatedStateAndDoesNotRecursivelyScanCacheFiles) {
  ASSERT_TRUE(base::CreateDirectory(Path().AppendASCII("Cache")));
  ASSERT_TRUE(
      base::WriteFile(StatePath(), "{\"unrelated\":{\"enabled\":true}}"));
  ASSERT_TRUE(base::WriteFile(
      Path().AppendASCII("Cache").AppendASCII("Cookies"), "not a database"));
  auto encryption = Create();
  ASSERT_TRUE(encryption);
  auto saved = base::JSONReader::ReadDict(Read(StatePath()),
                                          base::JSON_PARSE_CHROMIUM_EXTENSIONS);
  ASSERT_TRUE(saved);
  EXPECT_EQ(saved->FindBoolByDottedPath("unrelated.enabled"), true);
}

TEST_F(ElectronCookieEncryptionTest, OnlyInstalledContextsUseIndependentState) {
  auto encryption = Create();
  ASSERT_TRUE(encryption);
  content::TestBrowserContext ordinary;
  content::TestBrowserContext application;
  auto ordinary_params = network::mojom::NetworkContextParams::New();
  EXPECT_FALSE(ElectronCookieEncryption::ConfigureNetworkContext(
      &ordinary, ordinary_params.get()));
  EXPECT_FALSE(ordinary_params->cookie_encryption_provider);

  ElectronCookieEncryption::Install(&application, std::move(encryption));
  auto first = network::mojom::NetworkContextParams::New();
  auto second = network::mojom::NetworkContextParams::New();
  ASSERT_TRUE(ElectronCookieEncryption::ConfigureNetworkContext(&application,
                                                                first.get()));
  ASSERT_TRUE(ElectronCookieEncryption::ConfigureNetworkContext(&application,
                                                                second.get()));
  mojo::Remote<network::mojom::CookieEncryptionProvider> first_remote(
      std::move(first->cookie_encryption_provider));
  mojo::Remote<network::mojom::CookieEncryptionProvider> second_remote(
      std::move(second->cookie_encryption_provider));
  base::test::TestFuture<scoped_refptr<os_crypt_async::Encryptor>>
      first_encryptor;
  base::test::TestFuture<scoped_refptr<os_crypt_async::Encryptor>>
      second_encryptor;
  first_remote->GetEncryptor(first_encryptor.GetCallback());
  second_remote->GetEncryptor(second_encryptor.GetCallback());
  auto encrypted = first_encryptor.Get()->EncryptString("shared session key");
  ASSERT_TRUE(encrypted);
  EXPECT_EQ(second_encryptor.Get()->DecryptData(*encrypted),
            "shared session key");
}

#if BUILDFLAG(IS_WIN)
TEST_F(ElectronCookieEncryptionTest, NativeDpapiStateSurvivesRecreation) {
  ASSERT_TRUE(base::CreateDirectory(Path()));
  Created initial;
  ElectronCookieEncryption::Create(Path(), initial.GetCallback());
  auto [first, error] = initial.Take();
  ASSERT_TRUE(first) << error;
  auto encrypted = GetEncryptor(first.get())->EncryptString("native DPAPI");
  ASSERT_TRUE(encrypted);
  WriteCookie(Path(), *encrypted);
  const std::string saved = Read(StatePath());
  first.reset();

  Created later;
  ElectronCookieEncryption::Create(Path(), later.GetCallback());
  auto [next, later_error] = later.Take();
  ASSERT_TRUE(next) << later_error;
  EXPECT_EQ(GetEncryptor(next.get())->DecryptData(*encrypted), "native DPAPI");
  EXPECT_EQ(Read(StatePath()), saved);
}

TEST_F(ElectronCookieEncryptionTest, NativeDpapiCorruptionIsNotReset) {
  ASSERT_TRUE(base::CreateDirectory(Path()));
  const std::string saved =
      "{\"os_crypt\":{\"encrypted_key\":\"invalid encrypted key\"}}";
  ASSERT_TRUE(base::WriteFile(StatePath(), saved));
  Created future;
  ElectronCookieEncryption::Create(Path(), future.GetCallback());
  EXPECT_FALSE(future.Get<0>());
  EXPECT_FALSE(future.Get<1>().empty());
  EXPECT_EQ(Read(StatePath()), saved);
}
#endif

}  // namespace
}  // namespace xenon::ipc
