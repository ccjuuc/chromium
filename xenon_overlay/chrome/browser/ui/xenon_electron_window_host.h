// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_UI_XENON_ELECTRON_WINDOW_HOST_H_
#define XENON_OVERLAY_CHROME_BROWSER_UI_XENON_ELECTRON_WINDOW_HOST_H_

#include <cstdint>
#include <map>
#include <memory>
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
  void Close(int32_t window_id);
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
  XenonElectronWindowHost();
  ~XenonElectronWindowHost() override;

  class HostedWebContentsObserver;
  class PopupMenuSession;

  struct Entry {
    raw_ptr<views::Widget> widget = nullptr;
    uint64_t hwnd = 0;
    // False while the initial about:blank surface is hidden.
    bool has_loaded_url = false;
    bool ready_to_show_emitted = false;
    bool frameless = false;
    bool transparent = false;
    int32_t parent_id = 0;
    std::string container_id;
    std::string url;
    std::string user_agent;
    // A transparent owned window is an overlay surface. In Electron the
    // in-process native helper can keep it aligned with its parent; Xenon's
    // addon isolate is out of process, so the Browser-owned Widgets maintain
    // this native relationship directly.
    bool sync_bounds_with_parent = false;
    gfx::Rect bounds;
    gfx::Rect normal_bounds;
    bool minimized = false;
    bool maximized = false;
    bool fullscreen = false;
    bool ignore_mouse_events = false;
    std::vector<gfx::Rect> shape_rects;
    std::set<uint32_t> hooked_messages;
    std::unique_ptr<HostedWebContentsObserver> web_contents_observer;
  };

  views::Widget* FindWidget(int32_t window_id) const;
  int32_t FindEntryWindowForContainer(
      const std::string& container_id) const;
  bool ActivateEntryWindow(int32_t window_id);
  std::map<int32_t, Entry>::iterator FindEntry(views::Widget* widget);
  void NotifyEvent(int32_t window_id,
                   const std::string& event_name,
                   base::Value arguments);
  void SynchronizeOverlayBounds(int32_t source_id,
                                const gfx::Rect& bounds);
  void SynchronizeOverlayShowState(int32_t source_id);
  void ShowPopupMenu(int32_t window_id,
                     base::ListValue items,
                     gfx::Point screen_anchor);
  void OnPopupMenuClosed(PopupMenuSession* session);

  int32_t next_id_ = 1;
  bool pending_activate_ = false;
  bool shutting_down_ = false;
  std::set<std::string> pending_activate_containers_;
  bool synchronizing_overlay_bounds_ = false;
  bool synchronizing_overlay_show_state_ = false;
  std::map<int32_t, Entry> windows_;
  std::unique_ptr<PopupMenuSession> popup_menu_;
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_UI_XENON_ELECTRON_WINDOW_HOST_H_
