// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_COOKIE_ENCRYPTION_H_
#define XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_COOKIE_ENCRYPTION_H_

#include <memory>
#include <string>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/supports_user_data.h"
#include "base/types/expected.h"
#include "base/values.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "services/network/public/mojom/cookie_encryption_provider.mojom-forward.h"
#include "services/network/public/mojom/network_context.mojom-forward.h"

class CookieEncryptionProviderImpl;
class InMemoryPrefStore;
class PrefRegistrySimple;
class PrefService;

namespace base {
class SequencedTaskRunner;
}
namespace content {
class BrowserContext;
}
namespace os_crypt_async {
class Encryptor;
class OSCryptAsync;
}  // namespace os_crypt_async

namespace xenon::ipc {

// The encryption state belongs to a redirected application session directory,
// not to the browser's --user-data-dir. Create completes only after its
// OS-backed key state has been read, initialized and committed. Install before
// creating any of the BrowserContext's network contexts, including keyed
// services.
class ElectronCookieEncryption final : public base::SupportsUserData::Data {
 public:
  using CreatedCallback =
      base::OnceCallback<void(std::unique_ptr<ElectronCookieEncryption>,
                              std::string error)>;
  using RegisterPrefs = base::OnceCallback<void(PrefRegistrySimple*)>;
  using ProviderFactory =
      base::OnceCallback<std::unique_ptr<os_crypt_async::OSCryptAsync>(
          PrefService*)>;

  static void Create(const base::FilePath& session_path,
                     CreatedCallback callback);
  static void Install(content::BrowserContext* context,
                      std::unique_ptr<ElectronCookieEncryption> encryption);
  // Returns false for ordinary browser contexts. Their existing provider is
  // deliberately unchanged.
  static bool ConfigureNetworkContext(
      content::BrowserContext* context,
      network::mojom::NetworkContextParams* params);

  // Injects providers without accessing the real desktop keychain in tests.
  static void CreateForTesting(const base::FilePath& session_path,
                               RegisterPrefs register_prefs,
                               ProviderFactory provider_factory,
                               CreatedCallback callback);

  ~ElectronCookieEncryption() override;
  mojo::PendingRemote<network::mojom::CookieEncryptionProvider> BindNewRemote();

 private:
  explicit ElectronCookieEncryption(const base::FilePath& session_path);
  static void CreateWithProviders(const base::FilePath& session_path,
                                  RegisterPrefs register_prefs,
                                  ProviderFactory provider_factory,
                                  CreatedCallback callback);
  static void OnStateRead(std::unique_ptr<ElectronCookieEncryption> self,
                          RegisterPrefs register_prefs,
                          ProviderFactory provider_factory,
                          CreatedCallback callback,
                          base::expected<base::DictValue, std::string> state);
  static void OnEncryptorReady(
      std::unique_ptr<ElectronCookieEncryption> self,
      CreatedCallback callback,
      scoped_refptr<os_crypt_async::Encryptor> encryptor);

  const base::FilePath state_path_;
  scoped_refptr<base::SequencedTaskRunner> file_runner_;
  base::DictValue original_state_;
  scoped_refptr<InMemoryPrefStore> pref_store_;
  std::unique_ptr<PrefService> local_state_;
  std::unique_ptr<os_crypt_async::OSCryptAsync> os_crypt_;
  std::unique_ptr<CookieEncryptionProviderImpl> cookie_provider_;
};

}  // namespace xenon::ipc

#endif  // XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_COOKIE_ENCRYPTION_H_
