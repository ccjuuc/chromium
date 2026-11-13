// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
#ifndef XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_NET_OBSERVER_H_
#define XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_NET_OBSERVER_H_

#include "base/functional/callback.h"
#include "base/values.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "services/network/public/mojom/url_loader_network_service_observer.mojom.h"

namespace xenon::ipc {
// One trusted observer per application request. Authentication never falls
// through to the browser's interactive login UI or another application's
// Session. Certificate verification remains strict.
class ElectronNetObserver final
    : public network::mojom::URLLoaderNetworkServiceObserver {
 public:
  explicit ElectronNetObserver(
      base::RepeatingCallback<void(base::DictValue)> event);
  ~ElectronNetObserver() override;
  mojo::PendingRemote<network::mojom::URLLoaderNetworkServiceObserver> Bind();
  bool Reply(uint64_t challenge,
             const std::optional<std::string>& username,
             const std::optional<std::string>& password);
  void OnSSLCertificateError(const GURL&,
                             int error,
                             const net::SSLInfo&,
                             bool,
                             OnSSLCertificateErrorCallback callback) override;
  void OnCertificateRequested(
      const std::optional<base::UnguessableToken>&,
      const scoped_refptr<net::SSLCertRequestInfo>&,
      mojo::PendingRemote<network::mojom::ClientCertificateResponder>) override;
  void OnGmCertificateRequested(
      const std::optional<base::UnguessableToken>&,
      const url::Origin&,
      const std::string&,
      uint64_t,
      network::mojom::GmClientCertificateRequestPtr,
      mojo::PendingRemote<network::mojom::GmClientCertificateResponder>)
      override;
  void OnAuthRequired(
      const std::optional<base::UnguessableToken>&,
      int32_t,
      const GURL&,
      bool,
      const net::AuthChallengeInfo&,
      const scoped_refptr<net::HttpResponseHeaders>&,
      mojo::PendingRemote<network::mojom::AuthChallengeResponder>) override;
  void OnLocalNetworkAccessPermissionRequired(
      network::mojom::TransportType,
      network::mojom::IPAddressSpace,
      OnLocalNetworkAccessPermissionRequiredCallback callback) override;
  void OnPlatformLocalNetworkPermissionRequired(
      OnPlatformLocalNetworkPermissionRequiredCallback callback) override;
  void OnClearSiteData(const GURL&,
                       const std::string&,
                       int32_t,
                       const std::optional<net::CookiePartitionKey>&,
                       bool,
                       OnClearSiteDataCallback callback) override;
  void OnLoadingStateUpdate(network::mojom::LoadInfoPtr,
                            OnLoadingStateUpdateCallback callback) override;
  void OnDataUseUpdate(int32_t, base::ByteSize, base::ByteSize) override;
  void Clone(
      mojo::PendingReceiver<network::mojom::URLLoaderNetworkServiceObserver>)
      override;
  void OnWebSocketConnectedToLocalNetwork(
      const GURL&,
      network::mojom::IPAddressSpace) override;
  void OnUrlLoaderConnectedToLocalNetwork(
      const GURL&,
      network::mojom::IPAddressSpace,
      network::mojom::IPAddressSpace,
      network::mojom::IPAddressSpace) override;

 private:
  base::RepeatingCallback<void(base::DictValue)> event_;
  mojo::ReceiverSet<network::mojom::URLLoaderNetworkServiceObserver> receivers_;
  mojo::Remote<network::mojom::AuthChallengeResponder> pending_;
  uint64_t next_challenge_ = 0;
};
}  // namespace xenon::ipc
#endif
