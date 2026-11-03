// Copyright 2026 The Xenon Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_ACCOUNT_XENON_ACCOUNT_SERVICE_H_
#define XENON_OVERLAY_CHROME_BROWSER_ACCOUNT_XENON_ACCOUNT_SERVICE_H_

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "base/callback_list.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/supports_user_data.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/values.h"

class Profile;
namespace cross_transfer {
struct TlsConfig;
}
namespace base {
class SequencedTaskRunner;
}
namespace os_crypt_async {
class Encryptor;
}

// Profile-owned native account session. Credentials and device private keys
// never form part of the state exposed to a WebUI.
class XenonAccountService final : public base::SupportsUserData::Data {
 public:
  using StateCallback = base::OnceCallback<void(base::DictValue)>;

  struct Response {
    int status = 0;
    base::DictValue body;
    std::string error;
  };
  // A test handler runs on the service's worker sequence. Install and remove
  // it on the UI sequence before/after the test's outstanding requests finish.
  using RequestHandlerForTesting = base::RepeatingCallback<
      Response(std::string, std::string, std::string, base::DictValue)>;
  static void SetRequestHandlerForTesting(RequestHandlerForTesting handler);

  static XenonAccountService* GetForProfile(Profile* profile);
  ~XenonAccountService() override;

  bool HasAuthenticatedSession() const;
  std::string GetDeviceId() const;
  std::string GetAccountId() const;
  static cross_transfer::TlsConfig GetTlsConfig();
  // Snapshot for a dedicated transfer worker. Logout cancels all requests made
  // through this callback; it never exposes the token to a WebUI.
  using SessionRequest = base::RepeatingCallback<
      Response(std::string, std::string, base::DictValue)>;
  SessionRequest GetSessionRequest() const;
  base::CallbackListSubscription AddSessionChangedCallback(
      base::RepeatingClosure callback);
  // Native callers only. Credentials stay within the account worker.
  void RequestForSession(std::string method,
                         std::string path,
                         base::DictValue body,
                         base::OnceCallback<void(Response)> callback);
  base::DictValue GetState() const;
  void Authenticate(bool registration,
                    std::string username,
                    std::string password,
                    StateCallback callback);
  void Refresh(StateCallback callback);
  void Logout(StateCallback callback);

 private:
  explicit XenonAccountService(Profile* profile);
  void Initialize();
  void OnEncryptorReady(scoped_refptr<os_crypt_async::Encryptor> encryptor);
  void RestoreSession();
  void ValidateSession(StateCallback callback);
  void OnValidated(uint64_t generation, StateCallback callback, Response reply);
  void OnAuthenticated(uint64_t generation,
                       StateCallback callback,
                       Response reply);
  bool EnsureDeviceIdentity();
  bool PersistDeviceIdentities(const base::DictValue& identities);
  void SyncDevice(StateCallback callback);
  void OnDeviceSynced(uint64_t generation,
                      bool was_registration,
                      StateCallback callback,
                      Response reply);
  void OnDevicesListed(uint64_t generation,
                       StateCallback callback,
                       Response reply);
  void Request(std::string method,
               std::string path,
               base::DictValue body,
               base::OnceCallback<void(Response)> callback);
  void PersistSession();
  void StartTimers();
  void OnTimer();
  void OnExpired();
  void ClearSession();
  void InvalidatePendingRequests();
  void Finish(StateCallback callback, std::string error = {});
  bool IsCurrent(uint64_t generation, StateCallback& callback);
  bool HandleSessionError(const Response& reply);

  const raw_ptr<Profile> profile_;
  const scoped_refptr<base::SequencedTaskRunner> request_runner_;
  scoped_refptr<os_crypt_async::Encryptor> encryptor_;
  std::shared_ptr<std::atomic<bool>> cancellation_;
  uint64_t generation_ = 0;
  bool initializing_ = true;
  bool authenticated_ = false;
  bool restoring_ = false;
  bool busy_ = false;
  bool device_registered_ = false;
  std::string username_;
  std::string account_id_;
  std::string token_;
  base::Time expires_at_;
  std::string device_id_;
  std::string public_key_;
  base::DictValue identities_;
  base::ListValue devices_;
  std::string error_;
  base::RepeatingTimer heartbeat_timer_;
  base::OneShotTimer expiry_timer_;
  base::RepeatingClosureList session_changed_callbacks_;
  base::WeakPtrFactory<XenonAccountService> weak_factory_{this};
};

#endif  // XENON_OVERLAY_CHROME_BROWSER_ACCOUNT_XENON_ACCOUNT_SERVICE_H_
