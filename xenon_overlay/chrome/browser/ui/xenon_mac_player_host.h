// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_UI_XENON_MAC_PLAYER_HOST_H_
#define XENON_OVERLAY_CHROME_BROWSER_UI_XENON_MAC_PLAYER_HOST_H_

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/values.h"
#include "xenon_overlay/chrome/browser/ui/xenon_mac_player_callbacks.h"

namespace xenon {
class XenonMacPlayerView;
class XenonNodeExecutor;
struct MacPlayerAdapter;

// Hosts one registered process-lifetime native player SDK on Browser UI. The
// trusted bundle selects a versioned adapter; application JS remains in
// Utility. Only IDs and owner tokens cross IPC, never Cocoa pointers. All
// entrypoints run on UI. Switching to another SDK/container requires a new
// Browser process.
class XenonMacPlayerHost {
 public:
  using Reply = base::OnceCallback<void(base::Value, const std::string&)>;
  using Event = base::RepeatingCallback<void(base::Value)>;
  static XenonMacPlayerHost* GetInstance();

  void Call(const std::string& container,
            uint64_t generation,
            const base::FilePath& resources_directory,
            const base::FilePath& runtime_directory,
            base::Value request,
            Event event,
            Reply reply);
  void CloseWindow(int32_t window_id);
  void CloseContainer(const std::string& container);
  void Shutdown();

 private:
  friend class base::NoDestructor<XenonMacPlayerHost>;
  XenonMacPlayerHost();
  ~XenonMacPlayerHost();
  void Load(const base::FilePath& resources,
            const base::FilePath& runtime,
            const std::string& requested,
            Reply reply);
  void Execute(const base::DictValue& request, Reply reply);
  bool CloseView(std::string* error);
  bool CloseMedia(std::string* error);
  bool Invoke(std::string_view name,
              const base::Value& args,
              base::Value* result,
              std::string* error);
  void QueueEvent(base::Value event);
  void OnNativeCallback(int32_t client,
                        int32_t id,
                        std::vector<base::Value> args,
                        base::Value receiver);
  void OnNativeCallbackReleased(int32_t client, int32_t id);
  void CancelCompletion(int32_t instance_id);
  void CancelPendingCompletions();
  void QuarantineSession(const std::string& error);
  void DetachSubscriptions(int32_t instance_id = 0);
  struct Subscription {
    int32_t instance_id;
    std::string method;
    base::Value cookie;
  };
  std::vector<Subscription> subscriptions_;
  struct PendingCompletion {
    int32_t client;
    MacPlayerCompletion completion;
  };
  // Providers survive Utility generations so a callback retained by the SDK
  // can still fail its completion after its original client disconnects.
  std::map<std::pair<int32_t, int32_t>, MacPlayerCallbackRegistration>
      providers_;
  std::map<int32_t, PendingCompletion> pending_completions_;

  // Registry entries have static lifetime, including late provider callbacks.
  raw_ptr<const MacPlayerAdapter> adapter_ = nullptr;
  std::unique_ptr<XenonNodeExecutor> executor_;
  std::unique_ptr<XenonMacPlayerView> view_;
  std::string module_path_;
  std::string container_;
  std::string runtime_container_;
  uint64_t generation_ = 0;
  // Owner 1 is reserved for process-lifetime provider callbacks/completions.
  uint64_t owner_ = 1;
  int32_t client_ = 0;
  int32_t stack_id_ = 0;
  int32_t window_id_ = 0;
  bool loading_ = false;
  bool initialized_ = false;
  bool shutdown_ = false;
  bool quarantined_ = false;
  base::DictValue transport_identity_;
  Event event_;
  base::WeakPtrFactory<XenonMacPlayerHost> weak_factory_{this};
};
}  // namespace xenon
#endif
