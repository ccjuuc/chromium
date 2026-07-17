// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_SIDEBAR_XENON_SIDEBAR_SERVICE_FACTORY_H_
#define XENON_OVERLAY_CHROME_BROWSER_SIDEBAR_XENON_SIDEBAR_SERVICE_FACTORY_H_

#include <memory>

#include "chrome/browser/profiles/profile_keyed_service_factory.h"

namespace base {
template <typename T>
class NoDestructor;
}  // namespace base

class Profile;

namespace xenon {

class XenonSidebarService;

// Factory that creates and manages XenonSidebarService per Profile/BrowserContext.
class XenonSidebarServiceFactory : public ProfileKeyedServiceFactory {
 public:
  static XenonSidebarServiceFactory* GetInstance();
  static XenonSidebarService* GetForProfile(Profile* profile);

  XenonSidebarServiceFactory(const XenonSidebarServiceFactory&) = delete;
  XenonSidebarServiceFactory& operator=(const XenonSidebarServiceFactory&) = delete;

 private:
  friend base::NoDestructor<XenonSidebarServiceFactory>;

  XenonSidebarServiceFactory();
  ~XenonSidebarServiceFactory() override;

  // ProfileKeyedServiceFactory overrides:
  std::unique_ptr<KeyedService> BuildServiceInstanceForBrowserContext(
      content::BrowserContext* context) const override;
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_SIDEBAR_XENON_SIDEBAR_SERVICE_FACTORY_H_
