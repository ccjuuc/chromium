// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_SAFE_STORAGE_H_
#define XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_SAFE_STORAGE_H_

#include <list>
#include <set>
#include <string>

#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/types/expected.h"
#include "base/values.h"
#include "components/os_crypt/async/common/encryptor.h"

namespace xenon::ipc {

// Browser UI sequence only. The loader obtains the host BrowserProcess's real
// OSCrypt encryptor; no key or provider is supplied by JavaScript. Owner IDs
// include the authenticated container generation.
class ElectronSafeStorage {
 public:
  using Result = base::expected<base::Value, std::string>;
  using Callback = base::OnceCallback<void(Result)>;
  using EncryptorReady =
      base::OnceCallback<void(scoped_refptr<os_crypt_async::Encryptor>)>;
  using EncryptorLoader = base::RepeatingCallback<void(EncryptorReady)>;

  explicit ElectronSafeStorage(EncryptorLoader loader);
  ~ElectronSafeStorage();
  ElectronSafeStorage(const ElectronSafeStorage&) = delete;
  ElectronSafeStorage& operator=(const ElectronSafeStorage&) = delete;

  // Starts initialization without spinning a nested event loop. Availability
  // stays false and synchronous operations fail until the loader completes.
  void Initialize();
  Result Call(const std::string& owner, const base::Value& request);
  void CallAsync(const std::string& owner,
                 base::Value request,
                 Callback callback);
  void ReleaseOwner(const std::string& owner);

 private:
  struct Pending {
    std::string owner;
    base::Value request;
    Callback callback;
  };

  void OnReady(scoped_refptr<os_crypt_async::Encryptor> encryptor);
  bool IsBasicTextBackend() const;
  bool IsAvailable(const std::string& owner) const;

  EncryptorLoader loader_;
  scoped_refptr<os_crypt_async::Encryptor> encryptor_;
  bool requested_ = false;
  bool initialized_ = false;
  std::set<std::string> allow_plain_text_;
  std::list<Pending> pending_;
  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<ElectronSafeStorage> weak_factory_{this};
};

}  // namespace xenon::ipc

#endif  // XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_SAFE_STORAGE_H_
