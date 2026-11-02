// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/gm_key/gm_key_service.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <string>
#include <utility>

#include "base/check.h"
#include "base/check_op.h"
#include "base/functional/bind.h"
#include "base/process/process.h"
#include "base/task/single_thread_task_runner.h"
#include "base/unguessable_token.h"
#include "services/gm_key/device_change_monitor_win.h"
#include "services/gm_key/skf_abi.h"
#include "services/gm_key/skf_driver.h"

namespace gm_key {
namespace {

constexpr size_t kMaxSignerCapabilities = 64;

mojom::KeyResultPtr Result(mojom::KeyError error = mojom::KeyError::kOk) {
  auto result = mojom::KeyResult::New();
  result->error = error;
  return result;
}

mojom::KeyResultPtr Convert(Status status) {
  auto result = Result();
  result->skf_error = status.skf_error;
  result->system_error = status.system_error;
  result->pin_retries = status.pin_retries;
  if (status.ok()) {
    return result;
  }
  result->error = mojom::KeyError::kDriverFailure;
  if (status.error == Error::kUntrustedDriver) {
    result->error = mojom::KeyError::kUntrustedDriver;
  } else if (status.error == Error::kInvalidArgument) {
    result->error = mojom::KeyError::kInvalidArgument;
  } else if (status.error == Error::kAuthenticationRequired ||
             status.skf_error == internal::kNotLoggedIn) {
    result->error = mojom::KeyError::kPinRequired;
  } else if (status.skf_error == internal::kPinIncorrect) {
    result->error = mojom::KeyError::kPinIncorrect;
  } else if (status.skf_error == internal::kPinLocked) {
    result->error = mojom::KeyError::kPinLocked;
  } else if (status.skf_error == internal::kDeviceRemoved) {
    result->error = mojom::KeyError::kIdentityUnavailable;
  }
  return result;
}

}  // namespace

struct DeviceResult {
  explicit DeviceResult(mojom::KeyResultPtr value = Result())
      : result(std::move(value)) {}
  ~DeviceResult() = default;
  mojom::KeyResultPtr result;
  std::vector<mojom::KeyIdentityPtr> identities;
  mojom::KeyIdentityPtr identity;
  std::vector<uint8_t> bytes;
  bool changed = false;
  bool authorization_failed = false;
};

// Created, called and destroyed only by device_thread_. No pointer to a driver
// handle or its private key material crosses this object boundary.
class DeviceWorker {
 public:
  DeviceWorker(bool allow_unsigned,
               std::shared_ptr<std::atomic<uint64_t>> device_generation)
      : allow_unsigned_(allow_unsigned),
        device_generation_(std::move(device_generation)),
        observed_device_generation_(device_generation_->load()) {}
  ~DeviceWorker() = default;

  std::unique_ptr<DeviceResult> Configure(mojom::DriverConfigPtr config) {
    std::array<uint8_t, 32> hash;
    CHECK_EQ(config->sha256.size(), hash.size());
    std::copy(config->sha256.begin(), config->sha256.end(), hash.begin());
    Status status;
#if defined(GM_KEY_TESTING)
    if (allow_unsigned_) {
      status = SkfDriver::LoadPinnedUnsignedForTesting(
          config->library_path.value(), hash, &driver_);
    } else
#endif
    {
      status =
          SkfDriver::LoadPinned(config->library_path.value(), hash, &driver_);
    }
    return std::make_unique<DeviceResult>(Convert(status));
  }

  std::unique_ptr<DeviceResult> Enumerate() {
    auto output = std::make_unique<DeviceResult>();
    if (!driver_) {
      output->result = Result(mojom::KeyError::kNotConfigured);
      return output;
    }
    SyncDeviceGeneration();
    if (!entries_.empty()) {
      auto refreshed = Poll(true);
      if (refreshed->changed ||
          refreshed->result->error != mojom::KeyError::kOk) {
        return refreshed;
      }
    }
    // Reuse a stable inventory while devices are unchanged. Reopening SDK
    // application handles on every TLS connection can invalidate PIN state.
    if (entries_.empty()) {
      applications_.clear();
      containers_.clear();
      Status status = driver_->EnumerateDevices(&devices_);
      if (!status.ok()) {
        output->result = Convert(status);
        return output;
      }
      if (devices_.size() > 16) {
        output->result = Result(mojom::KeyError::kQueueFull);
        return output;
      }
      std::ranges::sort(devices_);
      for (const auto& device : devices_) {
        std::vector<std::string> applications;
        status = driver_->EnumerateApplications(device, &applications);
        if (!status.ok() || applications.size() > 32) {
          output->result = status.ok() ? Result(mojom::KeyError::kQueueFull)
                                       : Convert(status);
          entries_.clear();
          return output;
        }
        std::ranges::sort(applications);
        applications_[device] = applications;
        for (const auto& application : applications) {
          std::vector<std::string> containers;
          status =
              driver_->EnumerateContainers(device, application, &containers);
          if (!status.ok() || containers.size() > 64) {
            output->result = status.ok() ? Result(mojom::KeyError::kQueueFull)
                                         : Convert(status);
            entries_.clear();
            return output;
          }
          std::ranges::sort(containers);
          containers_[{device, application}] = containers;
          for (const auto& container : containers) {
            if (entries_.size() >= 64) {
              entries_.clear();
              output->result = Result(mojom::KeyError::kQueueFull);
              return output;
            }
            std::unique_ptr<SkfSession> session;
            status =
                driver_->OpenSession(device, application, container, &session);
            if (!status.ok()) {
              continue;  // RSA and non-certificate containers are not
                         // identities.
            }
            auto identity = mojom::KeyIdentity::New();
            status = session->ExportCertificate(true,
                                                &identity->signing_certificate);
            if (!status.ok()) {
              continue;
            }
            // A signing-only key may still serve RFC 8998. TLCP callers must
            // reject an identity without its separate encryption certificate.
            session->ExportCertificate(false,
                                       &identity->encryption_certificate);
            identity->identity_id = base::UnguessableToken::Create().ToString();
            identity->generation = next_generation_++;
            Entry entry{device, application, container, identity.Clone(),
                        std::move(session)};
            entries_.emplace(identity->identity_id, std::move(entry));
          }
        }
      }
      inventory_initialized_ = true;
      next_inventory_refresh_ = base::TimeTicks::Now() + base::Seconds(5);
    }
    if (SyncDeviceGeneration()) {
      output->changed = true;
      output->result = Result(mojom::KeyError::kIdentityUnavailable);
      return output;
    }
    for (const auto& [id, entry] : entries_) {
      output->identities.push_back(entry.identity.Clone());
    }
    return output;
  }

  std::unique_ptr<DeviceResult> Authorize(
      std::string id,
      uint64_t generation,
      SecurePin pin,
      std::shared_ptr<std::atomic_bool> cancelled,
      std::shared_ptr<std::atomic_bool> started) {
    auto output = std::make_unique<DeviceResult>();
    if (cancelled->load()) {
      output->result = Result(mojom::KeyError::kCancelled);
      return output;
    }
    output->changed = SyncDeviceGeneration();
    auto found = entries_.find(id);
    if (found == entries_.end() ||
        found->second.identity->generation != generation) {
      output->result = Result(mojom::KeyError::kIdentityUnavailable);
      return output;
    }
    Entry& entry = found->second;
    if (!entry.session) {
      Status status = driver_->OpenSession(entry.device, entry.application,
                                           entry.container, &entry.session);
      if (!status.ok()) {
        output->result = Convert(status);
        return output;
      }
    }
    std::vector<char> sdk_pin(pin.value().begin(), pin.value().end());
    sdk_pin.push_back(0);
    started->store(true);
    output->result = Convert(entry.session->VerifyUserPin(std::move(sdk_pin)));
    output->authorization_failed =
        output->result->error != mojom::KeyError::kOk;
    // A notification can arrive while a vendor blocks in VerifyPIN. Neither a
    // successful PIN nor its cached certificate can survive that generation.
    if (SyncDeviceGeneration()) {
      output->changed = true;
      output->result = Result(mojom::KeyError::kIdentityUnavailable);
      return output;
    }
    if (output->result->error == mojom::KeyError::kOk) {
      output->identity = entry.identity.Clone();
    }
    return output;
  }

  std::unique_ptr<DeviceResult> Sign(
      std::string id,
      uint64_t generation,
      std::array<uint8_t, 32> digest,
      std::shared_ptr<std::atomic_bool> cancelled) {
    auto output = std::make_unique<DeviceResult>();
    if (cancelled->load()) {
      output->result = Result(mojom::KeyError::kCancelled);
      return output;
    }
    output->changed = SyncDeviceGeneration();
    auto found = entries_.find(id);
    if (found == entries_.end() ||
        found->second.identity->generation != generation ||
        !found->second.session) {
      output->result = Result(mojom::KeyError::kIdentityUnavailable);
      return output;
    }
    Sm2Signature signature;
    Status status = found->second.session->SignSm2Digest(digest, &signature);
    if (status.ok()) {
      status = EncodeSm2SignatureDer(signature, &output->bytes);
    }
    output->result = Convert(status);
    if (SyncDeviceGeneration()) {
      output->changed = true;
      output->bytes.clear();
      output->result = Result(mojom::KeyError::kIdentityUnavailable);
      return output;
    }
    if (cancelled->load()) {
      output->bytes.clear();
      output->result = Result(mojom::KeyError::kCancelled);
    }
    if (!status.ok()) {
      InvalidateInventory();
      output->changed = true;
    }
    return output;
  }

  std::unique_ptr<DeviceResult> Lock(std::string id) {
    auto output = std::make_unique<DeviceResult>();
    auto found = entries_.find(id);
    if (found == entries_.end()) {
      output->result = Result(mojom::KeyError::kIdentityUnavailable);
      return output;
    }
    // SKF authentication may be shared by application handles. Revoke all
    // identities in that application rather than retaining stale grants.
    for (auto& [entry_id, entry] : entries_) {
      if (entry.device == found->second.device &&
          entry.application == found->second.application) {
        if (entry.session) {
          Status status = entry.session->ClearAuthentication();
          if (!status.ok()) {
            output->result = Convert(status);
          }
        }
        entry.identity->generation = next_generation_++;
      }
    }
    output->changed = true;
    return output;
  }

  std::unique_ptr<DeviceResult> Poll(bool force_inventory = false) {
    auto output = std::make_unique<DeviceResult>();
    output->changed = SyncDeviceGeneration();
    std::vector<std::string> devices;
    Status status = driver_->EnumerateDevices(&devices);
    std::ranges::sort(devices);
    if (!status.ok() || (inventory_initialized_ && devices != devices_)) {
      InvalidateInventory();
      devices_ = std::move(devices);
      output->changed = true;
    }
    output->result = Convert(status);
    if (output->changed || !inventory_initialized_ || entries_.empty() ||
        (!force_inventory &&
         base::TimeTicks::Now() < next_inventory_refresh_)) {
      return output;
    }
    next_inventory_refresh_ = base::TimeTicks::Now() + base::Seconds(5);
    const auto changed = [&](Status error = {}) {
      InvalidateInventory();
      output->changed = true;
      output->result = Convert(error);
    };
    // Reuse handles belonging to each application. Opening and closing an
    // unrelated application handle can clear some vendors' shared PIN state.
    for (const auto& [device, expected] : applications_) {
      auto entry = std::ranges::find_if(entries_, [&](const auto& item) {
        return item.second.device == device && item.second.session;
      });
      std::vector<std::string> actual;
      status = entry == entries_.end()
                   ? driver_->EnumerateApplications(device, &actual)
                   : entry->second.session->EnumerateApplications(&actual);
      std::ranges::sort(actual);
      if (!status.ok() || actual != expected) {
        changed(status);
        return output;
      }
    }
    for (const auto& [location, expected] : containers_) {
      auto entry = std::ranges::find_if(entries_, [&](const auto& item) {
        return item.second.device == location.first &&
               item.second.application == location.second &&
               item.second.session;
      });
      std::vector<std::string> actual;
      status = entry == entries_.end()
                   ? driver_->EnumerateContainers(location.first,
                                                  location.second, &actual)
                   : entry->second.session->EnumerateContainers(&actual);
      std::ranges::sort(actual);
      if (!status.ok() || actual != expected) {
        changed(status);
        return output;
      }
    }
    for (const auto& [id, entry] : entries_) {
      std::vector<uint8_t> signing, encryption;
      if (!entry.session) {
        changed();
        return output;
      }
      status = entry.session->ExportCertificate(true, &signing);
      if (!status.ok() || signing != entry.identity->signing_certificate) {
        changed(status);
        return output;
      }
      status = entry.session->ExportCertificate(false, &encryption);
      if ((!status.ok() && !entry.identity->encryption_certificate.empty()) ||
          encryption != entry.identity->encryption_certificate) {
        changed(status);
        return output;
      }
    }
    return output;
  }

#if defined(GM_KEY_TESTING)
  std::unique_ptr<DeviceResult> Control(uint32_t operation, uint32_t value) {
    auto output = std::make_unique<DeviceResult>();
    output->result = driver_ ? Convert(driver_->ControlForTesting(
                                   operation, value, &output->bytes))
                             : Result(mojom::KeyError::kNotConfigured);
    return output;
  }
#endif

 private:
  void InvalidateInventory() {
    entries_.clear();
    applications_.clear();
    containers_.clear();
    inventory_initialized_ = false;
  }
  bool SyncDeviceGeneration() {
    const uint64_t generation = device_generation_->load();
    if (generation == observed_device_generation_) {
      return false;
    }
    observed_device_generation_ = generation;
    InvalidateInventory();
    return true;
  }
  struct Entry {
    std::string device;
    std::string application;
    std::string container;
    mojom::KeyIdentityPtr identity;
    std::unique_ptr<SkfSession> session;
  };
  [[maybe_unused]] const bool allow_unsigned_;
  const std::shared_ptr<std::atomic<uint64_t>> device_generation_;
  uint64_t observed_device_generation_;
  uint64_t next_generation_ = 1;
  std::unique_ptr<SkfDriver> driver_;
  std::vector<std::string> devices_;
  std::map<std::string, std::vector<std::string>> applications_;
  std::map<std::pair<std::string, std::string>, std::vector<std::string>>
      containers_;
  bool inventory_initialized_ = false;
  base::TimeTicks next_inventory_refresh_;
  std::map<std::string, Entry> entries_;
};

GmKeyService::PendingSign::PendingSign(std::shared_ptr<std::atomic_bool> value,
                                       SignCallback completion)
    : cancelled(std::move(value)), callback(std::move(completion)) {}
GmKeyService::PendingSign::~PendingSign() = default;

GmKeyService::PendingAuthorization::PendingAuthorization(
    std::shared_ptr<std::atomic_bool> value,
    mojo::PendingReceiver<mojom::KeySigner> receiver,
    AuthorizeCallback completion)
    : cancelled(std::move(value)),
      signer(std::move(receiver)),
      callback(std::move(completion)) {}
GmKeyService::PendingAuthorization::~PendingAuthorization() = default;

GmKeyService::GmKeyService(mojo::PendingReceiver<mojom::KeyService> receiver)
    : GmKeyService(std::move(receiver), false, base::Seconds(30)) {}

GmKeyService::GmKeyService(mojo::PendingReceiver<mojom::KeyService> receiver,
                           bool allow_unsigned,
                           base::TimeDelta watchdog_timeout)
    : receiver_(this, std::move(receiver)),
      watchdog_timeout_(watchdog_timeout) {
  CHECK(device_thread_.Start());
  worker_.emplace(device_thread_.task_runner(), allow_unsigned,
                  device_generation_);
  device_monitor_ = std::make_unique<DeviceChangeMonitor>(
      device_generation_, base::BindRepeating(
                              [](base::WeakPtr<GmKeyService> service) {
                                if (service) {
                                  service->CheckDeviceChanges();
                                }
                              },
                              weak_factory_.GetWeakPtr()));
#if defined(GM_KEY_TESTING)
  // Tests inject a notification generation through their separate test-only
  // control pipe; physical USB activity must not make simulated tests flaky.
  if (allow_unsigned) {
    monitor_ready_ = true;
  } else
#endif
  {
    monitor_ready_ = device_monitor_->Start();
  }
  receiver_.set_disconnect_handler(base::BindOnce(
      &GmKeyService::OnControlDisconnected, weak_factory_.GetWeakPtr()));
  signers_.set_disconnect_handler(base::BindRepeating(
      &GmKeyService::OnSignerDisconnected, weak_factory_.GetWeakPtr()));
  watchdog_.Start(FROM_HERE, base::Milliseconds(100), this,
                  &GmKeyService::CheckWatchdog);
}

GmKeyService::~GmKeyService() {
  weak_factory_.InvalidateWeakPtrs();
  RevokeGrants(mojom::KeyError::kCancelled, false);
  // A vendor may also hang in SKF_Close* or DllMain. This service exclusively
  // owns its utility process, so do not join or unload a configured driver at
  // shutdown. Process teardown releases its OS resources without calling it.
  if (configuration_attempted_) {
    base::Process::TerminateCurrentProcessImmediately(operations_.empty() ? 0
                                                                          : 70);
  }
  worker_.Reset();
  device_thread_.Stop();
}

#if defined(GM_KEY_TESTING)
std::unique_ptr<GmKeyService> GmKeyService::CreateForTesting(
    mojo::PendingReceiver<mojom::KeyService> receiver,
    base::TimeDelta watchdog_timeout) {
  return std::unique_ptr<GmKeyService>(
      new GmKeyService(std::move(receiver), true, watchdog_timeout));
}
void GmKeyService::ControlForTesting(
    uint32_t operation,
    uint32_t value,
    base::OnceCallback<void(mojom::KeyResultPtr, const std::vector<uint8_t>&)>
        callback) {
  if (operation == 10) {
    const auto found = pending_authorizations_.find(value);
    if (found == pending_authorizations_.end()) {
      std::move(callback).Run(Result(mojom::KeyError::kInvalidArgument), {});
    } else if (found->second->started->load()) {
      std::move(callback).Run(Result(), {1});
    } else {
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
          FROM_HERE,
          base::BindOnce(&GmKeyService::ControlForTesting,
                         weak_factory_.GetWeakPtr(), operation, value,
                         std::move(callback)),
          base::Milliseconds(1));
    }
    return;
  }
  if (operation == 6) {
    // Model the OS event independently of the busy SDK thread. The simulated
    // remove/reinsert leaves exactly the same SKF name at the next poll.
    device_generation_->fetch_add(1);
    CheckDeviceChanges();
  }
  uint64_t task = BeginOperation();
  if (!task) {
    std::move(callback).Run(Result(mojom::KeyError::kQueueFull), {});
    return;
  }
  auto completed = base::BindOnce(
      [](base::WeakPtr<GmKeyService> self, uint64_t task_id,
         decltype(callback) completion, std::unique_ptr<DeviceResult> result) {
        if (self) {
          self->FinishOperation(task_id);
          if (result->changed) {
            self->RevokeGrants(mojom::KeyError::kIdentityUnavailable);
          }
          std::move(completion)
              .Run(std::move(result->result), std::move(result->bytes));
        }
      },
      weak_factory_.GetWeakPtr(), task, std::move(callback));
  if (operation == 9) {
    worker_.AsyncCall(&DeviceWorker::Poll)
        .WithArgs(true)
        .Then(std::move(completed));
  } else {
    worker_.AsyncCall(&DeviceWorker::Control)
        .WithArgs(operation, value)
        .Then(std::move(completed));
  }
}
#endif

uint64_t GmKeyService::BeginOperation() {
  if (operations_.size() >= 64) {
    return 0;
  }
  const uint64_t id = next_operation_++;
  CHECK(id);
  operations_.emplace(id, base::TimeTicks::Now() + watchdog_timeout_);
  return id;
}
void GmKeyService::FinishOperation(uint64_t operation) {
  operations_.erase(operation);
}

void GmKeyService::Configure(mojom::DriverConfigPtr config,
                             mojo::PendingRemote<mojom::KeyObserver> observer,
                             ConfigureCallback callback) {
  if (configuration_attempted_) {
    std::move(callback).Run(Result(mojom::KeyError::kAlreadyConfigured));
    return;
  }
  if (!config || !config->library_path.IsAbsolute() ||
      config->sha256.size() != 32 ||
      std::all_of(config->sha256.begin(), config->sha256.end(),
                  [](uint8_t b) { return b == 0; })) {
    std::move(callback).Run(Result(mojom::KeyError::kInvalidArgument));
    return;
  }
  if (!monitor_ready_) {
    std::move(callback).Run(Result(mojom::KeyError::kDriverFailure));
    return;
  }
  configuration_attempted_ = true;
  if (observer) {
    observer_.Bind(std::move(observer));
  }
  uint64_t operation = BeginOperation();
  worker_.AsyncCall(&DeviceWorker::Configure)
      .WithArgs(std::move(config))
      .Then(base::BindOnce(&GmKeyService::OnConfigure,
                           weak_factory_.GetWeakPtr(), operation,
                           std::move(callback)));
}
void GmKeyService::OnConfigure(uint64_t operation,
                               ConfigureCallback callback,
                               std::unique_ptr<DeviceResult> result) {
  FinishOperation(operation);
  configured_ = result->result->error == mojom::KeyError::kOk;
  if (configured_) {
    poll_timer_.Start(FROM_HERE, base::Seconds(1), this,
                      &GmKeyService::PollDevices);
  }
  std::move(callback).Run(std::move(result->result));
}

void GmKeyService::Enumerate(EnumerateCallback callback) {
  uint64_t operation = configured_ ? BeginOperation() : 0;
  if (!operation) {
    std::move(callback).Run(
        Result(configured_ ? mojom::KeyError::kQueueFull
                           : mojom::KeyError::kNotConfigured),
        {});
    return;
  }
  worker_.AsyncCall(&DeviceWorker::Enumerate)
      .Then(base::BindOnce(&GmKeyService::OnEnumerate,
                           weak_factory_.GetWeakPtr(), operation,
                           std::move(callback)));
}
void GmKeyService::OnEnumerate(uint64_t operation,
                               EnumerateCallback callback,
                               std::unique_ptr<DeviceResult> result) {
  FinishOperation(operation);
  const bool device_changed = CheckDeviceChanges();
  if (result->changed || device_changed) {
    if (!device_changed) {
      RevokeGrants(mojom::KeyError::kIdentityUnavailable);
    }
    std::move(callback).Run(Result(mojom::KeyError::kIdentityUnavailable), {});
    return;
  }
  std::move(callback).Run(std::move(result->result),
                          std::move(result->identities));
}

void GmKeyService::Authorize(uint64_t request_id,
                             const std::string& id,
                             uint64_t generation,
                             SecurePin pin,
                             mojo::PendingReceiver<mojom::KeySigner> signer,
                             AuthorizeCallback callback) {
  if (!request_id || request_id <= last_authorization_request_) {
    std::move(callback).Run(Result(mojom::KeyError::kInvalidArgument), nullptr);
    return;
  }
  last_authorization_request_ = request_id;
  if (!configured_ || id.empty() || !generation || !signer ||
      pin.value().empty() || pin.value().size() > 128 ||
      std::find(pin.value().begin(), pin.value().end(), 0) !=
          pin.value().end()) {
    std::move(callback).Run(Result(mojom::KeyError::kInvalidArgument), nullptr);
    return;
  }
  if (CheckDeviceChanges()) {
    std::move(callback).Run(Result(mojom::KeyError::kIdentityUnavailable),
                            nullptr);
    return;
  }
  uint64_t operation =
      signers_.size() + pending_authorizations_.size() < kMaxSignerCapabilities
          ? BeginOperation()
          : 0;
  if (!operation) {
    std::move(callback).Run(Result(mojom::KeyError::kQueueFull), nullptr);
    return;
  }
  auto cancelled = std::make_shared<std::atomic_bool>(false);
  auto [pending, inserted] = pending_authorizations_.emplace(
      request_id, std::make_unique<PendingAuthorization>(
                      cancelled, std::move(signer), std::move(callback)));
  CHECK(inserted);
  worker_.AsyncCall(&DeviceWorker::Authorize)
      .WithArgs(id, generation, std::move(pin), cancelled,
                pending->second->started)
      .Then(base::BindOnce(&GmKeyService::OnAuthorize,
                           weak_factory_.GetWeakPtr(), operation, request_id));
}
void GmKeyService::OnAuthorize(uint64_t operation,
                               uint64_t request_id,
                               std::unique_ptr<DeviceResult> result) {
  FinishOperation(operation);
  const bool device_changed = CheckDeviceChanges();
  if (result->changed || device_changed) {
    if (!device_changed) {
      RevokeGrants(mojom::KeyError::kIdentityUnavailable);
    }
    return;
  }
  const auto found = pending_authorizations_.find(request_id);
  AuthorizeCallback callback;
  if (found != pending_authorizations_.end()) {
    callback = std::move(found->second->callback);
    if (result->identity) {
      // Success is provisional until the browser accepts this exact request.
      // No signer is bound and no reusable authorization is created here.
      found->second->identity = result->identity.Clone();
      found->second->commit_deadline =
          base::TimeTicks::Now() + watchdog_timeout_;
    } else {
      pending_authorizations_.erase(found);
    }
  }
  if (result->authorization_failed) {
    // A failed SDK verification can clear shared application PIN state even
    // if its browser request was cancelled. Revoke previous capabilities, but
    // do not cancel newer PIN attempts already queued after this one.
    RevokeGrants(mojom::KeyError::kPinRequired, false, request_id);
  }
  if (callback) {
    std::move(callback).Run(std::move(result->result),
                            std::move(result->identity));
  }
}

void GmKeyService::CommitAuthorization(uint64_t request_id) {
  if (CheckDeviceChanges()) {
    return;
  }
  const auto found = pending_authorizations_.find(request_id);
  if (found == pending_authorizations_.end() || !found->second->identity) {
    return;
  }
  if (found->second->commit_deadline <= base::TimeTicks::Now()) {
    CancelAuthorization(request_id);
    return;
  }
  auto pending = std::move(found->second);
  pending_authorizations_.erase(found);
  auto& identity = *pending->identity;
  authorized_identities_[identity.identity_id] = pending->identity.Clone();
  signers_.Add(this, std::move(pending->signer),
               Grant{identity.identity_id, identity.generation});
}

void GmKeyService::CancelAuthorization(uint64_t request_id) {
  // The browser uses one ordered control pipe. Also consume an early cancel's
  // ID so an authorization arriving later can never resurrect that request.
  last_authorization_request_ =
      std::max(last_authorization_request_, request_id);
  const auto found = pending_authorizations_.find(request_id);
  if (found == pending_authorizations_.end()) {
    return;
  }
  auto pending = std::move(found->second);
  pending_authorizations_.erase(found);
  pending->cancelled->store(true);
  if (pending->callback) {
    std::move(pending->callback)
        .Run(Result(mojom::KeyError::kCancelled), nullptr);
  }
}

void GmKeyService::GrantAuthorized(
    const std::string& identity_id,
    uint64_t generation,
    mojo::PendingReceiver<mojom::KeySigner> signer,
    GrantAuthorizedCallback callback) {
  if (!configured_ || identity_id.empty() || !generation || !signer) {
    std::move(callback).Run(Result(mojom::KeyError::kInvalidArgument), nullptr);
    return;
  }
  if (CheckDeviceChanges()) {
    std::move(callback).Run(Result(mojom::KeyError::kPinRequired), nullptr);
    return;
  }
  const auto found = authorized_identities_.find(identity_id);
  if (found == authorized_identities_.end() ||
      found->second->generation != generation) {
    std::move(callback).Run(Result(mojom::KeyError::kPinRequired), nullptr);
    return;
  }
  if (signers_.size() + pending_authorizations_.size() >=
      kMaxSignerCapabilities) {
    std::move(callback).Run(Result(mojom::KeyError::kQueueFull), nullptr);
    return;
  }
  // This is a browser-only control method. Reuse the authorization decision,
  // not the PIN, and give each NetworkContext its own revocable capability.
  signers_.Add(this, std::move(signer), Grant{identity_id, generation});
  std::move(callback).Run(Result(), found->second.Clone());
}

void GmKeyService::Sign(uint64_t request_id,
                        const std::vector<uint8_t>& digest,
                        SignCallback callback) {
  if (CheckDeviceChanges()) {
    std::move(callback).Run(Result(mojom::KeyError::kIdentityUnavailable), {});
    return;
  }
  const SignKey key(signers_.current_receiver(), request_id);
  auto& grant = signers_.current_context();
  if (digest.size() != 32 || !request_id ||
      request_id <= grant.last_request_id || pending_signs_.contains(key)) {
    std::move(callback).Run(Result(mojom::KeyError::kInvalidArgument), {});
    return;
  }
  grant.last_request_id = request_id;
  uint64_t operation = BeginOperation();
  if (!operation) {
    std::move(callback).Run(Result(mojom::KeyError::kQueueFull), {});
    return;
  }
  auto cancelled = std::make_shared<std::atomic_bool>(false);
  pending_signs_.emplace(
      key, std::make_unique<PendingSign>(cancelled, std::move(callback)));
  std::array<uint8_t, 32> fixed_digest;
  std::copy(digest.begin(), digest.end(), fixed_digest.begin());
  worker_.AsyncCall(&DeviceWorker::Sign)
      .WithArgs(grant.identity, grant.generation, fixed_digest, cancelled)
      .Then(base::BindOnce(&GmKeyService::OnSign, weak_factory_.GetWeakPtr(),
                           operation, key));
}
void GmKeyService::OnSign(uint64_t operation,
                          SignKey key,
                          std::unique_ptr<DeviceResult> result) {
  FinishOperation(operation);
  if (CheckDeviceChanges()) {
    return;
  }
  auto found = pending_signs_.find(key);
  if (found != pending_signs_.end()) {
    auto callback = std::move(found->second->callback);
    pending_signs_.erase(found);
    std::move(callback).Run(std::move(result->result),
                            std::move(result->bytes));
  }
  if (result->changed) {
    RevokeGrants(mojom::KeyError::kIdentityUnavailable);
  }
}
void GmKeyService::Cancel(uint64_t request_id) {
  const SignKey key(signers_.current_receiver(), request_id);
  auto found = pending_signs_.find(key);
  if (found == pending_signs_.end()) {
    return;
  }
  found->second->cancelled->store(true);
  auto callback = std::move(found->second->callback);
  pending_signs_.erase(found);
  std::move(callback).Run(Result(mojom::KeyError::kCancelled), {});
}

void GmKeyService::Lock(const std::string& identity_id, LockCallback callback) {
  if (!configured_) {
    std::move(callback).Run(Result(mojom::KeyError::kNotConfigured));
    return;
  }
  // Revoke before queueing any SDK work; a waiting or blocked driver cannot
  // extend authorization on the main Mojo sequence.
  RevokeGrants(mojom::KeyError::kCancelled);
  uint64_t operation = BeginOperation();
  if (!operation) {
    std::move(callback).Run(Result(mojom::KeyError::kQueueFull));
    return;
  }
  worker_.AsyncCall(&DeviceWorker::Lock)
      .WithArgs(identity_id)
      .Then(base::BindOnce(&GmKeyService::OnLock, weak_factory_.GetWeakPtr(),
                           operation, std::move(callback)));
}
void GmKeyService::OnLock(uint64_t operation,
                          LockCallback callback,
                          std::unique_ptr<DeviceResult> result) {
  FinishOperation(operation);
  std::move(callback).Run(std::move(result->result));
}

void GmKeyService::OnSignerDisconnected() {
  const auto receiver = signers_.current_receiver();
  for (auto it = pending_signs_.begin(); it != pending_signs_.end();) {
    if (it->first.first == receiver) {
      it->second->cancelled->store(true);
      it = pending_signs_.erase(it);
    } else {
      ++it;
    }
  }
}
void GmKeyService::RevokeGrants(mojom::KeyError reason,
                                bool identities_changed,
                                uint64_t authorization_cutoff) {
  authorized_identities_.clear();
  std::vector<AuthorizeCallback> cancelled_authorizations;
  for (auto it = pending_authorizations_.begin();
       it != pending_authorizations_.end();) {
    if (it->first > authorization_cutoff) {
      ++it;
      continue;
    }
    it->second->cancelled->store(true);
    if (it->second->callback) {
      cancelled_authorizations.push_back(std::move(it->second->callback));
    }
    it = pending_authorizations_.erase(it);
  }
  for (auto& [key, request] : pending_signs_) {
    request->cancelled->store(true);
    std::move(request->callback).Run(Result(reason), {});
  }
  pending_signs_.clear();
  signers_.Clear();
  for (auto& callback : cancelled_authorizations) {
    std::move(callback).Run(Result(reason), nullptr);
  }
  if (observer_ && identities_changed) {
    observer_->OnIdentitiesChanged();
  }
}
bool GmKeyService::CheckDeviceChanges() {
  const uint64_t generation = device_generation_->load();
  if (generation == observed_device_generation_) {
    return false;
  }
  observed_device_generation_ = generation;
  RevokeGrants(mojom::KeyError::kIdentityUnavailable);
  return true;
}
void GmKeyService::PollDevices() {
  if (!configured_ || polling_) {
    return;
  }
  uint64_t operation = BeginOperation();
  if (!operation) {
    return;
  }
  polling_ = true;
  worker_.AsyncCall(&DeviceWorker::Poll)
      .WithArgs(false)
      .Then(base::BindOnce(&GmKeyService::OnPoll, weak_factory_.GetWeakPtr(),
                           operation));
}
void GmKeyService::OnPoll(uint64_t operation,
                          std::unique_ptr<DeviceResult> result) {
  FinishOperation(operation);
  polling_ = false;
  if (CheckDeviceChanges()) {
    return;
  }
  if (result->changed) {
    RevokeGrants(mojom::KeyError::kIdentityUnavailable);
  }
}
void GmKeyService::CheckWatchdog() {
  const base::TimeTicks now = base::TimeTicks::Now();
  // A prepared authorization owns a pipe but has no outstanding SDK call.
  // Expire abandoned commits without killing the service or other grants.
  for (auto it = pending_authorizations_.begin();
       it != pending_authorizations_.end();) {
    if (it->second->identity && it->second->commit_deadline <= now) {
      it->second->cancelled->store(true);
      it = pending_authorizations_.erase(it);
    } else {
      ++it;
    }
  }
  for (const auto& [id, deadline] : operations_) {
    if (deadline <= now) {
      if (observer_) {
        observer_->OnServiceFailure(mojom::KeyError::kTimedOut);
      }
      // Killing the dedicated process is the only safe recovery from an
      // uninterruptible vendor call. The browser observes pipe closure and
      // invalidates every grant before launching a fresh service.
      base::Process::TerminateCurrentProcessImmediately(71);
    }
  }
}
void GmKeyService::OnControlDisconnected() {
  RevokeGrants(mojom::KeyError::kCancelled, false);
  base::Process::TerminateCurrentProcessImmediately(operations_.empty() ? 0
                                                                        : 72);
}

}  // namespace gm_key
