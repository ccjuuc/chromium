// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_net_factory.h"

#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/network_service_instance.h"
#include "content/public/browser/storage_partition.h"
#include "mojo/public/cpp/bindings/callback_helpers.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "mojo/public/cpp/bindings/shared_remote.h"
#include "net/base/net_errors.h"
#include "services/network/public/cpp/constants.h"
#include "services/network/public/cpp/network_connection_tracker.h"
#include "services/network/public/cpp/url_loader_factory_builder.h"
#include "services/network/public/mojom/network_context.mojom.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_session_bridge.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_web_request.h"
#include "xenon_overlay/public/mojom/xenon_ipc.mojom.h"

namespace xenon::ipc {
namespace {
// Chromium intentionally excludes Set-Cookie from HttpResponseHeaders Mojo
// serialization. Observe the final trusted header callback, including changes
// made by webRequest, and acknowledge delivery before the network can respond.
class ResponseHeaderTap final : public network::mojom::TrustedHeaderClient {
 public:
  ResponseHeaderTap(
      mojo::PendingRemote<network::mojom::TrustedHeaderClient> downstream,
      mojo::SharedRemote<mojom::ElectronNetworkHeadersObserver> observer)
      : downstream_(std::move(downstream)), observer_(std::move(observer)) {}
  void OnBeforeSendHeaders(const GURL& url,
                           const net::HttpRequestHeaders& headers,
                           OnBeforeSendHeadersCallback callback) override {
    downstream_->OnBeforeSendHeaders(
        url, headers,
        mojo::WrapCallbackWithDefaultInvokeIfNotRun(
            std::move(callback), net::ERR_ABORTED,
            std::optional<net::HttpRequestHeaders>(),
            std::optional<base::DictValue>()));
  }
  void OnHeadersReceived(const std::string& headers,
                         const net::IPEndPoint& endpoint,
                         const std::optional<net::SSLInfo>& ssl,
                         OnHeadersReceivedCallback callback) override {
    downstream_->OnHeadersReceived(
        headers, endpoint, ssl,
        mojo::WrapCallbackWithDefaultInvokeIfNotRun(
            base::BindOnce(&ResponseHeaderTap::AfterInterception,
                           weak_.GetWeakPtr(), headers, std::move(callback)),
            net::ERR_ABORTED, std::optional<std::string>(),
            std::optional<GURL>()));
  }

 private:
  void AfterInterception(std::string original,
                         OnHeadersReceivedCallback callback,
                         int error,
                         const std::optional<std::string>& replacement,
                         const std::optional<GURL>& fragment) {
    if (error != net::OK) {
      std::move(callback).Run(error, replacement, fragment);
      return;
    }
    observer_->OnResponseHeaders(
        replacement.value_or(original),
        mojo::WrapCallbackWithDefaultInvokeIfNotRun(base::BindOnce(
            [](OnHeadersReceivedCallback callback,
               std::optional<std::string> replacement,
               std::optional<GURL> fragment) {
              std::move(callback).Run(net::OK, replacement, fragment);
            },
            std::move(callback), replacement, fragment)));
  }
  mojo::Remote<network::mojom::TrustedHeaderClient> downstream_;
  mojo::SharedRemote<mojom::ElectronNetworkHeadersObserver> observer_;
  base::WeakPtrFactory<ResponseHeaderTap> weak_{this};
};
class ResponseHeaderTapFactory final
    : public network::mojom::TrustedURLLoaderHeaderClient {
 public:
  ResponseHeaderTapFactory(
      mojo::PendingRemote<network::mojom::TrustedURLLoaderHeaderClient>
          downstream,
      mojo::PendingRemote<mojom::ElectronNetworkHeadersObserver> observer)
      : downstream_(std::move(downstream)), observer_(std::move(observer)) {}
  void OnLoaderCreated(
      int32_t id,
      mojo::PendingReceiver<network::mojom::TrustedHeaderClient> receiver)
      override {
    mojo::PendingRemote<network::mojom::TrustedHeaderClient> next;
    downstream_->OnLoaderCreated(id, next.InitWithNewPipeAndPassReceiver());
    mojo::MakeSelfOwnedReceiver(
        std::make_unique<ResponseHeaderTap>(std::move(next), observer_),
        std::move(receiver));
  }
  void OnLoaderForCorsPreflightCreated(
      const network::ResourceRequest& request,
      mojo::PendingReceiver<network::mojom::TrustedHeaderClient> receiver)
      override {
    // Application net requests use no-cors. If a future caller adds preflight,
    // its response metadata must not replace the application's response.
    downstream_->OnLoaderForCorsPreflightCreated(request, std::move(receiver));
  }

 private:
  mojo::Remote<network::mojom::TrustedURLLoaderHeaderClient> downstream_;
  mojo::SharedRemote<mojom::ElectronNetworkHeadersObserver> observer_;
};
}  // namespace
mojo::PendingRemote<network::mojom::URLLoaderFactory> CreateElectronNetFactory(
    content::BrowserContext* context,
    const std::string& container,
    const std::string& partition,
    std::string* error,
    mojo::PendingRemote<mojom::ElectronNetworkHeadersObserver> headers) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (container.empty() || partition.size() > 1024 ||
      partition.find('\0') != std::string::npos) {
    *error = "ERR_INVALID_ARG_VALUE: invalid Electron Session";
    return {};
  }
  auto* storage =
      ResolveElectronSessionPartition(context, container, partition);
  if (!storage) {
    *error = "ERR_ABORTED: Electron Session is unavailable";
    return {};
  }
  auto params = network::mojom::URLLoaderFactoryParams::New();
  params->process_id = network::OriginatingProcessId::browser();
  params->is_trusted = true;
  params->automatically_assign_isolation_info = true;
  params->is_orb_enabled = false;
  params->network_restrictions_id = network::GetNoOpNetworkRestrictionsId();
  network::URLLoaderFactoryBuilder builder;
  if (!AppendElectronWebRequestProxy(context, storage, builder, true,
                                     &params->header_client)) {
    *error = "ERR_ABORTED: Electron Session interception is unavailable";
    return {};
  }
  if (headers) {
    auto downstream = std::move(params->header_client);
    mojo::MakeSelfOwnedReceiver(
        std::make_unique<ResponseHeaderTapFactory>(std::move(downstream),
                                                   std::move(headers)),
        params->header_client.InitWithNewPipeAndPassReceiver());
  }
  // A fresh factory is necessary: a clone of the shared browser factory has
  // no trusted header channel and would silently bypass the Session's hooks.
  return std::move(builder)
      .Finish<mojo::PendingRemote<network::mojom::URLLoaderFactory>>(
          storage->GetNetworkContext(), std::move(params));
}
bool IsElectronNetworkOnline() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  return !content::GetNetworkConnectionTracker()->IsOffline();
}
}  // namespace xenon::ipc
