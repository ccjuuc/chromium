// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SERVICES_GM_KEY_GM_KEY_SERVICE_H_
#define SERVICES_GM_KEY_GM_KEY_SERVICE_H_

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <utility>

#include "base/memory/weak_ptr.h"
#include "base/threading/sequence_bound.h"
#include "base/threading/thread.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "services/gm_key/public/mojom/gm_key_service.mojom.h"

namespace gm_key {

class DeviceWorker;
class DeviceChangeMonitor;
struct DeviceResult;

// One instance per dedicated utility process. Only this service's fixed device
// thread invokes the SDK. The Mojo thread remains responsive during SDK waits.
class GmKeyService : public mojom::KeyService, public mojom::KeySigner {
 public:
  explicit GmKeyService(mojo::PendingReceiver<mojom::KeyService> receiver);
  ~GmKeyService() override;
  GmKeyService(const GmKeyService&) = delete;
  GmKeyService& operator=(const GmKeyService&) = delete;

#if defined(GM_KEY_TESTING)
  static std::unique_ptr<GmKeyService> CreateForTesting(
      mojo::PendingReceiver<mojom::KeyService> receiver,
      base::TimeDelta watchdog_timeout);
  void ControlForTesting(
      uint32_t operation,
      uint32_t value,
      base::OnceCallback<void(mojom::KeyResultPtr, const std::vector<uint8_t>&)>
          callback);
#endif

  void Configure(mojom::DriverConfigPtr config,
                 mojo::PendingRemote<mojom::KeyObserver> observer,
                 ConfigureCallback callback) override;
  void Enumerate(EnumerateCallback callback) override;
  void Authorize(uint64_t request_id,
                 const std::string& identity_id,
                 uint64_t generation,
                 SecurePin pin,
                 mojo::PendingReceiver<mojom::KeySigner> signer,
                 AuthorizeCallback callback) override;
  void CommitAuthorization(uint64_t request_id) override;
  void CancelAuthorization(uint64_t request_id) override;
  void GrantAuthorized(const std::string& identity_id,
                       uint64_t generation,
                       mojo::PendingReceiver<mojom::KeySigner> signer,
                       GrantAuthorizedCallback callback) override;
  void Lock(const std::string& identity_id, LockCallback callback) override;
  void Sign(uint64_t request_id,
            const std::vector<uint8_t>& digest,
            SignCallback callback) override;
  void Cancel(uint64_t request_id) override;

 private:
  struct Grant {
    std::string identity;
    uint64_t generation;
    uint64_t last_request_id = 0;
  };
  struct PendingSign {
    PendingSign(std::shared_ptr<std::atomic_bool> cancelled,
                SignCallback callback);
    ~PendingSign();
    std::shared_ptr<std::atomic_bool> cancelled;
    SignCallback callback;
  };
  struct PendingAuthorization {
    PendingAuthorization(std::shared_ptr<std::atomic_bool> cancelled,
                         mojo::PendingReceiver<mojom::KeySigner> signer,
                         AuthorizeCallback callback);
    ~PendingAuthorization();
    std::shared_ptr<std::atomic_bool> cancelled;
    std::shared_ptr<std::atomic_bool> started =
        std::make_shared<std::atomic_bool>(false);
    mojo::PendingReceiver<mojom::KeySigner> signer;
    AuthorizeCallback callback;
    mojom::KeyIdentityPtr identity;
    base::TimeTicks commit_deadline;
  };
  using SignKey = std::pair<mojo::ReceiverId, uint64_t>;

  GmKeyService(mojo::PendingReceiver<mojom::KeyService> receiver,
               bool allow_unsigned_test_driver,
               base::TimeDelta watchdog_timeout);
  uint64_t BeginOperation();
  void FinishOperation(uint64_t operation);
  void OnConfigure(uint64_t operation,
                   ConfigureCallback callback,
                   std::unique_ptr<DeviceResult> result);
  void OnEnumerate(uint64_t operation,
                   EnumerateCallback callback,
                   std::unique_ptr<DeviceResult> result);
  void OnAuthorize(uint64_t operation,
                   uint64_t request_id,
                   std::unique_ptr<DeviceResult> result);
  void OnSign(uint64_t operation,
              SignKey key,
              std::unique_ptr<DeviceResult> result);
  void OnLock(uint64_t operation,
              LockCallback callback,
              std::unique_ptr<DeviceResult> result);
  void OnSignerDisconnected();
  void RevokeGrants(mojom::KeyError reason,
                    bool identities_changed = true,
                    uint64_t authorization_cutoff = UINT64_MAX);
  bool CheckDeviceChanges();
  void PollDevices();
  void OnPoll(uint64_t operation, std::unique_ptr<DeviceResult> result);
  void CheckWatchdog();
  void OnControlDisconnected();

  base::Thread device_thread_{"GmKeyDevice"};
  const std::shared_ptr<std::atomic<uint64_t>> device_generation_ =
      std::make_shared<std::atomic<uint64_t>>(1);
  std::unique_ptr<DeviceChangeMonitor> device_monitor_;
  uint64_t observed_device_generation_ = 1;
  base::SequenceBound<DeviceWorker> worker_;
  mojo::Receiver<mojom::KeyService> receiver_;
  mojo::ReceiverSet<mojom::KeySigner, Grant> signers_;
  mojo::Remote<mojom::KeyObserver> observer_;
  // Public identity metadata only. Cleared synchronously by every revocation;
  // never contains a PIN or a private key.
  std::map<std::string, mojom::KeyIdentityPtr> authorized_identities_;
  std::map<uint64_t, std::unique_ptr<PendingAuthorization>>
      pending_authorizations_;
  std::map<SignKey, std::unique_ptr<PendingSign>> pending_signs_;
  std::map<uint64_t, base::TimeTicks> operations_;
  uint64_t next_operation_ = 1;
  uint64_t last_authorization_request_ = 0;
  bool configuration_attempted_ = false;
  bool configured_ = false;
  bool polling_ = false;
  bool monitor_ready_ = false;
  const base::TimeDelta watchdog_timeout_;
  base::RepeatingTimer watchdog_;
  base::RepeatingTimer poll_timer_;
  base::WeakPtrFactory<GmKeyService> weak_factory_{this};
};

}  // namespace gm_key

#endif  // SERVICES_GM_KEY_GM_KEY_SERVICE_H_
