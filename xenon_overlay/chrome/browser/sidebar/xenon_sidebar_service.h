// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_SIDEBAR_XENON_SIDEBAR_SERVICE_H_
#define XENON_OVERLAY_CHROME_BROWSER_SIDEBAR_XENON_SIDEBAR_SERVICE_H_

#include <string>

#include "base/memory/raw_ptr.h"
#include "base/observer_list.h"
#include "base/observer_list_types.h"
#include "components/keyed_service/core/keyed_service.h"
#include "components/prefs/pref_change_registrar.h"

class PrefService;

namespace xenon {

// Profile-keyed service that owns and manages Xenon browser sidebar state,
// settings, and observer notifications across multiple browser windows.
class XenonSidebarService : public KeyedService {
 public:
  class Observer : public base::CheckedObserver {
   public:
    virtual void OnSidebarAutoHideChanged(bool auto_hide_enabled) {}
    virtual void OnSidebarAlignmentChanged(bool right_aligned) {}

   protected:
    ~Observer() override = default;
  };

  explicit XenonSidebarService(PrefService* prefs);
  ~XenonSidebarService() override;

  XenonSidebarService(const XenonSidebarService&) = delete;
  XenonSidebarService& operator=(const XenonSidebarService&) = delete;

  bool IsAutoHideEnabled() const;
  void SetAutoHideEnabled(bool enabled);

  bool IsRightAligned() const;
  void SetRightAligned(bool right_aligned);

  void AddObserver(Observer* observer);
  void RemoveObserver(Observer* observer);

 private:
  void OnPreferenceChanged(const std::string& pref_name);

  raw_ptr<PrefService> prefs_ = nullptr;
  PrefChangeRegistrar pref_change_registrar_;
  base::ObserverList<Observer> observers_;
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_SIDEBAR_XENON_SIDEBAR_SERVICE_H_
