// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_REMOTE_REMOTE_INPUT_ROUTER_H_
#define XENON_OVERLAY_CHROME_BROWSER_REMOTE_REMOTE_INPUT_ROUTER_H_

#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "build/build_config.h"
#include "content/public/browser/desktop_media_id.h"
#include "content/public/browser/devtools_agent_host_client.h"
#include "content/public/browser/web_contents_observer.h"

namespace content {
class DevToolsAgentHost;
}
namespace remoting {
class InputInjector;
namespace protocol {
class InputEventTracker;
}
}  // namespace remoting

namespace xenon {

// Routes a bounded input vocabulary to a browser-selected capture target.
// Call only on the browser UI sequence. Setting a target never grants control;
// the local sharing UI must explicitly enable it for the current generation.
class RemoteInputRouter : private content::DevToolsAgentHostClient,
                          private content::WebContentsObserver {
 public:
  RemoteInputRouter();
  ~RemoteInputRouter() override;

  RemoteInputRouter(const RemoteInputRouter&) = delete;
  RemoteInputRouter& operator=(const RemoteInputRouter&) = delete;

  void SetTarget(const content::DesktopMediaID& target, int generation);
  bool SetEnabled(bool enabled);
  bool HandleInput(const base::DictValue& event);
  void ReleaseAll();
  void Reset();

  // Reports an internal loss of control on the next UI task, after held input
  // is released. Explicit SetEnabled/Reset/SetTarget calls cancel an
  // outstanding notification and never notify. The callback may reset or
  // destroy this.
  void SetControlRevokedCallback(base::RepeatingClosure callback);

  // Reports platform support; existence/eligibility is checked by SetEnabled.
  static bool SupportsControl(const content::DesktopMediaID& target);

 private:
  friend class RemoteInputRouterTestPeer;

  bool IsTargetAvailable() const;
  bool ValidateInput(const base::DictValue& event) const;
  bool HandleSystemInput(const base::DictValue& event);
  void CheckSystemTarget();
  void RevokeControl(bool reset_target = false);
  void NotifyControlRevoked();
  void PumpPageInput();
  void SendPageInput(const base::DictValue& event,
                     double viewport_width,
                     double viewport_height);
  void SendPageCommand(int id, std::string method, base::DictValue params);
  void DetachPage();

  // content::DevToolsAgentHostClient:
  void DispatchProtocolMessage(content::DevToolsAgentHost* agent_host,
                               base::span<const uint8_t> message) override;
  void AgentHostClosed(content::DevToolsAgentHost* agent_host) override;
  bool MayAttachToURL(const GURL& url, bool is_webui) override;
  bool MayAttachToRenderFrameHost(content::RenderFrameHost* frame) override;
  bool IsTrusted() override;
  bool MayAccessAllCookies() override;
  bool MayReadLocalFiles() override;
  bool MayWriteLocalFiles() override;
  bool AllowUnsafeOperations() override;

  // content::WebContentsObserver:
  void PrimaryPageChanged(content::Page& page) override;
  void WebContentsDestroyed() override;

  content::DesktopMediaID target_;
  int generation_ = 0;
  bool enabled_ = false;
  bool control_revocation_pending_ = false;
  base::RepeatingClosure control_revoked_callback_;
  scoped_refptr<content::DevToolsAgentHost> agent_host_;
  int next_command_id_ = 0;
  int pending_metrics_id_ = 0;
  int pending_input_id_ = 0;
  std::deque<base::DictValue> page_input_queue_;
  std::map<std::string, std::string> pressed_page_keys_;
  int pressed_page_buttons_ = 0;
  double last_page_x_ = 0;
  double last_page_y_ = 0;

#if BUILDFLAG(IS_WIN)
  std::optional<std::wstring> screen_device_key_;
  std::unique_ptr<remoting::InputInjector> system_injector_;
  std::unique_ptr<remoting::protocol::InputEventTracker> system_tracker_;
  base::RepeatingTimer system_target_timer_;
#endif
  base::WeakPtrFactory<RemoteInputRouter> weak_factory_{this};
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_REMOTE_REMOTE_INPUT_ROUTER_H_
