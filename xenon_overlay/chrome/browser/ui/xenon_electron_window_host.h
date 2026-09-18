// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_UI_XENON_ELECTRON_WINDOW_HOST_H_
#define XENON_OVERLAY_CHROME_BROWSER_UI_XENON_ELECTRON_WINDOW_HOST_H_

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/no_destructor.h"
#include "base/values.h"
#include "ui/gfx/geometry/point.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/views/widget/widget_observer.h"

namespace content {
class BrowserContext;
class WebContents;
}

namespace views {
class Widget;
}

namespace xenon {

// Browser-process backing for Electron `BrowserWindow`.
// Every window is created with its final WebDialog/HWND synchronously. The
// about:blank web surface stays hidden until the first loadURL/loadFile, so a
// BrowserWindow that never navigates remains usable as a native content host
// without product- or container-specific detection.
class XenonElectronWindowHost : public views::WidgetObserver {
 public:
  static XenonElectronWindowHost* GetInstance();

  bool CreateHostedWindow(content::BrowserContext* context,
                    int width,
                    int height,
                    bool show,
                    bool frame,
                    bool transparent,
                    int32_t parent_id,
                    const std::string& title,
                    const std::string& container_id,
                    int32_t* window_id,
                    uint64_t* hwnd,
                    std::string* error);
  void LoadURL(int32_t window_id, const std::string& url);
  void SetVisible(int32_t window_id, bool visible);
  bool Call(int32_t window_id,
            const std::string& command,
            const base::Value& arguments,
            base::Value* result,
            std::string* error);
  int32_t FindWindowIdForWebContents(
      content::WebContents* web_contents) const;
  std::string GetContainerIdForWebContents(
      content::WebContents* web_contents) const;
  const base::DictValue* GetWebPreferencesForWebContents(
      content::WebContents* web_contents) const;
  void Close(int32_t window_id);
  // Synchronously destroys the windows of a disconnected ipcMain container.
  // Their events cannot be delivered to the old process or a later restart.
  void CloseForContainer(const std::string& container_id);
  // Synchronously destroys every hosted BrowserWindow before its
  // BrowserContext is torn down. No window events are dispatched while the
  // process is shutting down.
  void ShutdownForProcessExit();
  bool ActivateAll();
  // Shows only BrowserWindows created by the given ipcMain container.
  bool ActivateForContainer(const std::string& container_id);
  bool HasWindows() const { return !windows_.empty(); }
  bool HasWindowsForContainer(const std::string& container_id) const;

  // Electron `dialog.showOpenDialog`. Runs on the Browser UI thread with an
  // owner HWND from a hosted BrowserWindow. Empty result means cancel.
  std::vector<std::string> ShowOpenDialog(
      const std::string& title,
      bool directory,
      bool allow_multi,
      const std::vector<std::string>& extensions);

  // views::WidgetObserver:
  void OnWidgetActivationChanged(views::Widget* widget, bool active) override;
  void OnWidgetBoundsChanged(views::Widget* widget,
                             const gfx::Rect& new_bounds) override;
  void OnWidgetDestroying(views::Widget* widget) override;
  void OnWidgetShowStateChanged(views::Widget* widget) override;
  void OnWidgetVisibilityChanged(views::Widget* widget,
                                 bool visible) override;

  // Called by the platform subclass procedure. Public only so the Win32
  // trampoline can stay outside the class without exposing HWND in this
  // cross-platform header.
  void OnNativeWindowMessage(uint64_t hwnd,
                             uint32_t message,
                             uint64_t w_param,
                             int64_t l_param);
  bool HandleNativeNonClientMessage(uint64_t hwnd,
                                    uint32_t message,
                                    uint64_t w_param,
                                    int64_t l_param,
                                    int64_t* result);

 private:
  friend class base::NoDestructor<XenonElectronWindowHost>;
  friend class XenonHostedWindowBoundsTest;
  friend class XenonHostedWindowCloseTest;
  friend class XenonHostedWindowPairingTest;
  friend class XenonHostedWindowUserAgentTest;
  XenonElectronWindowHost();
  ~XenonElectronWindowHost() override;

  class HostedWebContentsObserver;
  class PopupMenuSession;

  struct Entry {
    Entry();
    ~Entry();
    raw_ptr<views::Widget> widget = nullptr;
    uint64_t hwnd = 0;
    // False while the initial about:blank surface is hidden.
    bool has_loaded_url = false;
    // A show:false helper must not become the application's sidebar entry.
    bool ever_shown = false;
    // ipcMain already emitted cancellable close and authorized destruction.
    bool close_authorized = false;
    bool ready_to_show_emitted = false;
    bool frameless = false;
    bool transparent = false;
    // Construction establishes the geometry baseline before JS can subscribe.
    bool initializing_bounds = false;
    uint64_t bounds_revision = 0;
    int32_t parent_id = 0;
    std::string container_id;
    base::DictValue web_preferences;
    // Pairing follows the live document, never a pending/aborted navigation.
    std::string committed_url;
    std::string user_agent;
    bool user_agent_update_pending = false;
    // Preserve the existing transparent-overlay behavior independently of the
    // embedder's explicit document pairing configuration.
    bool legacy_parent_pairing = false;
    // Effective pairing requires a live parent in the same container. Explicit
    // document declarations also support opaque control surfaces.
    bool sync_bounds_with_parent = false;
    gfx::Rect bounds;
    gfx::Rect normal_bounds;
    gfx::Size minimum_size;
    bool minimized = false;
    bool maximized = false;
    bool fullscreen = false;
    bool ignore_mouse_events = false;
    std::vector<gfx::Rect> shape_rects;
    std::set<uint32_t> hooked_messages;
    std::unique_ptr<HostedWebContentsObserver> web_contents_observer;
  };

  views::Widget* FindWidget(int32_t window_id) const;
  struct BoundsChange {
    int32_t window_id;
    uint64_t revision;
    gfx::Rect before;
    gfx::Rect after;
  };
  bool CallImpl(int32_t window_id,
                const std::string& command,
                const base::Value& arguments,
                base::Value* result,
                std::string* error);
  void RecordBoundsChange(int32_t window_id, const gfx::Rect& bounds);
  static base::DictValue BoundsChangeMetadata(const BoundsChange& change);
  void DispatchBoundsChange(const BoundsChange& change);
  base::Value MakeWindowCallReply(
      int32_t window_id,
      base::Value value,
      const std::vector<BoundsChange>& changes) const;
  int32_t FindEntryWindowForContainer(const std::string& container_id) const;
  bool ActivateEntryWindow(int32_t window_id);
  bool RequestClose(int32_t window_id);
  void ObserveWebContents(int32_t window_id, content::WebContents* contents);
  void UpdateUserAgent(int32_t window_id,
                       content::WebContents* contents,
                       const std::string& user_agent);
  void ApplyPendingUserAgent(int32_t window_id, content::WebContents* contents);
  std::map<int32_t, Entry>::iterator FindEntry(views::Widget* widget);
  void NotifyEvent(int32_t window_id,
                   const std::string& event_name,
                   base::Value arguments);
  void SynchronizeOverlayBounds(int32_t source_id,
                                const gfx::Rect& bounds);
  void UpdateWindowPairing(int32_t window_id);
  std::vector<int32_t> GetPairedWindowIds(int32_t source_id) const;
  void SynchronizeOverlayShowState(int32_t source_id);
  void ShowPopupMenu(int32_t window_id,
                     base::ListValue items,
                     gfx::Point screen_anchor);
  void OnPopupMenuClosed(PopupMenuSession* session);

  int32_t next_id_ = 1;
  bool pending_activate_ = false;
  bool shutting_down_ = false;
  std::set<std::string> closing_containers_;
  std::set<std::string> pending_activate_containers_;
  bool synchronizing_overlay_bounds_ = false;
  bool synchronizing_overlay_show_state_ = false;
  // Synchronous Widget changes belong in their call's reply, not in a later
  // event delivered to listeners registered after that call has returned.
  raw_ptr<std::vector<BoundsChange>> bounds_transaction_ = nullptr;
  std::optional<std::string> bounds_transaction_container_;
  std::map<int32_t, Entry> windows_;
  std::unique_ptr<PopupMenuSession> popup_menu_;
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_UI_XENON_ELECTRON_WINDOW_HOST_H_
