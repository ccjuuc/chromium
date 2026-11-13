// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_electron_document_registration.h"

#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "content/public/browser/document_user_data.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/weak_document_ptr.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"

namespace xenon::ipc {
namespace {
using Reply = base::OnceCallback<void(std::string)>;

void PostReply(Reply callback,
               content::WeakDocumentPtr document,
               std::string endpoint_id,
               base::RepeatingCallback<bool()> still_registered = {}) {
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(
                     [](Reply callback, content::WeakDocumentPtr document,
                        std::string endpoint_id,
                        base::RepeatingCallback<bool()> still_registered) {
                       auto* frame = document.AsRenderFrameHostIfValid();
                       if (!frame || !frame->IsActive() ||
                           !frame->IsRenderFrameLive() ||
                           (!endpoint_id.empty() &&
                            (!still_registered || !still_registered.Run()))) {
                         endpoint_id.clear();
                       }
                       std::move(callback).Run(std::move(endpoint_id));
                     },
                     std::move(callback), document, std::move(endpoint_id),
                     std::move(still_registered)));
}

class Registration final : public content::DocumentUserData<Registration>,
                           public content::WebContentsObserver {
 public:
  ~Registration() override { Complete(false); }

  base::OnceCallback<void(bool)> Begin(
      const std::string& endpoint_id,
      base::RepeatingCallback<bool()> owner_is_current) {
    Complete(false);
    weak_factory_.InvalidateWeakPtrs();
    endpoint_id_ = endpoint_id;
    owner_is_current_ = std::move(owner_is_current);
    status_ = Status::kPending;
    return base::BindOnce(&Registration::Complete, weak_factory_.GetWeakPtr());
  }

  void Revoke(const std::string& endpoint_id) {
    if (endpoint_id_ != endpoint_id) {
      return;
    }
    weak_factory_.InvalidateWeakPtrs();
    Complete(false);
  }

  void Wait(Reply callback) {
    auto* frame = document_.AsRenderFrameHostIfValid();
    if (!frame || !frame->IsActive() || !frame->IsRenderFrameLive() ||
        status_ == Status::kFailed) {
      PostReply(std::move(callback), document_, {});
      return;
    }
    if (status_ == Status::kReady) {
      PostReply(std::move(callback), document_, endpoint_id_, IsRegistered());
      return;
    }
    if (waiters_.size() >= 1024) {
      PostReply(std::move(callback), document_, {});
      return;
    }
    waiters_.push_back(std::move(callback));
    if (!deadline_.IsRunning()) {
      deadline_.Start(FROM_HERE, base::Seconds(30), this,
                      &Registration::CancelWaiters);
    }
  }

 private:
  friend content::DocumentUserData<Registration>;
  explicit Registration(content::RenderFrameHost* frame)
      : content::DocumentUserData<Registration>(frame),
        content::WebContentsObserver(
            content::WebContents::FromRenderFrameHost(frame)),
        document_(frame->GetWeakDocumentPtr()) {}
  DOCUMENT_USER_DATA_KEY_DECL();

  enum class Status { kPending, kReady, kFailed };

  base::RepeatingCallback<bool()> IsRegistered() {
    return base::BindRepeating(
        [](base::WeakPtr<Registration> self, const std::string& endpoint_id) {
          return self && self->status_ == Status::kReady &&
                 self->endpoint_id_ == endpoint_id &&
                 (!self->owner_is_current_ || self->owner_is_current_.Run());
        },
        weak_factory_.GetWeakPtr(), endpoint_id_);
  }

  void Complete(bool registered) {
    status_ = registered ? Status::kReady : Status::kFailed;
    deadline_.Stop();
    auto waiters = std::move(waiters_);
    for (auto& callback : waiters) {
      PostReply(std::move(callback), document_, registered ? endpoint_id_ : "",
                IsRegistered());
    }
  }

  void CancelWaiters() {
    deadline_.Stop();
    auto waiters = std::move(waiters_);
    for (auto& callback : waiters) {
      PostReply(std::move(callback), document_, {});
    }
  }

  void RenderFrameDeleted(content::RenderFrameHost* frame) override {
    if (frame == &render_frame_host()) {
      Revoke(endpoint_id_);
    }
  }

  void RenderFrameHostStateChanged(
      content::RenderFrameHost* frame,
      content::RenderFrameHost::LifecycleState,
      content::RenderFrameHost::LifecycleState new_state) override {
    if (frame == &render_frame_host() &&
        new_state != content::RenderFrameHost::LifecycleState::kActive) {
      // A BFCache document retains its registration for a future restore, but
      // callers waiting for its old active page must be cancelled immediately.
      CancelWaiters();
    }
  }

  void WebContentsDestroyed() override { Revoke(endpoint_id_); }

  const content::WeakDocumentPtr document_;
  std::string endpoint_id_;
  base::RepeatingCallback<bool()> owner_is_current_;
  Status status_ = Status::kFailed;
  std::vector<Reply> waiters_;
  base::OneShotTimer deadline_;
  base::WeakPtrFactory<Registration> weak_factory_{this};
};
DOCUMENT_USER_DATA_KEY_IMPL(Registration);
}  // namespace

base::OnceCallback<void(bool)> BeginElectronDocumentRegistration(
    content::RenderFrameHost* frame,
    const std::string& endpoint_id,
    base::RepeatingCallback<bool()> owner_is_current) {
  return Registration::GetOrCreateForCurrentDocument(frame)->Begin(
      endpoint_id, std::move(owner_is_current));
}

void RevokeElectronDocumentRegistration(content::RenderFrameHost* frame,
                                        const std::string& endpoint_id) {
  if (frame) {
    if (auto* registration = Registration::GetForCurrentDocument(frame)) {
      registration->Revoke(endpoint_id);
    }
  }
}

void WaitForElectronDocumentRegistration(
    content::RenderFrameHost* frame,
    base::OnceCallback<void(std::string)> callback) {
  auto* registration =
      frame ? Registration::GetForCurrentDocument(frame) : nullptr;
  if (!registration) {
    PostReply(std::move(callback), {}, {});
    return;
  }
  registration->Wait(std::move(callback));
}

}  // namespace xenon::ipc
