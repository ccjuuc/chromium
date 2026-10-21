// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
#ifndef XENON_OVERLAY_CHROME_BROWSER_UPDATER_XENON_SYSTEM_UPDATER_H_
#define XENON_OVERLAY_CHROME_BROWSER_UPDATER_XENON_SYSTEM_UPDATER_H_

#include <cstdint>
#include <memory>
#include <string>

#include "base/functional/callback.h"

namespace xenon::updater {
// The system service owns the update transaction, including full-package
// fallback. Browser callers cannot choose a different server or installer.
class XenonSystemUpdater {
 public:
  struct Event {
    enum class Phase {
      kChecking,
      kAvailable,
      kDownloading,
      kPreparing,
      kAwaitingExit,
      kInstalled,
      kNoUpdate,
      kError
    };
    Phase phase = Phase::kChecking;
    std::string version;
    int64_t downloaded = 0;
    int64_t total = 0;
    std::string error;
  };
  using Observer = base::RepeatingCallback<void(const Event&)>;
  virtual ~XenonSystemUpdater() = default;
  virtual std::string GetFeedURL() const = 0;
  virtual void Start(bool check_only, Observer observer) = 0;
};
std::unique_ptr<XenonSystemUpdater> CreateXenonSystemUpdater();
}  // namespace xenon::updater
#endif
