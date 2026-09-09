// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_GUEST_H_
#define XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_GUEST_H_

#include <string>

#include "base/supports_user_data.h"
#include "base/values.h"
#include "content/public/browser/web_contents_delegate.h"
#include "content/public/browser/web_contents_observer.h"

namespace xenon::ipc {

// Owned by the inner WebContents; the embedding WebContents owns that guest.
// Removing the frame or destroying its owner also destroys the IPC identity.
class XenonElectronGuest final : public base::SupportsUserData::Data,
                                 public content::WebContentsObserver,
                                 public content::WebContentsDelegate {
 public:
  static int Attach(content::RenderFrameHost* frame,
                    const std::string& container_id,
                    base::DictValue preferences);
  static XenonElectronGuest* FromWebContents(content::WebContents* contents);
  static XenonElectronGuest* FromId(int id);
  ~XenonElectronGuest() override;

  int id() const { return id_; }
  const std::string& container_id() const { return container_id_; }
  const base::DictValue& preferences() const { return preferences_; }
  void LoadURL(const std::string& url);
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
  void DidStartLoading() override;
  void DidStopLoading() override;
  void DidFinishNavigation(content::NavigationHandle* navigation) override;
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
};

}  // namespace xenon::ipc
#endif
