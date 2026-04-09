// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/xenon_ai/browser_context_keyed_service_factories.h"

#include "xenon_overlay/buildflags/buildflags.h"

#if BUILDFLAG(ENABLE_XENON_SERVICE) && BUILDFLAG(ENABLE_XENON_AI)
#include "xenon_overlay/chrome/browser/xenon_ai/xenon_ai_service_factory.h"
#endif

namespace xenon {

void EnsureXenonBrowserContextKeyedServiceFactoriesBuilt() {
#if BUILDFLAG(ENABLE_XENON_SERVICE) && BUILDFLAG(ENABLE_XENON_AI)
  // `ProfileKeyedServiceFactory::GetInstance()` registers this factory with
  // Chrome's keyed-service / dependency manager. It does not construct
  // `XenonAiService`; that happens lazily on the first
  // `XenonAiServiceFactory::GetForProfile(profile)` (or equivalent).
  XenonAiServiceFactory::GetInstance();
#endif
}

}  // namespace xenon
