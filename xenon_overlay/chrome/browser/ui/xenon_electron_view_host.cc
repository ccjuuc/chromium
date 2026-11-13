// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ui/xenon_electron_view_host.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include "base/auto_reset.h"
#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/color_parser.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/rounded_corners_f.h"
#include "ui/views/background.h"
#include "ui/views/controls/native/native_view_host.h"
#include "ui/views/controls/webview/webview.h"
#include "ui/views/view.h"
#include "ui/views/view_targeter.h"
#include "ui/views/view_targeter_delegate.h"
#include "ui/views/widget/widget.h"
#include "ui/views/widget/widget_delegate.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_guest.h"
#include "xenon_overlay/chrome/browser/ui/xenon_electron_window_host.h"

namespace xenon {
namespace {

// The contentView is a sibling above BrowserWindow's primary web surface. Its
// empty area must remain transparent to input intended for that web surface.
class ContainerTargeter final : public views::ViewTargeterDelegate {
 public:
  bool DoesIntersectRect(const views::View* target,
                         const gfx::Rect& rect) const override;
};

bool ContainerTargeter::DoesIntersectRect(const views::View* target,
                                          const gfx::Rect& rect) const {
  if (!target->GetLocalBounds().Intersects(rect)) {
    return false;
  }
  for (const views::View* child : target->children()) {
    if (!child->GetVisible()) {
      continue;
    }
    const gfx::Rect local =
        views::View::ConvertRectToTarget(target, child, rect);
    if (child->HitTestRect(local)) {
      return true;
    }
  }
  return false;
}

base::Value BoundsValue(const gfx::Rect& bounds) {
  return base::Value(base::DictValue()
                         .Set("x", bounds.x())
                         .Set("y", bounds.y())
                         .Set("width", bounds.width())
                         .Set("height", bounds.height()));
}

std::optional<gfx::Rect> ParseBounds(const base::DictValue* bounds) {
  if (!bounds) {
    return std::nullopt;
  }
  const auto x = bounds->FindInt("x"), y = bounds->FindInt("y");
  const auto width = bounds->FindInt("width"),
             height = bounds->FindInt("height");
  if (!x || !y || !width || !height || *width < 0 || *height < 0) {
    return std::nullopt;
  }
  return gfx::Rect(*x, *y, *width, *height);
}

std::unique_ptr<views::View> MakeContainerView() {
  auto view = std::make_unique<views::View>();
  view->SetEventTargeter(std::make_unique<views::ViewTargeter>(
      std::make_unique<ContainerTargeter>()));
  return view;
}

}  // namespace

struct XenonElectronViewHost::Entry {
  int id = 0;
  int parent_id = 0;
  int window_id = 0;
  int contents_id = 0;
  std::string owner;
  raw_ptr<views::View> view = nullptr;
  raw_ptr<views::WebView> web_view = nullptr;
  std::unique_ptr<views::View> detached;
  EventCallback events;
};

struct XenonElectronViewHost::ContentsEntry {
  std::string owner;
  int view_id = 0;
  std::unique_ptr<content::WebContents> contents;
};

XenonElectronViewHost* XenonElectronViewHost::GetInstance() {
  static base::NoDestructor<XenonElectronViewHost> instance;
  return instance.get();
}

XenonElectronViewHost::XenonElectronViewHost() = default;
XenonElectronViewHost::~XenonElectronViewHost() = default;

base::expected<base::Value, std::string> XenonElectronViewHost::Call(
    content::BrowserContext* context,
    const std::string& container,
    const std::string& owner,
    const base::Value& request,
    EventCallback events) {
  const auto* options = request.GetIfDict();
  const auto* action = options ? options->FindString("action") : nullptr;
  if (!action) {
    return base::unexpected("Electron View expects an action");
  }
  if (*action == "create" || *action == "root") {
    auto entry = std::make_unique<Entry>();
    entry->id = next_id_++;
    entry->owner = owner;
    entry->events = std::move(events);
    views::Widget* root_widget = nullptr;
    if (*action == "root") {
      const auto window_id = options->FindInt("windowId");
      content::WebContents* primary = nullptr;
      std::string error;
      if (!window_id ||
          !XenonElectronWindowHost::GetInstance()->GetOwnedWindowTarget(
              container, *window_id, &root_widget, &primary, &error)) {
        return base::unexpected(error.empty() ? "Invalid BrowserWindow"
                                              : error);
      }
      if (const auto found = root_widgets_.find(root_widget);
          found != root_widgets_.end()) {
        const auto& existing = *views_.at(found->second);
        if (existing.owner != owner) {
          return base::unexpected("ERR_ACCESS_DENIED: stale contentView owner");
        }
        return base::Value(base::DictValue().Set("id", existing.id));
      }
      entry->window_id = *window_id;
      entry->detached = MakeContainerView();
    } else {
      const auto* kind = options->FindString("kind");
      if (!kind || (*kind != "view" && *kind != "web-contents")) {
        return base::unexpected("Invalid Electron View kind");
      }
      if (*kind == "view") {
        entry->detached = std::make_unique<views::View>();
      } else {
        const auto provided_id = options->FindInt("webContentsId");
        if (options->Find("webContentsId") &&
            (!provided_id || *provided_id >= 0)) {
          return base::unexpected("Invalid WebContents identity");
        }
        int contents_id = provided_id.value_or(0);
        if (contents_id) {
          auto found = contents_.find(contents_id);
          if (found == contents_.end() || found->second->owner != owner) {
            return base::unexpected(
                "ERR_ACCESS_DENIED: WebContents is not owned by this "
                "application");
          }
          if (found->second->view_id) {
            auto& previous = *views_.at(found->second->view_id);
            if (previous.view->GetWidget()) {
              return base::unexpected(
                  "options.webContents is already attached to a window");
            }
            previous.web_view->SetWebContents(nullptr);
            previous.contents_id = 0;
          }
        } else {
          const auto* preferences = options->FindDict("preferences");
          if (!context || !preferences) {
            return base::unexpected(
                "WebContentsView requires webPreferences and a BrowserContext");
          }
          auto contents_entry = std::make_unique<ContentsEntry>();
          contents_entry->owner = owner;
          contents_entry->contents = ipc::XenonElectronGuest::CreateStandalone(
              context, container, preferences->Clone());
          if (!contents_entry->contents) {
            return base::unexpected(
                "Failed to create WebContentsView WebContents");
          }
          auto* guest = ipc::XenonElectronGuest::FromWebContents(
              contents_entry->contents.get());
          if (!guest) {
            return base::unexpected(
                "Failed to initialize WebContentsView identity");
          }
          contents_id = guest->id();
          guest->SetCloseCallback(
              base::BindRepeating(&XenonElectronViewHost::CloseContents,
                                  base::Unretained(this), contents_id));
          contents_.emplace(contents_id, std::move(contents_entry));
        }
        auto web_view = std::make_unique<views::WebView>();
        web_view->SetWebContents(contents_.at(contents_id)->contents.get());
        contents_.at(contents_id)->contents->WasHidden();
        entry->web_view = web_view.get();
        entry->contents_id = contents_id;
        contents_.at(contents_id)->view_id = entry->id;
        entry->detached = std::move(web_view);
      }
    }
    entry->view = entry->detached.get();
    const int id = entry->id;
    const int contents_id = entry->contents_id;
    auto* view = entry->view.get();
    view_ids_.emplace(view, id);
    views_.emplace(id, std::move(entry));
    view->AddObserver(this);
    if (root_widget) {
      auto* parent = root_widget->widget_delegate()->GetContentsView();
      root_widgets_.emplace(root_widget, id);
      root_parents_.emplace(parent, id);
      root_widget->AddObserver(this);
      parent->AddObserver(this);
      parent->AddChildView(std::move(views_.at(id)->detached));
      view->SetBoundsRect(parent->GetLocalBounds());
    }
    return base::Value(
        base::DictValue().Set("id", id).Set("webContentsId", contents_id));
  }
  if (*action == "close-contents" || *action == "contents-owner") {
    const auto id = options->FindInt("webContentsId");
    auto found = id ? contents_.find(*id) : contents_.end();
    if (found == contents_.end() || found->second->owner != owner) {
      return base::unexpected("ERR_ACCESS_DENIED: unknown WebContents");
    }
    if (*action == "close-contents") {
      const auto wait = options->FindBool("waitForBeforeUnload");
      if (options->Find("waitForBeforeUnload") && !wait) {
        return base::unexpected("waitForBeforeUnload must be a boolean");
      }
      auto* contents = found->second->contents.get();
      if (wait.value_or(false) &&
          contents->NeedToFireBeforeUnloadOrUnloadEvents()) {
        // Electron treats a renderer request to prevent unload as cancellation,
        // without Chromium's confirmation dialog. The main-process listener may
        // override that decision with a subsequent explicit close.
        contents->DispatchBeforeUnload(true);
      } else {
        contents->Close();
      }
      return base::Value();
    }
    for (int view_id = found->second->view_id; view_id;) {
      const auto& entry = *views_.at(view_id);
      if (entry.window_id) {
        return base::Value(entry.window_id);
      }
      view_id = entry.parent_id;
    }
    return base::Value();
  }
  const auto id = options->FindInt("id");
  auto found = id ? views_.find(*id) : views_.end();
  if (found == views_.end() || found->second->owner != owner) {
    return base::unexpected("ERR_ACCESS_DENIED: unknown View");
  }
  auto& entry = *found->second;
  if (*action == "release") {
    ReleaseView(*id);
  } else if (*action == "add" || *action == "remove") {
    const auto child_id = options->FindInt("childId");
    auto child = child_id ? views_.find(*child_id) : views_.end();
    if (child == views_.end() || child->second->owner != owner ||
        child->second->window_id) {
      return base::unexpected("ERR_ACCESS_DENIED: invalid child View");
    }
    if (*action == "remove") {
      if (child->second->parent_id == *id) {
        Detach(*child->second);
      }
    } else {
      for (int ancestor = *id; ancestor;
           ancestor = views_.at(ancestor)->parent_id) {
        if (ancestor == *child_id) {
          return base::unexpected("A View cannot contain one of its ancestors");
        }
      }
      const auto index = options->FindInt("index");
      if (options->Find("index") && (!index || *index < 0)) {
        return base::unexpected("View index must not be negative");
      }
      Detach(*child->second);
      const size_t position = index ? std::min(static_cast<size_t>(*index),
                                               entry.view->children().size())
                                    : entry.view->children().size();
      entry.view->AddChildViewAt(std::move(child->second->detached), position);
      child->second->parent_id = *id;
    }
  } else if (*action == "set-bounds") {
    const auto bounds = ParseBounds(options->FindDict("bounds"));
    if (!bounds) {
      return base::unexpected(
          "View bounds must be an integer Rectangle with non-negative size");
    }
    entry.view->SetBoundsRect(*bounds);
  } else if (*action == "get-bounds") {
    return BoundsValue(entry.view->bounds());
  } else if (*action == "set-visible") {
    const auto visible = options->FindBool("visible");
    if (!visible) {
      return base::unexpected("View visibility must be a boolean");
    }
    entry.view->SetVisible(*visible);
  } else if (*action == "get-visible") {
    return base::Value(entry.view->GetVisible());
  } else if (*action == "set-background") {
    const auto* color_value = options->Find("color");
    std::optional<SkColor> color;
    if (!color_value) {
      return base::unexpected("View background expects a color");
    }
    if (!color_value->is_none()) {
      SkColor parsed;
      if (!color_value->is_string() ||
          !content::ParseCssColorString(color_value->GetString(), &parsed)) {
        return base::unexpected("Invalid View background color");
      }
      color = parsed;
    }
    entry.view->SetBackground(color ? views::CreateSolidBackground(*color)
                                    : nullptr);
    if (entry.contents_id) {
      contents_.at(entry.contents_id)
          ->contents->SetPageBaseBackgroundColor(color);
    }
  } else if (*action == "set-border-radius") {
    const auto radius = options->FindDouble("radius");
    if (!entry.web_view || !radius || !std::isfinite(*radius) || *radius < 0 ||
        *radius > std::numeric_limits<float>::max()) {
      return base::unexpected(
          "WebContentsView radius must be non-negative and finite");
    }
    if (!entry.web_view->holder()->SetNativeViewCornerRadii(
            gfx::RoundedCornersF(static_cast<float>(*radius)))) {
      return base::unexpected(
          "ERR_NOT_SUPPORTED: native View corner radius is unavailable");
    }
  } else {
    return base::unexpected("Unsupported Electron View action: " + *action);
  }
  return base::Value();
}

void XenonElectronViewHost::Detach(Entry& entry) {
  if (entry.view->parent()) {
    entry.detached = entry.view->parent()->RemoveChildViewT(entry.view.get());
  }
  entry.parent_id = 0;
}

void XenonElectronViewHost::UpdateContentsVisibility(
    Entry& entry,
    std::optional<bool> widget_visible) {
  if (!entry.contents_id) {
    return;
  }
  auto* widget = entry.view->GetWidget();
  bool visible = widget && !widget->IsMinimized() &&
                 widget_visible.value_or(widget->IsVisible());
  for (const views::View* view = entry.view.get(); visible && view;
       view = view->parent()) {
    visible = view->GetVisible();
  }
  auto* contents = contents_.at(entry.contents_id)->contents.get();
  if (visible) {
    contents->WasShown();
  } else {
    contents->WasHidden();
  }
}

void XenonElectronViewHost::ReleaseView(int id) {
  auto found = views_.find(id);
  if (found == views_.end()) {
    return;
  }
  auto& entry = *found->second;
  std::vector<int> children;
  for (const auto& [child_id, child] : views_) {
    if (child->parent_id == id) {
      children.push_back(child_id);
    }
  }
  for (int child : children) {
    Detach(*views_.at(child));
  }
  if (entry.window_id && entry.view->GetWidget()) {
    OnWidgetDestroying(entry.view->GetWidget());
  }
  Detach(entry);
  if (entry.contents_id) {
    contents_.at(entry.contents_id)->view_id = 0;
  }
  entry.view->RemoveObserver(this);
  view_ids_.erase(entry.view.get());
  // Detached owns the view and is destroyed with the registry entry. Drop
  // non-owning aliases first so raw_ptr never outlives that owned allocation.
  entry.web_view = nullptr;
  entry.view = nullptr;
  views_.erase(found);
}

void XenonElectronViewHost::CloseContents(int id) {
  auto found = contents_.find(id);
  if (found == contents_.end()) {
    return;
  }
  if (found->second->view_id) {
    auto& entry = *views_.at(found->second->view_id);
    entry.web_view->SetWebContents(nullptr);
    entry.contents_id = 0;
  }
  contents_.erase(found);
}

void XenonElectronViewHost::ReleaseOwner(const std::string& owner) {
  base::AutoReset releasing(&releasing_, true);
  std::vector<int> views;
  for (const auto& [id, entry] : views_) {
    if (entry->owner == owner) {
      views.push_back(id);
    }
  }
  for (int id : views) {
    ReleaseView(id);
  }
  std::erase_if(contents_,
                [&](const auto& item) { return item.second->owner == owner; });
}

void XenonElectronViewHost::Notify(Entry& entry, const char* event) {
  if (!releasing_ && entry.events) {
    entry.events.Run(
        base::Value(base::DictValue().Set("id", entry.id).Set("event", event)));
  }
}

void XenonElectronViewHost::OnViewBoundsChanged(views::View* view) {
  if (auto parent = root_parents_.find(view); parent != root_parents_.end()) {
    views_.at(parent->second)->view->SetBoundsRect(view->GetLocalBounds());
  }
  if (auto found = view_ids_.find(view); found != view_ids_.end()) {
    Notify(*views_.at(found->second), "bounds-changed");
  }
}

void XenonElectronViewHost::OnViewVisibilityChanged(views::View* view,
                                                    views::View* starting_view,
                                                    bool visible) {
  if (auto found = view_ids_.find(view); found != view_ids_.end()) {
    auto& entry = *views_.at(found->second);
    // Views reports intended Widget visibility before Widget::IsVisible() is
    // updated on some platforms. Keep the page lifecycle in sync explicitly;
    // independent WebContents cannot rely on browser-tab occlusion updates.
    UpdateContentsVisibility(
        entry, starting_view ? std::nullopt : std::optional(visible));
    Notify(entry, "visibility-changed");
  }
}

void XenonElectronViewHost::OnViewAddedToWidget(views::View* view) {
  if (auto found = view_ids_.find(view); found != view_ids_.end()) {
    UpdateContentsVisibility(*views_.at(found->second));
  }
}

void XenonElectronViewHost::OnViewRemovedFromWidget(views::View* view) {
  if (auto found = view_ids_.find(view); found != view_ids_.end()) {
    UpdateContentsVisibility(*views_.at(found->second), false);
  }
}

void XenonElectronViewHost::OnViewIsDeleting(views::View* view) {
  // Root widgets are detached in OnWidgetDestroying before their Views tree is
  // deleted. Other application-owned Views are deleted only by ReleaseView.
  root_parents_.erase(view);
}

void XenonElectronViewHost::OnWidgetDestroying(views::Widget* widget) {
  auto found = root_widgets_.find(widget);
  if (found == root_widgets_.end()) {
    return;
  }
  auto& root = *views_.at(found->second);
  if (auto* parent = root.view->parent()) {
    parent->RemoveObserver(this);
    root_parents_.erase(parent);
  }
  root_widgets_.erase(found);
  widget->RemoveObserver(this);
  Detach(root);
  root.window_id = 0;
}

}  // namespace xenon
