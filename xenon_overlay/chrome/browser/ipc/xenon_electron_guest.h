// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_GUEST_H_
#define XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_GUEST_H_

#include <memory>
#include <string>

#include "base/functional/callback.h"
#include "base/supports_user_data.h"
#include "base/values.h"
#include "content/public/browser/web_contents_delegate.h"
#include "content/public/browser/web_contents_observer.h"

namespace content {
class BrowserContext;
}

namespace xenon::ipc {

// Metadata owned by a WebContents. DOM guests are owned by their embedder;
// standalone WebContentsView surfaces retain explicit owner-managed lifetime.
class XenonElectronGuest final : public base::SupportsUserData::Data,
                                 public content::WebContentsObserver,
                                 public content::WebContentsDelegate {
 public:
  static int Attach(content::RenderFrameHost* frame,
                    const std::string& container_id,
                    base::DictValue preferences);
  static std::unique_ptr<content::WebContents> CreateStandalone(
      content::BrowserContext* context,
      const std::string& container_id,
      base::DictValue preferences);
  static XenonElectronGuest* FromWebContents(content::WebContents* contents);
  static XenonElectronGuest* FromId(int id);
  ~XenonElectronGuest() override;

  int id() const { return id_; }
  const std::string& container_id() const { return container_id_; }
  const base::DictValue& preferences() const { return preferences_; }
  void LoadURL(const std::string& url,
               base::OnceCallback<void(base::Value, std::string)> reply = {});
  void SetCloseCallback(base::RepeatingClosure callback);
  bool Call(const std::string& command,
            const base::Value& arguments,
            base::Value* result,
            std::string* error);

 private:
  XenonElectronGuest(content::WebContents* contents,
                     const std::string& container_id,
                     base::DictValue preferences);
  void Notify(const std::string& name, base::Value details = base::Value());
  void DOMContentLoaded(content::RenderFrameHost* frame) override;
  void DidFinishLoad(content::RenderFrameHost* frame, const GURL& url) override;
  void DidFailLoad(content::RenderFrameHost* frame,
                   const GURL& url,
                   int error) override;
  void DidStartLoading() override;
  void DidStopLoading() override;
  void DidFinishNavigation(content::NavigationHandle* navigation) override;
  void ReadyToCommitNavigation(content::NavigationHandle* navigation) override;
  void RenderFrameCreated(content::RenderFrameHost* frame) override;
  void CloseContents(content::WebContents* source) override;
  void RequestMediaAccessPermission(
      content::WebContents* contents,
      const content::MediaStreamRequest& request,
      content::MediaResponseCallback callback) override;
  bool CheckMediaAccessPermission(content::RenderFrameHost* frame,
                                  const url::Origin& origin,
                                  blink::mojom::MediaStreamType type) override;
  using content::WebContentsObserver::BeforeUnloadFired;
  void BeforeUnloadFired(content::WebContents* source,
                         bool proceed,
                         bool* proceed_to_fire_unload) override;
  void TitleWasSet(content::NavigationEntry* entry) override;
  void WebContentsDestroyed() override;
  bool IsWebContentsCreationOverridden(content::RenderFrameHost* opener,
                                       content::SiteInstance* source,
                                       content::mojom::WindowContainerType type,
                                       const GURL& opener_url,
                                       const std::string& frame_name,
                                       const GURL& target_url) override;

  const int id_;
  const std::string container_id_;
  const base::DictValue preferences_;
  bool observes_before_input_ = false;
  base::RepeatingClosure close_callback_;
};

}  // namespace xenon::ipc
#endif
