// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/sidebar/xenon_sidebar_service.h"

#include "base/functional/bind.h"
#include "chrome/common/pref_names.h"
#include "components/prefs/pref_service.h"
#include "xenon_overlay/chrome/browser/xenon_prefs.h"

namespace xenon {

XenonSidebarService::XenonSidebarService(PrefService* prefs) : prefs_(prefs) {
  if (prefs_) {
    pref_change_registrar_.Init(prefs_);
    pref_change_registrar_.Add(
        prefs::kSidebarAutoHideEnabled,
        base::BindRepeating(&XenonSidebarService::OnPreferenceChanged,
                            base::Unretained(this)));
    pref_change_registrar_.Add(
        ::prefs::kSidePanelHorizontalAlignment,
        base::BindRepeating(&XenonSidebarService::OnPreferenceChanged,
                            base::Unretained(this)));
  }
}

XenonSidebarService::~XenonSidebarService() = default;

bool XenonSidebarService::IsAutoHideEnabled() const {
  if (!prefs_) {
    return true;
  }
  return prefs_->GetBoolean(prefs::kSidebarAutoHideEnabled);
}

void XenonSidebarService::SetAutoHideEnabled(bool enabled) {
  if (prefs_) {
    prefs_->SetBoolean(prefs::kSidebarAutoHideEnabled, enabled);
  }
}

bool XenonSidebarService::IsRightAligned() const {
  if (!prefs_) {
    return false;
  }
  return prefs_->GetBoolean(::prefs::kSidePanelHorizontalAlignment);
}

void XenonSidebarService::SetRightAligned(bool right_aligned) {
  if (prefs_) {
    prefs_->SetBoolean(::prefs::kSidePanelHorizontalAlignment, right_aligned);
  }
}

void XenonSidebarService::AddObserver(Observer* observer) {
  observers_.AddObserver(observer);
}

void XenonSidebarService::RemoveObserver(Observer* observer) {
  observers_.RemoveObserver(observer);
}

void XenonSidebarService::OnPreferenceChanged(const std::string& pref_name) {
  if (pref_name == prefs::kSidebarAutoHideEnabled) {
    const bool auto_hide = IsAutoHideEnabled();
    for (auto& observer : observers_) {
      observer.OnSidebarAutoHideChanged(auto_hide);
    }
  } else if (pref_name == ::prefs::kSidePanelHorizontalAlignment) {
    const bool right_aligned = IsRightAligned();
    for (auto& observer : observers_) {
      observer.OnSidebarAlignmentChanged(right_aligned);
    }
  }
}

}  // namespace xenon
