// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_electron_cookie_encryption.h"

#include <utility>
#include <vector>

#include "base/check.h"
#include "base/command_line.h"
#include "base/feature_list.h"
#include "base/files/file.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/files/important_file_writer.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/memory/ptr_util.h"
#include "base/strings/string_split.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "build/build_config.h"
#include "build/config/linux/dbus/buildflags.h"
#include "components/os_crypt/async/browser/os_crypt_async.h"
#include "components/os_crypt/async/common/encryptor.h"
#include "components/prefs/in_memory_pref_store.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/pref_service_factory.h"
#include "content/public/browser/browser_context.h"
#include "services/network/public/cpp/cookie_encryption_provider_impl.h"
#include "services/network/public/mojom/network_context.mojom.h"
#include "sql/database.h"
#include "sql/statement.h"

#if BUILDFLAG(IS_WIN)
#include "components/os_crypt/async/browser/dpapi_key_provider.h"
#include "components/os_crypt/async/browser/os_crypt_win.h"
#endif

#if BUILDFLAG(IS_MAC)
#include "chrome/common/chrome_features.h"
#include "components/os_crypt/async/browser/keychain_key_provider.h"
#endif

#if BUILDFLAG(IS_LINUX) && BUILDFLAG(USE_DBUS)
#include "chrome/browser/browser_features.h"
#include "chrome/grit/generated_resources.h"
#include "components/os_crypt/async/browser/freedesktop_secret_key_provider.h"
#include "components/os_crypt/async/browser/secret_portal_key_provider.h"
#include "components/password_manager/core/browser/password_manager_switches.h"
#include "ui/base/l10n/l10n_util.h"
#endif

#if BUILDFLAG(IS_POSIX) && !BUILDFLAG(IS_MAC)
#include "components/os_crypt/async/browser/posix_key_provider.h"
#endif

namespace xenon::ipc {
namespace {

const char kEncryptionDataKey = 0;
constexpr int64_t kMaxStateBytes = 16 * 1024 * 1024;

// JsonPrefStore normally recovers from corrupt JSON by renaming it and starting
// over. Encryption key material must instead remain intact for recovery. Read
// strictly, and never create a replacement key after an unreadable state file.
base::expected<base::DictValue, std::string> ReadState(
    const base::FilePath& path) {
  base::File file(path, base::File::FLAG_OPEN | base::File::FLAG_READ);
  if (!file.IsValid()) {
    if (file.error_details() == base::File::FILE_ERROR_NOT_FOUND) {
      return base::DictValue();
    }
    return base::unexpected(
        "ERR_FAILED: application encryption Local State "
        "cannot be read");
  }
  const int64_t length = file.GetLength();
  if (length < 0 || length > kMaxStateBytes) {
    return base::unexpected(
        "ERR_FAILED: application encryption Local State "
        "has an invalid size");
  }
  std::string json(static_cast<size_t>(length), '\0');
  if (!file.ReadAndCheck(0, base::as_writable_byte_span(json))) {
    return base::unexpected(
        "ERR_FAILED: application encryption Local State "
        "cannot be read");
  }
  auto state =
      base::JSONReader::ReadDict(json, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
  if (!state) {
    return base::unexpected(
        "ERR_FAILED: application encryption Local State "
        "is corrupt");
  }
  return std::move(*state);
}

bool LoadRegisteredPrefs(const base::DictValue& state,
                         PrefRegistrySimple* registry,
                         InMemoryPrefStore* store) {
  for (const auto& [name, default_value] : *registry) {
    // A malformed parent dictionary must not turn an existing key into an
    // apparently missing preference and cause silent key regeneration.
    auto parts = base::SplitStringPiece(name, ".", base::KEEP_WHITESPACE,
                                        base::SPLIT_WANT_ALL);
    const base::DictValue* parent = &state;
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
      const base::Value* ancestor = parent->Find(parts[i]);
      if (!ancestor) {
        parent = nullptr;
        break;
      }
      if (!ancestor->is_dict()) {
        return false;
      }
      parent = &ancestor->GetDict();
    }
    const base::Value* value = parent ? parent->Find(parts.back()) : nullptr;
    if (value) {
      if (value->type() != default_value.type()) {
        return false;
      }
      store->SetValueSilently(name, value->Clone(), 0);
    }
  }
  return true;
}

void RegisterNativePrefs(PrefRegistrySimple* registry) {
#if BUILDFLAG(IS_WIN)
  os_crypt_async::RegisterLocalPrefs(registry);
#endif
#if BUILDFLAG(IS_LINUX) && BUILDFLAG(USE_DBUS)
  os_crypt_async::SecretPortalKeyProvider::RegisterLocalPrefs(registry);
#endif
}

std::unique_ptr<os_crypt_async::OSCryptAsync> CreateNativeProviders(
    PrefService* local_state) {
  std::vector<std::pair<size_t, std::unique_ptr<os_crypt_async::KeyProvider>>>
      providers;
#if BUILDFLAG(IS_WIN)
  const auto result = os_crypt_async::InitWithExistingKey(local_state);
  if (result != os_crypt_async::InitResult::kSuccess &&
      (result != os_crypt_async::InitResult::kKeyDoesNotExist ||
       !os_crypt_async::Init(local_state))) {
    // Init() itself regenerates a key on a DPAPI decryption failure. That is
    // unsafe here: a new key would destroy access to the application's cookies.
    return nullptr;
  }
  // App-Bound encryption requires Chrome's default browser data directory.
  // An explicitly redirected application directory uses the same DPAPI
  // provider as Chromium with a non-default --user-data-dir.
  providers.emplace_back(
      10u, std::make_unique<os_crypt_async::DPAPIKeyProvider>(local_state));
#endif
#if BUILDFLAG(IS_LINUX) && BUILDFLAG(USE_DBUS)
  const auto password_store =
      base::CommandLine::ForCurrentProcess()->GetSwitchValueASCII(
          password_manager::kPasswordStore);
  if (password_store != "basic" &&
      base::FeatureList::IsEnabled(features::kDbusSecretPortal)) {
    providers.emplace_back(
        15u, std::make_unique<os_crypt_async::SecretPortalKeyProvider>(
                 local_state,
                 base::FeatureList::IsEnabled(
                     features::kSecretPortalKeyProviderUseForEncryption)));
  }
  providers.emplace_back(
      10u,
      std::make_unique<os_crypt_async::FreedesktopSecretKeyProvider>(
          password_store, l10n_util::GetStringUTF8(IDS_PRODUCT_NAME), nullptr));
#endif
#if BUILDFLAG(IS_POSIX) && !BUILDFLAG(IS_MAC)
  providers.emplace_back(5u,
                         std::make_unique<os_crypt_async::PosixKeyProvider>());
#endif
#if BUILDFLAG(IS_MAC)
  if (base::FeatureList::IsEnabled(features::kUseKeychainKeyProvider)) {
    providers.emplace_back(
        10u, std::make_unique<os_crypt_async::KeychainKeyProvider>());
  }
#endif
  return std::make_unique<os_crypt_async::OSCryptAsync>(std::move(providers));
}

bool ValidateCookieDatabase(const base::FilePath& path,
                            os_crypt_async::Encryptor* encryptor) {
  base::File file(path, base::File::FLAG_OPEN | base::File::FLAG_READ);
  if (!file.IsValid()) {
    return file.error_details() == base::File::FILE_ERROR_NOT_FOUND;
  }
  file.Close();
  bool database_error = false;
  sql::Database db(
      sql::DatabaseOptions().set_read_only(true).set_exclusive_locking(false),
      "Cookie");
  db.set_error_callback(base::BindRepeating(
      [](bool* error, int, sql::Statement*) { *error = true; },
      &database_error));
  if (!db.Open(path)) {
    return false;
  }
  if (!db.DoesTableExist("cookies")) {
    return !database_error;
  }
  sql::Statement cookies(db.GetUniqueStatement(
      "SELECT encrypted_value FROM cookies WHERE length(encrypted_value)>0"));
  while (cookies.Step()) {
    // Validate with Chromium's own providers, without guessing cipher tags or
    // deleting entries that might be recoverable when the correct key returns.
    if (!encryptor->DecryptData(cookies.ColumnBlob(0)).has_value()) {
      return false;
    }
  }
  return cookies.Succeeded() && !database_error;
}

bool ValidatePartitionCookies(const base::FilePath& partition,
                              os_crypt_async::Encryptor* encryptor) {
  // Both paths can exist during Chromium's Network directory migration.
  return ValidateCookieDatabase(partition.AppendASCII("Cookies"), encryptor) &&
         ValidateCookieDatabase(
             partition.AppendASCII("Network").AppendASCII("Cookies"),
             encryptor);
}

bool ValidatePersistedCookies(const base::FilePath& session_path,
                              os_crypt_async::Encryptor* encryptor) {
  if (!ValidatePartitionCookies(session_path, encryptor)) {
    return false;
  }
  // Chromium's non-default persistent partitions use Storage/ext/domain/name.
  // Visit exactly those directory levels; recursively walking caches here can
  // turn a key check into an expensive scan of an application's entire profile.
  const auto root = session_path.AppendASCII("Storage").AppendASCII("ext");
  base::FileEnumerator domains(
      root, false, base::FileEnumerator::DIRECTORIES, {},
      base::FileEnumerator::FolderSearchPolicy::ALL,
      base::FileEnumerator::ErrorPolicy::STOP_ENUMERATION);
  for (auto domain = domains.Next(); !domain.empty(); domain = domains.Next()) {
    base::FileEnumerator partitions(
        domain, false, base::FileEnumerator::DIRECTORIES, {},
        base::FileEnumerator::FolderSearchPolicy::ALL,
        base::FileEnumerator::ErrorPolicy::STOP_ENUMERATION);
    for (auto partition = partitions.Next(); !partition.empty();
         partition = partitions.Next()) {
      if (!ValidatePartitionCookies(partition, encryptor)) {
        return false;
      }
    }
    if (partitions.GetError() != base::File::FILE_OK) {
      return false;
    }
  }
  return domains.GetError() == base::File::FILE_OK ||
         domains.GetError() == base::File::FILE_ERROR_NOT_FOUND;
}

std::string CommitState(const base::FilePath& path,
                        base::DictValue state,
                        scoped_refptr<os_crypt_async::Encryptor> encryptor) {
  if (!ValidatePersistedCookies(path.DirName(), encryptor.get())) {
    return "ERR_FAILED: existing application cookies cannot be decrypted; "
           "the original Local State and operating-system key are required";
  }
  auto json = base::WriteJson(state);
  if (!json || !base::ImportantFileWriter::WriteFileAtomically(
                   path, *json, "XenonAppCookieEncryption")) {
    return "ERR_FAILED: application encryption Local State could not be "
           "persisted";
  }
  return "";
}

}  // namespace

ElectronCookieEncryption::ElectronCookieEncryption(
    const base::FilePath& session_path)
    : state_path_(session_path.AppendASCII("Local State")),
      file_runner_(base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
           base::TaskShutdownBehavior::BLOCK_SHUTDOWN})),
      pref_store_(base::MakeRefCounted<InMemoryPrefStore>()) {}

ElectronCookieEncryption::~ElectronCookieEncryption() = default;

void ElectronCookieEncryption::Create(const base::FilePath& session_path,
                                      CreatedCallback callback) {
  CreateWithProviders(session_path, base::BindOnce(&RegisterNativePrefs),
                      base::BindOnce(&CreateNativeProviders),
                      std::move(callback));
}

void ElectronCookieEncryption::CreateForTesting(
    const base::FilePath& session_path,
    RegisterPrefs register_prefs,
    ProviderFactory provider_factory,
    CreatedCallback callback) {
  CreateWithProviders(session_path, std::move(register_prefs),
                      std::move(provider_factory), std::move(callback));
}

void ElectronCookieEncryption::CreateWithProviders(
    const base::FilePath& session_path,
    RegisterPrefs register_prefs,
    ProviderFactory provider_factory,
    CreatedCallback callback) {
  auto self = base::WrapUnique(new ElectronCookieEncryption(session_path));
  const auto state_path = self->state_path_;
  const auto runner = self->file_runner_;
  runner->PostTaskAndReplyWithResult(
      FROM_HERE, base::BindOnce(&ReadState, state_path),
      base::BindOnce(&OnStateRead, std::move(self), std::move(register_prefs),
                     std::move(provider_factory), std::move(callback)));
}

void ElectronCookieEncryption::OnStateRead(
    std::unique_ptr<ElectronCookieEncryption> self,
    RegisterPrefs register_prefs,
    ProviderFactory provider_factory,
    CreatedCallback callback,
    base::expected<base::DictValue, std::string> state) {
  if (!state.has_value()) {
    std::move(callback).Run(nullptr, std::move(state.error()));
    return;
  }
  self->original_state_ = std::move(*state);
  auto registry = base::MakeRefCounted<PrefRegistrySimple>();
  std::move(register_prefs).Run(registry.get());
  if (!LoadRegisteredPrefs(self->original_state_, registry.get(),
                           self->pref_store_.get())) {
    std::move(callback).Run(
        nullptr, "ERR_FAILED: application encryption preferences are corrupt");
    return;
  }
  PrefServiceFactory factory;
  factory.set_user_prefs(self->pref_store_);
  self->local_state_ = factory.Create(registry);
  self->os_crypt_ = std::move(provider_factory).Run(self->local_state_.get());
  if (!self->os_crypt_) {
    std::move(callback).Run(
        nullptr, "ERR_FAILED: application encryption key is unavailable");
    return;
  }
  auto* os_crypt = self->os_crypt_.get();
  os_crypt->GetInstance(
      base::BindOnce(&OnEncryptorReady, std::move(self), std::move(callback)));
}

void ElectronCookieEncryption::OnEncryptorReady(
    std::unique_ptr<ElectronCookieEncryption> self,
    CreatedCallback callback,
    scoped_refptr<os_crypt_async::Encryptor> encryptor) {
  // Do not delete an OSCryptAsync from its own initialization callback, even
  // on failure. The posted continuation also makes every result asynchronous.
  if (!encryptor->IsEncryptionAvailable()) {
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(
            [](std::unique_ptr<ElectronCookieEncryption> self,
               CreatedCallback callback) {
              std::move(callback).Run(
                  nullptr, "ERR_FAILED: application encryption is unavailable");
            },
            std::move(self), std::move(callback)));
    return;
  }
  // Providers finish updating their preferences before delivering the key.
  // Preserve unrelated Local State entries when committing this snapshot.
  self->original_state_.Merge(self->pref_store_->GetValues());
  const auto state_path = self->state_path_;
  auto state = std::move(self->original_state_);
  const auto runner = self->file_runner_;
  runner->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&CommitState, state_path, std::move(state),
                     std::move(encryptor)),
      base::BindOnce(
          [](std::unique_ptr<ElectronCookieEncryption> self,
             CreatedCallback callback, std::string error) {
            if (!error.empty()) {
              std::move(callback).Run(nullptr, std::move(error));
              return;
            }
            self->cookie_provider_ =
                std::make_unique<CookieEncryptionProviderImpl>(
                    self->os_crypt_.get());
            std::move(callback).Run(std::move(self), "");
          },
          std::move(self), std::move(callback)));
}

void ElectronCookieEncryption::Install(
    content::BrowserContext* context,
    std::unique_ptr<ElectronCookieEncryption> encryption) {
  CHECK(context);
  CHECK(encryption);
  CHECK(!context->GetUserData(&kEncryptionDataKey));
  context->SetUserData(&kEncryptionDataKey, std::move(encryption));
}

bool ElectronCookieEncryption::ConfigureNetworkContext(
    content::BrowserContext* context,
    network::mojom::NetworkContextParams* params) {
  auto* self = static_cast<ElectronCookieEncryption*>(
      context->GetUserData(&kEncryptionDataKey));
  if (!self) {
    return false;
  }
  params->cookie_encryption_provider = self->BindNewRemote();
  return true;
}

mojo::PendingRemote<network::mojom::CookieEncryptionProvider>
ElectronCookieEncryption::BindNewRemote() {
  CHECK(cookie_provider_);
  return cookie_provider_->BindNewRemote();
}

}  // namespace xenon::ipc
