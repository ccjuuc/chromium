// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_net_observer.h"

#include "base/strings/utf_string_conversions.h"
#include "net/base/auth.h"
#include "net/base/net_errors.h"
#include "services/network/public/mojom/gm_identity.mojom.h"

namespace xenon::ipc {
ElectronNetObserver::ElectronNetObserver(
    base::RepeatingCallback<void(base::DictValue)> event)
    : event_(std::move(event)) {}
ElectronNetObserver::~ElectronNetObserver() {
  if (pending_) {
    pending_->OnAuthCredentials(std::nullopt);
  }
}
mojo::PendingRemote<network::mojom::URLLoaderNetworkServiceObserver>
ElectronNetObserver::Bind() {
  mojo::PendingRemote<network::mojom::URLLoaderNetworkServiceObserver> remote;
  receivers_.Add(this, remote.InitWithNewPipeAndPassReceiver());
  return remote;
}
bool ElectronNetObserver::Reply(uint64_t challenge,
                                const std::optional<std::string>& username,
                                const std::optional<std::string>& password) {
  if (!pending_ || challenge != next_challenge_) {
    return false;
  }
  std::optional<net::AuthCredentials> credentials;
  if (username && password) {
    credentials.emplace(base::UTF8ToUTF16(*username),
                        base::UTF8ToUTF16(*password));
  }
  pending_->OnAuthCredentials(credentials);
  pending_.reset();
  return true;
}
void ElectronNetObserver::OnAuthRequired(
    const std::optional<base::UnguessableToken>&,
    int32_t,
    const GURL&,
    bool,
    const net::AuthChallengeInfo& info,
    const scoped_refptr<net::HttpResponseHeaders>&,
    mojo::PendingRemote<network::mojom::AuthChallengeResponder> responder) {
  if (pending_) {
    pending_->OnAuthCredentials(std::nullopt);
  }
  pending_.reset();
  pending_.Bind(std::move(responder));
  // A stale callback cannot answer a later challenge on this request.
  ++next_challenge_;
  base::DictValue auth;
  auth.Set("isProxy", info.is_proxy);
  auth.Set("scheme", info.scheme);
  auth.Set("host", info.challenger.host());
  auth.Set("port", static_cast<int>(info.challenger.port()));
  auth.Set("realm", info.realm);
  event_.Run(base::DictValue()
                 .Set("type", "login")
                 .Set("challengeId", static_cast<double>(next_challenge_))
                 .Set("authInfo", std::move(auth)));
}
void ElectronNetObserver::OnSSLCertificateError(
    const GURL&,
    int error,
    const net::SSLInfo&,
    bool,
    OnSSLCertificateErrorCallback callback) {
  std::move(callback).Run(error);
}
void ElectronNetObserver::OnCertificateRequested(
    const std::optional<base::UnguessableToken>&,
    const scoped_refptr<net::SSLCertRequestInfo>&,
    mojo::PendingRemote<network::mojom::ClientCertificateResponder> responder) {
  mojo::Remote<network::mojom::ClientCertificateResponder> remote(
      std::move(responder));
  remote->ContinueWithoutCertificate();
}
void ElectronNetObserver::OnGmCertificateRequested(
    const std::optional<base::UnguessableToken>&,
    const url::Origin&,
    const std::string&,
    uint64_t,
    network::mojom::GmClientCertificateRequestPtr,
    mojo::PendingRemote<network::mojom::GmClientCertificateResponder>
        responder) {
  mojo::Remote<network::mojom::GmClientCertificateResponder> remote(
      std::move(responder));
  remote->Continue(net::ERR_SSL_CLIENT_AUTH_PRIVATE_KEY_ACCESS_DENIED, nullptr);
}
void ElectronNetObserver::OnLocalNetworkAccessPermissionRequired(
    network::mojom::TransportType,
    network::mojom::IPAddressSpace,
    OnLocalNetworkAccessPermissionRequiredCallback callback) {
  std::move(callback).Run(network::mojom::LocalNetworkAccessResult::kDenied);
}
void ElectronNetObserver::OnPlatformLocalNetworkPermissionRequired(
    OnPlatformLocalNetworkPermissionRequiredCallback callback) {
  std::move(callback).Run(false);
}
void ElectronNetObserver::OnClearSiteData(
    const GURL&,
    const std::string&,
    int32_t,
    const std::optional<net::CookiePartitionKey>&,
    bool,
    OnClearSiteDataCallback callback) {
  std::move(callback).Run();
}
void ElectronNetObserver::OnLoadingStateUpdate(
    network::mojom::LoadInfoPtr,
    OnLoadingStateUpdateCallback callback) {
  std::move(callback).Run();
}
void ElectronNetObserver::OnDataUseUpdate(int32_t,
                                          base::ByteSize,
                                          base::ByteSize) {}
void ElectronNetObserver::Clone(
    mojo::PendingReceiver<network::mojom::URLLoaderNetworkServiceObserver>
        receiver) {
  receivers_.Add(this, std::move(receiver));
}
void ElectronNetObserver::OnWebSocketConnectedToLocalNetwork(
    const GURL&,
    network::mojom::IPAddressSpace) {}
void ElectronNetObserver::OnUrlLoaderConnectedToLocalNetwork(
    const GURL&,
    network::mojom::IPAddressSpace,
    network::mojom::IPAddressSpace,
    network::mojom::IPAddressSpace) {}
}  // namespace xenon::ipc
