// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_UI_XENON_ELECTRON_VIEW_HOST_H_
#define XENON_OVERLAY_CHROME_BROWSER_UI_XENON_ELECTRON_VIEW_HOST_H_

#include <map>
#include <memory>
#include <optional>
#include <string>

#include "base/functional/callback.h"
#include "base/no_destructor.h"
#include "base/types/expected.h"
#include "base/values.h"
#include "ui/views/view_observer.h"
#include "ui/views/widget/widget_observer.h"

namespace content {
class BrowserContext;
}
namespace views {
class View;
class Widget;
}  // namespace views

namespace xenon {

// Native backing for Electron's View tree. Attached nodes are owned by their
// parent; detached nodes retain explicit ownership here until their JS wrapper
// is collected or its application service generation exits. WebContents have
// independent lifetimes, matching WebContentsView's explicit close contract.
class XenonElectronViewHost final : public views::ViewObserver,
                                    public views::WidgetObserver {
 public:
  using EventCallback = base::RepeatingCallback<void(base::Value)>;

  static XenonElectronViewHost* GetInstance();
  base::expected<base::Value, std::string> Call(
      content::BrowserContext* context,
      const std::string& container,
      const std::string& owner,
      const base::Value& request,
      EventCallback events);
  void ReleaseOwner(const std::string& owner);

 private:
  friend class base::NoDestructor<XenonElectronViewHost>;
  struct Entry;
  struct ContentsEntry;
  XenonElectronViewHost();
  ~XenonElectronViewHost() override;
  void ReleaseView(int id);
  void CloseContents(int id);
  void Detach(Entry& entry);
  void UpdateContentsVisibility(Entry& entry,
                                std::optional<bool> widget_visible = {});
  void Notify(Entry& entry, const char* event);
  void OnViewBoundsChanged(views::View* view) override;
  void OnViewVisibilityChanged(views::View* view,
                               views::View* starting_view,
                               bool visible) override;
  void OnViewAddedToWidget(views::View* view) override;
  void OnViewRemovedFromWidget(views::View* view) override;
  void OnViewIsDeleting(views::View* view) override;
  void OnWidgetDestroying(views::Widget* widget) override;

  int next_id_ = 1;
  bool releasing_ = false;
  std::map<int, std::unique_ptr<Entry>> views_;
  std::map<int, std::unique_ptr<ContentsEntry>> contents_;
  std::map<views::View*, int> view_ids_;
  std::map<views::View*, int> root_parents_;
  std::map<views::Widget*, int> root_widgets_;
};

}  // namespace xenon
#endif  // XENON_OVERLAY_CHROME_BROWSER_UI_XENON_ELECTRON_VIEW_HOST_H_
