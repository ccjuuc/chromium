// Copyright 2016 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/gfx/win/rendering_window_manager.h"

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/task/single_thread_task_runner.h"

#if BUILDFLAG(ENABLE_XENON_SERVICE)
#include <cstdint>
#include <tuple>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/containers/flat_set.h"
#include "base/functional/callback_helpers.h"
#include "base/win/scoped_gdi_object.h"
#include "ui/gfx/win/hwnd_util.h"
#endif

namespace gfx {

#if BUILDFLAG(ENABLE_XENON_SERVICE)
namespace {

using ScopedRegion = base::win::ScopedGDIObject<HRGN>;
constexpr wchar_t kClippingIdentity[] = L"Chromium.NativeChildClippingIdentity";

// SetWindowRgn transfers ownership only on success. Compare first because it
// also generates WinEvents, which must not cause an endless update cycle.
void ApplyWindowRegion(HWND window, ScopedRegion region) {
  ScopedRegion current(::CreateRectRgn(0, 0, 0, 0));
  const bool has_current = ::GetWindowRgn(window, current.get()) != ERROR;
  if ((!region.get() && !has_current) ||
      (region.get() && has_current &&
       ::EqualRgn(region.get(), current.get()))) {
    return;
  }
  if (::SetWindowRgn(window, region.get(), TRUE)) {
    std::ignore = region.release();
  }
}

}  // namespace

struct RenderingWindowManager::NativeChildClippingState {
  bool IsCurrentWindow() const {
    return child && identity && ::GetProp(child, kClippingIdentity) == identity;
  }

  void Restore(HWND parent) {
    const bool is_current = IsCurrentWindow();
    const HWND old_child = std::exchange(child, nullptr);
    identity = nullptr;
    ScopedRegion region = std::move(original_region);
    if (is_current) {
      ::RemoveProp(old_child, kClippingIdentity);
      if (::GetParent(old_child) == parent) {
        ApplyWindowRegion(old_child, std::move(region));
      }
    }
  }

  HWND child = nullptr;
  HANDLE identity = nullptr;
  ScopedRegion original_region;
  // Remember even hidden siblings, so a destroy/reparent event can be matched
  // after GetParent() stops identifying the registered parent.
  base::flat_set<HWND> siblings;
};
#endif  // BUILDFLAG(ENABLE_XENON_SERVICE)

// static
RenderingWindowManager* RenderingWindowManager::GetInstance() {
  static base::NoDestructor<RenderingWindowManager> instance;
  return instance.get();
}

void RenderingWindowManager::RegisterParent(HWND parent) {
  DCHECK(task_runner_->BelongsToCurrentThread());
  registered_hwnds_.emplace(parent, nullptr);
}

void RenderingWindowManager::RegisterChild(HWND parent,
                                           HWND child,
                                           DWORD expected_child_process_id) {
  if (!child) {
    return;
  }

  // This can be called from any thread, if we're not on the correct thread then
  // PostTask back to the UI thread before doing anything.
  if (!task_runner_->BelongsToCurrentThread()) {
    task_runner_->PostTask(
        FROM_HERE, base::BindOnce(&RenderingWindowManager::RegisterChild,
                                  base::Unretained(this), parent, child,
                                  expected_child_process_id));
    return;
  }

  // Check that |parent| was registered as a HWND that could have a child HWND.
  auto it = registered_hwnds_.find(parent);
  if (it == registered_hwnds_.end()) {
    return;
  }

  // Check that |child| belongs to the GPU process.
  DWORD child_process_id = 0;
  DWORD child_thread_id = GetWindowThreadProcessId(child, &child_process_id);
  if (!child_thread_id || child_process_id != expected_child_process_id) {
    DLOG(ERROR) << "Child HWND not owned by GPU process.";
    return;
  }

#if BUILDFLAG(ENABLE_XENON_SERVICE)
  auto clipping = native_child_clipping_.find(parent);
  if (clipping != native_child_clipping_.end() &&
      (it->second != child || !clipping->second->IsCurrentWindow())) {
    clipping->second->Restore(parent);
  }
  // Restoring a region can send synchronous window messages.
  it = registered_hwnds_.find(parent);
  if (it == registered_hwnds_.end()) {
    return;
  }
#endif
  it->second = child;

  ::SetParent(child, parent);
  // Move D3D window behind Chrome's window to avoid losing some messages.
  ::SetWindowPos(child, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
#if BUILDFLAG(ENABLE_XENON_SERVICE)
  clipping = native_child_clipping_.find(parent);
  if (clipping != native_child_clipping_.end()) {
    UpdateNativeChildClipping(parent, *clipping->second);
  }
#endif
}

void RenderingWindowManager::UnregisterParent(HWND parent) {
  DCHECK(task_runner_->BelongsToCurrentThread());
  registered_hwnds_.erase(parent);
#if BUILDFLAG(ENABLE_XENON_SERVICE)
  auto it = native_child_clipping_.find(parent);
  if (it != native_child_clipping_.end()) {
    it->second->siblings.clear();
    it->second->Restore(parent);
  }
#endif
}

#if BUILDFLAG(ENABLE_XENON_SERVICE)
bool RenderingWindowManager::SetNativeChildClippingEnabled(HWND parent,
                                                           bool enabled) {
  DCHECK(task_runner_->BelongsToCurrentThread());
  if (!enabled) {
    StopNativeChildClipping(parent);
    return true;
  }
  DWORD parent_process_id = 0;
  if (!::GetWindowThreadProcessId(parent, &parent_process_id) ||
      parent_process_id != ::GetCurrentProcessId()) {
    return false;
  }
  if (!native_child_event_hook_) {
    // Out-of-context callbacks are delivered on this UI thread, including
    // events from native children owned by another process. No polling is used.
    native_child_event_hook_ =
        ::SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_UNCLOAKED, nullptr,
                          OnWindowEvent, 0, 0, WINEVENT_OUTOFCONTEXT);
    if (!native_child_event_hook_) {
      PLOG(ERROR) << "Could not monitor native child windows";
      return false;
    }
  }
  auto [it, inserted] = native_child_clipping_.try_emplace(parent);
  if (inserted) {
    it->second = std::make_unique<NativeChildClippingState>();
  }
  UpdateNativeChildClipping(parent, *it->second);
  return true;
}

void RenderingWindowManager::StopNativeChildClipping(HWND parent) {
  auto it = native_child_clipping_.find(parent);
  if (it != native_child_clipping_.end()) {
    auto state = std::move(it->second);
    native_child_clipping_.erase(it);
    state->Restore(parent);
  }
  if (native_child_clipping_.empty() && native_child_event_hook_) {
    ::UnhookWinEvent(native_child_event_hook_);
    native_child_event_hook_ = nullptr;
  }
}

// static
void CALLBACK RenderingWindowManager::OnWindowEvent(HWINEVENTHOOK hook,
                                                    DWORD event,
                                                    HWND window,
                                                    LONG object_id,
                                                    LONG child_id,
                                                    DWORD event_thread,
                                                    DWORD event_time) {
  // Native child Z-order changes are reported on the parent's client object.
  const bool is_client_reorder =
      event == EVENT_OBJECT_REORDER && object_id == OBJID_CLIENT;
  if (!window || (object_id != OBJID_WINDOW && !is_client_reorder) ||
      child_id != CHILDID_SELF) {
    return;
  }
  switch (event) {
    case EVENT_OBJECT_CREATE:
    case EVENT_OBJECT_DESTROY:
    case EVENT_OBJECT_SHOW:
    case EVENT_OBJECT_HIDE:
    case EVENT_OBJECT_REORDER:
    case EVENT_OBJECT_LOCATIONCHANGE:
    case EVENT_OBJECT_PARENTCHANGE:
    case EVENT_OBJECT_CLOAKED:
    case EVENT_OBJECT_UNCLOAKED:
      break;
    default:
      return;
  }
  auto* manager = GetInstance();
  DCHECK(manager->task_runner_->BelongsToCurrentThread());
  if (hook != manager->native_child_event_hook_) {
    return;
  }
  if (event == EVENT_OBJECT_DESTROY && !::IsWindow(window) &&
      manager->native_child_clipping_.contains(window)) {
    manager->StopNativeChildClipping(window);
    manager->UnregisterParent(window);
    return;
  }
  const HWND parent = ::GetParent(window);
  bool affected = false;
  for (auto& [registered_parent, state] : manager->native_child_clipping_) {
    if (window == registered_parent || parent == registered_parent ||
        window == state->child || state->siblings.contains(window)) {
      affected = true;
      if (event == EVENT_OBJECT_DESTROY && window == state->child &&
          !state->IsCurrentWindow()) {
        // A delayed destroy event must not clear a new window registered with
        // the same numeric HWND. The current window's cookie identifies it.
        state->child = nullptr;
        state->identity = nullptr;
        state->original_region.reset();
        manager->registered_hwnds_[registered_parent] = nullptr;
      }
    }
  }
  if (affected && !manager->clipping_update_pending_) {
    manager->clipping_update_pending_ = true;
    manager->task_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(static_cast<void (RenderingWindowManager::*)()>(
                           &RenderingWindowManager::UpdateNativeChildClipping),
                       base::Unretained(manager)));
  }
}

void RenderingWindowManager::UpdateNativeChildClipping() {
  clipping_update_pending_ = false;
  std::vector<HWND> parents;
  for (const auto& [parent, state] : native_child_clipping_) {
    parents.push_back(parent);
  }
  for (HWND parent : parents) {
    if (!::IsWindow(parent)) {
      StopNativeChildClipping(parent);
      UnregisterParent(parent);
      continue;
    }
    auto it = native_child_clipping_.find(parent);
    if (it != native_child_clipping_.end()) {
      UpdateNativeChildClipping(parent, *it->second);
    }
  }
}

void RenderingWindowManager::UpdateNativeChildClipping(
    HWND parent,
    NativeChildClippingState& state) {
  auto registered = registered_hwnds_.find(parent);
  if (registered == registered_hwnds_.end()) {
    return;
  }
  const HWND child = registered->second;
  if (state.child && !state.IsCurrentWindow()) {
    // A reused HWND must pass RegisterChild's process-id check again before it
    // can be managed. Never capture a replacement window's region implicitly.
    state.Restore(parent);
    registered->second = nullptr;
    return;
  }
  if (!::IsWindow(child) || ::GetParent(child) != parent) {
    return;
  }

  // Window rectangles and regions must use the same physical-pixel space,
  // including when a child belongs to a process with different DPI awareness.
  base::ScopedClosureRunner restore_dpi(base::BindOnce(
      [](DPI_AWARENESS_CONTEXT previous) {
        if (previous) {
          ::SetThreadDpiAwarenessContext(previous);
        }
      },
      ::SetThreadDpiAwarenessContext(
          DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)));
  RECT child_rect;
  if (!::GetWindowRect(child, &child_rect)) {
    return;
  }
  if (state.child != child) {
    // RegisterChild restores the previous GPU window before replacing it.
    DCHECK(!state.child);
    // An integer cookie avoids exposing a browser object address to the GPU
    // process. Window properties disappear when their HWND is destroyed.
    static uintptr_t next_identity = 0;
    ++next_identity;
    CHECK(next_identity != 0);
    HANDLE identity = reinterpret_cast<HANDLE>(next_identity);
    if (!::SetProp(child, kClippingIdentity, identity)) {
      PLOG(ERROR) << "Could not identify GPU window for native child clipping";
      return;
    }
    state.child = child;
    state.identity = identity;
    ScopedRegion original(::CreateRectRgn(0, 0, 0, 0));
    if (::GetWindowRgn(child, original.get()) != ERROR) {
      state.original_region = std::move(original);
    }
  }

  ScopedRegion visible(::CreateRectRgn(0, 0, child_rect.right - child_rect.left,
                                       child_rect.bottom - child_rect.top));
  if (state.original_region.get()) {
    ::CombineRgn(visible.get(), visible.get(), state.original_region.get(),
                 RGN_AND);
  }
  state.siblings.clear();
  bool above_gpu = true;
  bool clipped = false;
  for (HWND sibling = ::GetTopWindow(parent); sibling;
       sibling = ::GetWindow(sibling, GW_HWNDNEXT)) {
    if (sibling == child) {
      above_gpu = false;
      continue;
    }
    state.siblings.insert(sibling);
    if (!above_gpu || !::IsWindowVisible(sibling) ||
        !(::GetWindowLongPtr(sibling, GWL_STYLE) & WS_CHILD) ||
        (::GetWindowLongPtr(sibling, GWL_EXSTYLE) &
         (WS_EX_TRANSPARENT | WS_EX_LAYERED)) ||
        IsWindowCloaked(sibling)) {
      continue;
    }
    RECT rect;
    RECT intersection;
    if (!::GetWindowRect(sibling, &rect) ||
        !::IntersectRect(&intersection, &rect, &child_rect)) {
      continue;
    }
    ScopedRegion region(::CreateRectRgn(0, 0, 0, 0));
    if (::GetWindowRgn(sibling, region.get()) == ERROR) {
      ::SetRectRgn(region.get(), 0, 0, rect.right - rect.left,
                   rect.bottom - rect.top);
    }
    ::OffsetRgn(region.get(), rect.left - child_rect.left,
                rect.top - child_rect.top);
    ScopedRegion bounds(::CreateRectRgn(intersection.left - child_rect.left,
                                        intersection.top - child_rect.top,
                                        intersection.right - child_rect.left,
                                        intersection.bottom - child_rect.top));
    ::CombineRgn(region.get(), region.get(), bounds.get(), RGN_AND);
    ::CombineRgn(visible.get(), visible.get(), region.get(), RGN_DIFF);
    clipped = true;
  }
  if (!clipped && !state.original_region.get()) {
    visible.reset();
  }
  ApplyWindowRegion(child, std::move(visible));
}
#endif  // BUILDFLAG(ENABLE_XENON_SERVICE)

bool RenderingWindowManager::HasValidChildWindow(HWND parent) {
  DCHECK(task_runner_->BelongsToCurrentThread());
  auto it = registered_hwnds_.find(parent);
  if (it == registered_hwnds_.end()) {
    return false;
  }
  return !!it->second && ::IsWindow(it->second);
}

RenderingWindowManager::RenderingWindowManager()
    : task_runner_(base::SingleThreadTaskRunner::GetCurrentDefault()) {}

RenderingWindowManager::~RenderingWindowManager() = default;

}  // namespace gfx
