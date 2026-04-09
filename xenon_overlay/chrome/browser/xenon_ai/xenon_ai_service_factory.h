// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_XENON_AI_XENON_AI_SERVICE_FACTORY_H_
#define XENON_OVERLAY_CHROME_BROWSER_XENON_AI_XENON_AI_SERVICE_FACTORY_H_

#include <memory>

#include "chrome/browser/profiles/profile_keyed_service_factory.h"
#include "content/public/browser/browser_context.h"

namespace base {
template <typename T>
class NoDestructor;
}  // namespace base

class Profile;

namespace xenon {

class XenonAiService;

class XenonAiServiceFactory : public ProfileKeyedServiceFactory {
 public:
  static XenonAiServiceFactory* GetInstance();
  static XenonAiService* GetForProfile(Profile* profile);

  XenonAiServiceFactory(const XenonAiServiceFactory&) = delete;
  XenonAiServiceFactory& operator=(const XenonAiServiceFactory&) = delete;

 private:
  friend base::NoDestructor<XenonAiServiceFactory>;

  XenonAiServiceFactory();
  ~XenonAiServiceFactory() override;

  std::unique_ptr<KeyedService> BuildServiceInstanceForBrowserContext(
      content::BrowserContext* context) const override;
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_XENON_AI_XENON_AI_SERVICE_FACTORY_H_
