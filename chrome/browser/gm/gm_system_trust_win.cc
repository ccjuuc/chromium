// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/gm/gm_system_trust_win.h"

#include <algorithm>
#include <array>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

#include "base/check_is_test.h"
#include "base/compiler_specific.h"
#include "base/containers/heap_array.h"
#include "base/containers/span.h"
#include "base/containers/to_vector.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/no_destructor.h"
#include "base/synchronization/lock.h"
#include "base/task/bind_post_task.h"
#include "base/task/thread_pool.h"
#include "base/threading/scoped_blocking_call.h"
#include "base/threading/sequence_bound.h"
#include "base/win/object_watcher.h"
#include "base/win/scoped_handle.h"
#include "crypto/scoped_capi_types.h"
#include "net/cert/cert_database.h"
#include "net/cert/x509_util_win.h"
#include "net/gm/gm_tls_policy.h"

namespace gm_browser {
namespace {

using Der = std::vector<uint8_t>;
using CertificateSet = std::set<Der>;

struct TestSnapshot {
  base::Lock lock;
  network::mojom::GmAutoConfigPtr value GUARDED_BY(lock);
};

TestSnapshot& SnapshotForTesting() {
  static base::NoDestructor<TestSnapshot> snapshot;
  return *snapshot;
}

// Flags=0 applies the intersection of the stored EKU property and the signed
// EKU extension. An explicitly empty usage property means trust no purpose;
// a missing property and extension mean unrestricted usage. See the identical
// Windows trust semantics in net/cert/internal/trust_store_win.cc.
bool AllowsServerAuthentication(PCCERT_CONTEXT cert) {
  DWORD size = 0;
  if (!CertGetEnhancedKeyUsage(cert, 0, nullptr, &size) ||
      size < sizeof(CERT_ENHKEY_USAGE) || size > 256 * 1024) {
    return false;
  }
  auto buffer = base::HeapArray<uint8_t>::WithSize(size);
  auto* usage = reinterpret_cast<CERT_ENHKEY_USAGE*>(buffer.data());
  if (!CertGetEnhancedKeyUsage(cert, 0, usage, &size)) {
    return false;
  }
  if (!usage->cUsageIdentifier) {
    return GetLastError() == static_cast<DWORD>(CRYPT_E_NOT_FOUND);
  }
  // SAFETY: CryptoAPI returned an array of cUsageIdentifier NUL-terminated
  // OIDs.
  const auto identifiers = UNSAFE_BUFFERS(
      base::span(usage->rgpszUsageIdentifier, usage->cUsageIdentifier));
  for (std::string_view oid : identifiers) {
    if (oid == szOID_PKIX_KP_SERVER_AUTH ||
        oid == szOID_ANY_ENHANCED_KEY_USAGE) {
      return true;
    }
  }
  return false;
}

bool ReadCertificates(HCERTSTORE store,
                      bool distrust,
                      CertificateSet* allowed,
                      CertificateSet* restricted,
                      size_t* bytes) {
  if (!store) {
    return false;
  }
  crypto::ScopedPCCERT_CONTEXT cert;
  while (true) {
    cert.reset(CertEnumCertificatesInStore(store, cert.release()));
    if (!cert) {
      return GetLastError() == static_cast<DWORD>(CRYPT_E_NOT_FOUND);
    }
    if (!cert->cbCertEncoded || cert->cbCertEncoded > 256 * 1024) {
      return false;
    }
    Der der = base::ToVector(net::x509_util::CertContextAsSpan(cert.get()));
    // Bound work and memory while enumerating, before constructing the IPC
    // snapshot. Repeated physical-store copies count against this budget too.
    *bytes += der.size();
    if (*bytes > 64 * 1024 * 1024) {
      return false;
    }
    if (distrust) {
      // Disallowed takes precedence regardless of expiry or EKU properties.
      allowed->insert(std::move(der));
    } else if (!AllowsServerAuthentication(cert.get())) {
      // Preserve property restrictions even when the peer sends this issuer.
      restricted->insert(std::move(der));
    } else if (CertVerifyTimeValidity(nullptr, cert->pCertInfo) == 0) {
      allowed->insert(std::move(der));
    }
    if (allowed->size() > 4096 || restricted->size() > 4096) {
      return false;
    }
  }
}

bool ReadCrls(HCERTSTORE store, CertificateSet* output, size_t* bytes) {
  PCCRL_CONTEXT crl = nullptr;
  while ((crl = CertEnumCRLsInStore(store, crl)) != nullptr) {
    if (!crl->cbCrlEncoded || crl->cbCrlEncoded > 256 * 1024) {
      CertFreeCRLContext(crl);
      return false;
    }
    *bytes += crl->cbCrlEncoded;
    if (*bytes > 64 * 1024 * 1024) {
      CertFreeCRLContext(crl);
      return false;
    }
    // SAFETY: CryptoAPI owns this DER buffer for the lifetime of crl.
    output->insert(base::ToVector(
        UNSAFE_BUFFERS(base::span(crl->pbCrlEncoded, crl->cbCrlEncoded))));
    if (output->size() > 128) {
      CertFreeCRLContext(crl);
      return false;
    }
  }
  return GetLastError() == static_cast<DWORD>(CRYPT_E_NOT_FOUND);
}

network::mojom::GmAutoConfigPtr EmptySnapshot() {
  auto config = network::mojom::GmAutoConfig::New();
  config->enabled = true;
  return config;
}

network::mojom::GmAutoConfigPtr ReadStores(HCERTSTORE roots,
                                           HCERTSTORE intermediates,
                                           HCERTSTORE disallowed) {
  CertificateSet trusted;
  CertificateSet issuers;
  CertificateSet denied;
  CertificateSet restricted;
  CertificateSet crls;
  size_t bytes = 0;
  if (!ReadCertificates(roots, false, &trusted, &restricted, &bytes) ||
      !ReadCertificates(intermediates, false, &issuers, &restricted, &bytes) ||
      !ReadCertificates(disallowed, true, &denied, &restricted, &bytes) ||
      !ReadCrls(roots, &crls, &bytes) ||
      !ReadCrls(intermediates, &crls, &bytes)) {
    return EmptySnapshot();
  }
  // Duplicate copies can carry different usage properties. A usable local
  // copy permits server auth, but explicit Disallowed always overrides it.
  for (const auto& der : restricted) {
    if (!trusted.contains(der) && !issuers.contains(der)) {
      denied.insert(der);
    }
  }
  for (const auto& der : denied) {
    trusted.erase(der);
    issuers.erase(der);
  }
  auto config = EmptySnapshot();
  config->trust_anchors.assign(trusted.begin(), trusted.end());
  config->untrusted_intermediates.assign(issuers.begin(), issuers.end());
  config->disallowed_certificates.assign(denied.begin(), denied.end());
  config->crls.assign(crls.begin(), crls.end());
  net::gm::AutoConfig validation;
  validation.epoch = 1;
  validation.trust_anchors = config->trust_anchors;
  validation.untrusted_intermediates = config->untrusted_intermediates;
  validation.disallowed_certificates = config->disallowed_certificates;
  validation.crls = config->crls;
  return validation.IsValid() ? std::move(config) : EmptySnapshot();
}

bool AddRegistryStores(HCERTSTORE collection, const wchar_t* name) {
  for (DWORD location : {CERT_SYSTEM_STORE_LOCAL_MACHINE,
                         CERT_SYSTEM_STORE_LOCAL_MACHINE_GROUP_POLICY,
                         CERT_SYSTEM_STORE_LOCAL_MACHINE_ENTERPRISE,
                         CERT_SYSTEM_STORE_CURRENT_USER,
                         CERT_SYSTEM_STORE_CURRENT_USER_GROUP_POLICY}) {
    crypto::ScopedHCERTSTORE store(CertOpenStore(
        CERT_STORE_PROV_SYSTEM_REGISTRY_W, 0, 0,
        location | CERT_STORE_OPEN_EXISTING_FLAG | CERT_STORE_READONLY_FLAG,
        name));
    if (!store.get()) {
      const DWORD error = GetLastError();
      // Optional group-policy/enterprise stores may not exist on this machine.
      if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ||
          error == ERROR_NO_SUCH_DOMAIN ||
          error == static_cast<DWORD>(CRYPT_E_NOT_FOUND)) {
        continue;
      }
      return false;
    }
    if (!CertAddStoreToCollection(collection, store.get(), 0, 0)) {
      return false;
    }
  }
  return true;
}

class TrustChangeMonitor : public base::win::ObjectWatcher::Delegate {
 public:
  TrustChangeMonitor(base::RepeatingCallback<bool(DWORD, HANDLE*)> control,
                     base::RepeatingCallback<void(bool)> on_change)
      : control_(std::move(control)), on_change_(std::move(on_change)) {
    event_.Set(CreateEvent(nullptr, TRUE, FALSE, nullptr));
    if (!event_.is_valid()) {
      on_change_.Run(false);
      return;
    }
    if (!control_) {
      constexpr std::array<const wchar_t*, 3> names = {L"ROOT", L"CA",
                                                       L"Disallowed"};
      for (size_t i = 0; i < stores_.size(); ++i) {
        stores_[i].reset(
            CertOpenStore(CERT_STORE_PROV_COLLECTION, 0, 0, 0, nullptr));
        if (!stores_[i].get() ||
            !AddRegistryStores(stores_[i].get(), names[i])) {
          on_change_.Run(false);
          return;
        }
      }
      control_ = base::BindRepeating(&TrustChangeMonitor::ControlStores,
                                     base::Unretained(this));
    }
    HANDLE event = event_.get();
    const bool ready = control_.Run(CERT_STORE_CTRL_NOTIFY_CHANGE, &event) &&
                       watcher_.StartWatchingOnce(event_.get(), this);
    // Register before taking the first snapshot, closing the startup gap.
    on_change_.Run(ready);
  }

  ~TrustChangeMonitor() override {
    // Cancel the wait (including queued callbacks) before closing the stores,
    // whose notification registrations must outlive the shared event.
    watcher_.StopWatching();
    for (auto& store : stores_) {
      store.reset();
    }
  }

  void OnObjectSignaled(HANDLE) override {
    // Reset before resync so a concurrent subsequent store change stays
    // signaled. Rearm on this sequence immediately, independently of snapshots
    // and of Chromium's lazy TrustStoreWin access.
    HANDLE event = event_.get();
    const bool ready = ResetEvent(event_.get()) &&
                       control_.Run(CERT_STORE_CTRL_RESYNC, &event) &&
                       watcher_.StartWatchingOnce(event_.get(), this);
    on_change_.Run(ready);
  }

 private:
  bool ControlStores(DWORD code, HANDLE* event) {
    for (const auto& store : stores_) {
      if (!CertControlStore(store.get(), 0, code, event)) {
        return false;
      }
    }
    return true;
  }

  base::win::ScopedHandle event_;
  std::array<crypto::ScopedHCERTSTORE, 3> stores_;
  base::win::ObjectWatcher watcher_;
  base::RepeatingCallback<bool(DWORD, HANDLE*)> control_;
  base::RepeatingCallback<void(bool)> on_change_;
};

base::OnceClosure StartMonitor(
    base::RepeatingCallback<bool(DWORD, HANDLE*)> control,
    base::RepeatingCallback<void(bool)> on_change) {
  base::SequenceBound<TrustChangeMonitor> monitor(
      base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskPriority::USER_VISIBLE}),
      std::move(control),
      base::BindPostTaskToCurrentDefault(std::move(on_change)));
  return base::BindOnce([](base::SequenceBound<TrustChangeMonitor>) {},
                        std::move(monitor));
}

}  // namespace

base::OnceClosure StartGmSystemTrustMonitoring(
    base::RepeatingCallback<void(bool)> on_change) {
  return StartMonitor({}, std::move(on_change));
}

base::OnceClosure StartGmSystemTrustMonitoringForTesting(
    base::RepeatingCallback<bool(DWORD, HANDLE*)> store_control,
    base::RepeatingCallback<void(bool)> on_change) {
  CHECK_IS_TEST();
  return StartMonitor(std::move(store_control), std::move(on_change));
}

network::mojom::GmAutoConfigPtr ReadGmSystemTrust() {
  {
    auto& replacement = SnapshotForTesting();
    base::AutoLock lock(replacement.lock);
    if (replacement.value) {
      CHECK_IS_TEST();
      return replacement.value.Clone();
    }
  }
  base::ScopedBlockingCall blocking(FROM_HERE, base::BlockingType::MAY_BLOCK);
  crypto::ScopedHCERTSTORE roots(
      CertOpenStore(CERT_STORE_PROV_COLLECTION, 0, 0, 0, nullptr));
  crypto::ScopedHCERTSTORE intermediates(
      CertOpenStore(CERT_STORE_PROV_COLLECTION, 0, 0, 0, nullptr));
  crypto::ScopedHCERTSTORE disallowed(
      CertOpenStore(CERT_STORE_PROV_COLLECTION, 0, 0, 0, nullptr));
  if (!roots.get() || !intermediates.get() || !disallowed.get() ||
      !AddRegistryStores(roots.get(), L"ROOT") ||
      !AddRegistryStores(intermediates.get(), L"CA") ||
      !AddRegistryStores(disallowed.get(), L"Disallowed")) {
    return EmptySnapshot();
  }
  return ReadStores(roots.get(), intermediates.get(), disallowed.get());
}

network::mojom::GmAutoConfigPtr ReadGmSystemTrustFromStoresForTesting(
    HCERTSTORE roots,
    HCERTSTORE intermediates,
    HCERTSTORE disallowed) {
  return ReadStores(roots, intermediates, disallowed);
}

void SetGmSystemTrustSnapshotForTesting(
    network::mojom::GmAutoConfigPtr snapshot) {
  CHECK_IS_TEST();
  {
    auto& replacement = SnapshotForTesting();
    base::AutoLock lock(replacement.lock);
    replacement.value = std::move(snapshot);
  }
  net::CertDatabase::GetInstance()->NotifyObserversTrustStoreChanged();
}

}  // namespace gm_browser
