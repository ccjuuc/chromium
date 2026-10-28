// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_electron_session_shutdown_notifier_factory.h"

namespace xenon::ipc {

ElectronSessionShutdownNotifierFactory*
ElectronSessionShutdownNotifierFactory::GetInstance() {
  static base::NoDestructor<ElectronSessionShutdownNotifierFactory> factory;
  return factory.get();
}

ElectronSessionShutdownNotifierFactory::ElectronSessionShutdownNotifierFactory()
    : BrowserContextKeyedServiceShutdownNotifierFactory(
          "XenonElectronSessions") {}

ElectronSessionShutdownNotifierFactory::
    ~ElectronSessionShutdownNotifierFactory() = default;

}  // namespace xenon::ipc
