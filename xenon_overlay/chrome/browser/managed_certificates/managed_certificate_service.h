// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_MANAGED_CERTIFICATES_MANAGED_CERTIFICATE_SERVICE_H_
#define XENON_OVERLAY_CHROME_BROWSER_MANAGED_CERTIFICATES_MANAGED_CERTIFICATE_SERVICE_H_

#include <memory>
#include <optional>
#include <string>

#include "base/callback_list.h"
#include "base/files/file_path.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/sequence_checker.h"
#include "base/task/sequenced_task_runner.h"
#include "base/timer/timer.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "net/base/network_change_notifier.h"
#include "services/network/public/mojom/network_context.mojom.h"
#include "services/network/public/mojom/url_loader_factory.mojom.h"
#include "xenon_overlay/chrome/browser/managed_certificates/managed_certificate_types.h"

namespace network {
class SimpleURLLoader;
}

namespace xenon::certificates {

struct ServiceReadResult;
struct ServiceWriteResult {
  bool success = false;
  PersistentState state;
  std::string error;
};

// Safe diagnostic fields; transport addresses, local paths, and credentials
// deliberately stay private to the service.
struct ServiceStatus {
  bool started = false;
  bool enabled = false;
  bool ready = true;
  bool downloading = false;
  std::string source_id;
  std::string configuration_error;
  std::string refresh_error;
  base::Time last_attempt;
  base::Time last_success;
};

// UI-sequence owner of the browser-process managed trust snapshot.
// The distribution connection deliberately has its own trust configuration.
class XenonManagedCertificateService
    : public net::NetworkChangeNotifier::NetworkChangeObserver {
 public:
  static XenonManagedCertificateService* Get();

  // Installed before Start(). The factory supplies transport/proxy settings;
  // this service always replaces its verifier with independent bootstrap trust.
  void SetNetworkContextParamsFactory(
      base::RepeatingCallback<network::mojom::NetworkContextParamsPtr()>
          factory);
  void Start(const base::FilePath& config_path,
             const base::FilePath& user_data_dir);
  void Shutdown();
  bool ready() const;
  ServiceStatus GetStatus() const;
  const Snapshot* snapshot() const;
  base::CallbackListSubscription AddObserver(base::RepeatingClosure observer);

 private:
  friend class base::NoDestructor<XenonManagedCertificateService>;
  friend class ManagedCertificateServiceTest;

  XenonManagedCertificateService();
  ~XenonManagedCertificateService() override;

  static std::unique_ptr<ServiceReadResult> ReadInputs(
      const base::FilePath& config_path,
      const base::FilePath& user_data_dir);
  static std::unique_ptr<ServiceWriteResult> ValidateAndSave(
      const base::FilePath& path,
      PersistentState state,
      std::string body,
      std::string etag,
      std::string request_context);

  void OnRead(uint64_t epoch, std::unique_ptr<ServiceReadResult> result);
  void Fetch();
  bool EnsureNetworkContext();
  void OnDownloaded(uint64_t epoch, std::optional<std::string> body);
  void OnSaved(uint64_t epoch, std::unique_ptr<ServiceWriteResult> result);
  void OnNetworkDisconnected(uint64_t epoch);
  void OnNetworkChanged(
      net::NetworkChangeNotifier::ConnectionType type) override;
  void ScheduleRefresh(
      bool success,
      std::optional<base::TimeDelta> retry_after = std::nullopt);
  void Publish();
  void CancelNetwork();
  void FinishIO();

  SEQUENCE_CHECKER(sequence_checker_);
  bool started_ = false;
  bool ready_ = true;
  bool io_busy_ = false;
  bool fetch_pending_ = false;
  bool state_usable_ = false;
  bool request_sent_conditional_ = false;
  uint64_t epoch_ = 0;
  unsigned failures_ = 0;
  base::FilePath user_data_dir_;
  std::optional<Config> config_;
  PersistentState state_;
  std::string bearer_token_;
  std::string request_context_;
  std::string configuration_error_;
  std::string refresh_error_;
  base::Time last_attempt_;
  base::Time last_success_;
  std::optional<Snapshot> published_;
  scoped_refptr<base::SequencedTaskRunner> io_runner_;
  base::RepeatingClosureList observers_;
  base::OneShotTimer refresh_timer_;
  mojo::Remote<network::mojom::NetworkContext> network_context_;
  mojo::Remote<network::mojom::URLLoaderFactory> factory_;
  base::RepeatingCallback<network::mojom::NetworkContextParamsPtr()>
      network_params_factory_;
  std::unique_ptr<network::SimpleURLLoader> loader_;
  raw_ptr<network::mojom::URLLoaderFactory> test_factory_ = nullptr;
  base::WeakPtrFactory<XenonManagedCertificateService> weak_factory_{this};
};

}  // namespace xenon::certificates

#endif  // XENON_OVERLAY_CHROME_BROWSER_MANAGED_CERTIFICATES_MANAGED_CERTIFICATE_SERVICE_H_
