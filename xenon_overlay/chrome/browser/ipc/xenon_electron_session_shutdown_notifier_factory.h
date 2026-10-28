// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_SESSION_SHUTDOWN_NOTIFIER_FACTORY_H_
#define XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_SESSION_SHUTDOWN_NOTIFIER_FACTORY_H_

#include "base/no_destructor.h"
#include "components/keyed_service/content/browser_context_keyed_service_shutdown_notifier_factory.h"

namespace xenon::ipc {

// Registered during Chrome's factory initialization, before registration is
// frozen. The per-context notifier remains lazy until its first session.
class ElectronSessionShutdownNotifierFactory
    : public BrowserContextKeyedServiceShutdownNotifierFactory {
 public:
  static ElectronSessionShutdownNotifierFactory* GetInstance();

 private:
  friend class base::NoDestructor<ElectronSessionShutdownNotifierFactory>;
  ElectronSessionShutdownNotifierFactory();
  ~ElectronSessionShutdownNotifierFactory() override;
};

}  // namespace xenon::ipc

#endif  // XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_SESSION_SHUTDOWN_NOTIFIER_FACTORY_H_
