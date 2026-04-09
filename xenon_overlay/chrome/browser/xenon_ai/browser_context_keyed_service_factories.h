// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_XENON_AI_BROWSER_CONTEXT_KEYED_SERVICE_FACTORIES_H_
#define XENON_OVERLAY_CHROME_BROWSER_XENON_AI_BROWSER_CONTEXT_KEYED_SERVICE_FACTORIES_H_

namespace xenon {

// Invoked from ChromeBrowserMainExtraPartsProfiles::
// EnsureBrowserContextKeyedServiceFactoriesBuilt().
void EnsureXenonBrowserContextKeyedServiceFactoriesBuilt();

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_XENON_AI_BROWSER_CONTEXT_KEYED_SERVICE_FACTORIES_H_
