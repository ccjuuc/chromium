// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_GM_GM_KEY_MANAGER_H_
#define CHROME_BROWSER_GM_GM_KEY_MANAGER_H_

#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/timer/timer.h"
#include "chrome/browser/gm/gm_pin_dialog.h"
#include "components/prefs/pref_change_registrar.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "net/gm/gm_client_identity.h"
#include "services/network/public/mojom/gm_identity.mojom.h"
#include "url/origin.h"

class PrefService;
namespace content {
class WebContents;
}

namespace gm_browser {

// One manager per Profile (including separate incognito profiles). Only this
// browser object holds the KeyService control pipe. NetworkContexts get a
// broker and then an origin-authorized signing capability, never the control.
class KeyManager : public network::mojom::GmIdentityBroker,
                   public gm_key::mojom::KeyObserver {
 public:
  using ServiceFactory =
      base::RepeatingCallback<mojo::PendingRemote<gm_key::mojom::KeyService>()>;
  using Prompt = base::RepeatingCallback<base::WeakPtr<views::Widget>(
      const std::string&,
      const std::vector<std::u16string>&,
      const std::u16string&,
      PinCallback)>;

  explicit KeyManager(PrefService* local_state);
  ~KeyManager() override;
  mojo::PendingRemote<network::mojom::GmIdentityBroker> Bind();
  // Injectable only through C++ tests, never through a production switch/pref.
  void SetTestHooks(ServiceFactory factory, Prompt prompt);
  content::WebContents* active_web_contents_for_testing() const;

  // Called only through the browser's verified URLLoader observer context.
  // The returned closure cancels this request without affecting other pages.
  base::OnceClosure AcquireIdentityForWebContents(
      content::WebContents* web_contents,
      const url::Origin& origin,
      const std::string& identity_policy_id,
      uint64_t identity_generation,
      network::mojom::GmClientCertificateRequestPtr certificate_request,
      AcquireIdentityCallback callback);

  void AcquireIdentity(
      uint64_t request_id,
      const url::Origin& origin,
      const std::string& identity_policy_id,
      uint64_t identity_generation,
      network::mojom::GmClientCertificateRequestPtr certificate_request,
      AcquireIdentityCallback callback) override;
  void Cancel(uint64_t request_id) override;
  void OnIdentitiesChanged() override;
  void OnServiceFailure(gm_key::mojom::KeyError error) override;

 private:
  struct BrokerContext {
    uint64_t last_request_id = 0;
  };
  struct Request {
    std::optional<mojo::ReceiverId> broker_id;
    uint64_t request_id;
    url::Origin origin;
    std::string policy_id;
    uint64_t generation;
    std::string certificate_sha256;
    net::gm::ClientCertificateRequest certificate_request;
    AcquireIdentityCallback callback;
    base::WeakPtr<content::WebContents> web_contents;
  };
  void QueueRequest(std::optional<mojo::ReceiverId> broker_id,
                    uint64_t request_id,
                    content::WebContents* web_contents,
                    const url::Origin& origin,
                    const std::string& identity_policy_id,
                    uint64_t identity_generation,
                    network::mojom::GmClientCertificateRequestPtr constraints,
                    AcquireIdentityCallback callback);
  void CancelRequest(std::optional<mojo::ReceiverId> broker_id,
                     uint64_t request_id);
  void Process();
  bool Allowed(const Request& request) const;
  bool MatchesRequest(const gm_key::mojom::KeyIdentity& identity) const;
  void OnConfigured(gm_key::mojom::KeyResultPtr result);
  void OnEnumerated(gm_key::mojom::KeyResultPtr result,
                    std::vector<gm_key::mojom::KeyIdentityPtr> identities);
  void OnCertificatesCompleted(
      std::vector<gm_key::mojom::KeyIdentityPtr> identities);
  void PromptForPin(const std::u16string& message);
  void OnPin(size_t selected, std::unique_ptr<gm_key::SecurePin> pin);
  void OnGranted(bool from_pin,
                 gm_key::mojom::KeyResultPtr result,
                 gm_key::mojom::KeyIdentityPtr identity);
  void Finish(int error, network::mojom::GmIdentityGrantPtr grant = nullptr);
  void StopActiveRequest();
  void ScheduleNextRequest();
  void OnBrokerDisconnected();
  void Reset(int error);
  void OnPolicyChanged();
  std::string RequestKey() const;

  raw_ptr<PrefService> prefs_;
  PrefChangeRegistrar registrar_;
  mojo::ReceiverSet<network::mojom::GmIdentityBroker, BrokerContext> brokers_;
  mojo::Remote<gm_key::mojom::KeyService> service_;
  mojo::Receiver<gm_key::mojom::KeyObserver> observer_{this};
  bool configured_ = false;
  bool configuring_ = false;
  bool processing_ = false;
  bool test_hooks_ = false;
  uint64_t next_browser_request_id_ = 0;
  uint64_t next_authorization_id_ = 0;
  uint64_t active_authorization_id_ = 0;
  std::deque<Request> requests_;
  std::vector<gm_key::mojom::KeyIdentityPtr> candidates_;
  gm_key::mojom::KeyIdentityPtr selected_identity_;
  std::map<std::string, std::pair<std::string, uint64_t>> selected_;
  base::WeakPtr<views::Widget> dialog_;
  mojo::PendingRemote<gm_key::mojom::KeySigner> pending_signer_;
  base::OneShotTimer prompt_timeout_;
  ServiceFactory service_factory_;
  Prompt prompt_;
  base::WeakPtrFactory<KeyManager> weak_factory_{this};
  // Invalidated whenever the active request ends; service startup is separate
  // so cancelling one request neither abandons Configure nor other grants.
  base::WeakPtrFactory<KeyManager> operation_weak_factory_{this};
};

}  // namespace gm_browser
#endif  // CHROME_BROWSER_GM_GM_KEY_MANAGER_H_
