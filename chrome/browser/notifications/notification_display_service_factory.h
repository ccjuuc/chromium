// Copyright 2016 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_NOTIFICATIONS_NOTIFICATION_DISPLAY_SERVICE_FACTORY_H_
#define CHROME_BROWSER_NOTIFICATIONS_NOTIFICATION_DISPLAY_SERVICE_FACTORY_H_

#include "base/no_destructor.h"
#include "chrome/browser/profiles/profile_keyed_service_factory.h"
#include "xenon_overlay/buildflags/buildflags.h"

class NotificationDisplayService;
class Profile;

class NotificationDisplayServiceFactory : public ProfileKeyedServiceFactory {
 public:
  NotificationDisplayServiceFactory(const NotificationDisplayServiceFactory&) =
      delete;
  NotificationDisplayServiceFactory& operator=(
      const NotificationDisplayServiceFactory&) = delete;
  static NotificationDisplayService* GetForProfile(Profile* profile);
#if BUILDFLAG(ENABLE_XENON_SERVICE)
  // Cleanup must not initialize the platform notification bridge.
  static NotificationDisplayService* GetForProfileIfExists(Profile* profile);
#endif
  static NotificationDisplayServiceFactory* GetInstance();

 private:
  friend base::NoDestructor<NotificationDisplayServiceFactory>;

  NotificationDisplayServiceFactory();

  // BrowserContextKeyedServiceFactory implementation.
  std::unique_ptr<KeyedService> BuildServiceInstanceForBrowserContext(
      content::BrowserContext* context) const override;
};

#endif  // CHROME_BROWSER_NOTIFICATIONS_NOTIFICATION_DISPLAY_SERVICE_FACTORY_H_
