// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/xenon_ai/xenon_ai_service_factory.h"

#include "base/check.h"
#include "base/no_destructor.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_selections.h"
#include "content/public/browser/browser_context.h"
#include "xenon_overlay/chrome/browser/xenon_ai/xenon_ai_service.h"

namespace xenon {

// static
XenonAiServiceFactory* XenonAiServiceFactory::GetInstance() {
  static base::NoDestructor<XenonAiServiceFactory> instance;
  return instance.get();
}

// static
XenonAiService* XenonAiServiceFactory::GetForProfile(Profile* profile) {
  return static_cast<XenonAiService*>(
      GetInstance()->GetServiceForBrowserContext(profile, true));
}

XenonAiServiceFactory::XenonAiServiceFactory()
    : ProfileKeyedServiceFactory(
          "XenonAiService",
          ProfileSelections::Builder()
              .WithRegular(ProfileSelection::kOriginalOnly)
              .Build()) {}

XenonAiServiceFactory::~XenonAiServiceFactory() = default;

std::unique_ptr<KeyedService>
XenonAiServiceFactory::BuildServiceInstanceForBrowserContext(
    content::BrowserContext* context) const {
  Profile* const profile = Profile::FromBrowserContext(context);
  CHECK(profile);
  return std::make_unique<XenonAiService>(profile);
}

}  // namespace xenon
