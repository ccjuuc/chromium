// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_electron_navigation.h"

#include <cstdint>
#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/supports_user_data.h"
#include "base/task/sequenced_task_runner.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/weak_document_ptr.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "net/base/net_errors.h"

namespace xenon::ipc {
namespace {
const char kNavigationKey = 0;

void Reply(ElectronNavigationReply reply, int error, const GURL& url) {
  if (!reply) {
    return;
  }
  // Never run application callbacks while NavigationController is on stack.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(std::move(reply),
                     base::Value(base::DictValue()
                                     .Set("ok", error == net::OK)
                                     .Set("errorCode", error)
                                     .Set("errorDescription",
                                          net::ErrorToShortString(error))
                                     .Set("url", url.possibly_invalid_spec())),
                     std::string()));
}

class Navigation final : public base::SupportsUserData::Data,
                         public content::WebContentsObserver {
 public:
  explicit Navigation(content::WebContents* contents)
      : content::WebContentsObserver(contents) {}
  ~Navigation() override { Complete(net::ERR_ABORTED); }

  void Load(const content::NavigationController::LoadURLParams& params,
            ElectronNavigationReply reply) {
    Complete(net::ERR_ABORTED);
    if (!params.url.is_valid()) {
      Reply(std::move(reply), net::ERR_INVALID_URL, params.url);
      return;
    }
    pending_ = std::make_unique<Pending>(std::move(reply), params.url);
    const uint64_t serial = ++serial_;
    auto weak = weak_factory_.GetWeakPtr();
    auto handle = web_contents()->GetController().LoadURLWithParams(params);
    if (!weak || !pending_ || serial != serial_) {
      return;
    }
    // DidStartNavigation normally sets the ID synchronously. A rejected
    // controller request can instead return no handle and emit no events.
    if (!pending_->navigation_id) {
      if (handle) {
        pending_->navigation_id = handle->GetNavigationId();
      } else {
        Complete(net::ERR_ABORTED);
      }
    }
  }

 private:
  struct Pending {
    Pending(ElectronNavigationReply reply, GURL url)
        : reply(std::move(reply)), url(std::move(url)) {}
    ElectronNavigationReply reply;
    GURL url;
    int64_t navigation_id = 0;
    content::WeakDocumentPtr document;
  };

  void Complete(int error) {
    if (!pending_) {
      return;
    }
    auto pending = std::move(pending_);
    Reply(std::move(pending->reply), error, pending->url);
  }

  void DidStartNavigation(content::NavigationHandle* navigation) override {
    if (!pending_ || !navigation->IsInPrimaryMainFrame()) {
      return;
    }
    if (!pending_->navigation_id) {
      pending_->navigation_id = navigation->GetNavigationId();
    } else if (pending_->navigation_id != navigation->GetNavigationId()) {
      Complete(net::ERR_ABORTED);
    }
  }

  void DidFinishNavigation(content::NavigationHandle* navigation) override {
    if (!pending_ || navigation->GetNavigationId() != pending_->navigation_id) {
      return;
    }
    int error = navigation->GetNetErrorCode();
    if (error != net::OK || !navigation->HasCommitted() ||
        navigation->IsErrorPage()) {
      Complete(error == net::OK ? net::ERR_FAILED : error);
      return;
    }
    if (navigation->IsSameDocument()) {
      Complete(net::OK);
      return;
    }
    pending_->document = navigation->GetRenderFrameHost()->GetWeakDocumentPtr();
  }

  bool IsPendingDocument(content::RenderFrameHost* frame) const {
    return pending_ && frame == web_contents()->GetPrimaryMainFrame() &&
           pending_->document.AsRenderFrameHostIfValid() == frame;
  }

  void DidFinishLoad(content::RenderFrameHost* frame, const GURL&) override {
    if (IsPendingDocument(frame) && !frame->IsErrorDocument()) {
      Complete(net::OK);
    }
  }

  void DidFailLoad(content::RenderFrameHost* frame,
                   const GURL&,
                   int error) override {
    if (IsPendingDocument(frame)) {
      Complete(error);
    }
  }

  void PrimaryMainFrameRenderProcessGone(base::TerminationStatus) override {
    Complete(net::ERR_FAILED);
  }
  void WebContentsDestroyed() override { Complete(net::ERR_ABORTED); }

  uint64_t serial_ = 0;
  std::unique_ptr<Pending> pending_;
  base::WeakPtrFactory<Navigation> weak_factory_{this};
};
}  // namespace

void LoadElectronURL(content::WebContents* contents,
                     const content::NavigationController::LoadURLParams& params,
                     ElectronNavigationReply reply) {
  if (!contents || contents->IsBeingDestroyed()) {
    Reply(std::move(reply), net::ERR_ABORTED, params.url);
    return;
  }
  auto* navigation =
      static_cast<Navigation*>(contents->GetUserData(&kNavigationKey));
  if (!navigation) {
    auto owned = std::make_unique<Navigation>(contents);
    navigation = owned.get();
    contents->SetUserData(&kNavigationKey, std::move(owned));
  }
  navigation->Load(params, std::move(reply));
}
}  // namespace xenon::ipc
