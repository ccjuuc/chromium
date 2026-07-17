// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/sidebar/xenon_sidebar_service_factory.h"

#include "base/check.h"
#include "base/no_destructor.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_selections.h"
#include "content/public/browser/browser_context.h"
#include "xenon_overlay/chrome/browser/sidebar/xenon_sidebar_service.h"

namespace xenon {

// static
XenonSidebarServiceFactory* XenonSidebarServiceFactory::GetInstance() {
  static base::NoDestructor<XenonSidebarServiceFactory> instance;
  return instance.get();
}

// static
XenonSidebarService* XenonSidebarServiceFactory::GetForProfile(
    Profile* profile) {
  if (!profile) {
    return nullptr;
  }
  return static_cast<XenonSidebarService*>(
      GetInstance()->GetServiceForBrowserContext(profile, true));
}

XenonSidebarServiceFactory::XenonSidebarServiceFactory()
    : ProfileKeyedServiceFactory(
          "XenonSidebarService",
          ProfileSelections::Builder()
              .WithRegular(ProfileSelection::kOwnInstance)
              .WithGuest(ProfileSelection::kOwnInstance)
              .WithAshInternals(ProfileSelection::kRedirectedToOriginal)
              .Build()) {}

XenonSidebarServiceFactory::~XenonSidebarServiceFactory() = default;

std::unique_ptr<KeyedService>
XenonSidebarServiceFactory::BuildServiceInstanceForBrowserContext(
    content::BrowserContext* context) const {
  Profile* const profile = Profile::FromBrowserContext(context);
  CHECK(profile);
  return std::make_unique<XenonSidebarService>(profile->GetPrefs());
}

}  // namespace xenon
