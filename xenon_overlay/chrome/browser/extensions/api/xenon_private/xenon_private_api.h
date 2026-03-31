// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_EXTENSIONS_API_XENON_PRIVATE_XENON_PRIVATE_API_H_
#define XENON_OVERLAY_CHROME_BROWSER_EXTENSIONS_API_XENON_PRIVATE_XENON_PRIVATE_API_H_

#include "extensions/browser/extension_function.h"

namespace extensions {

class XenonPrivatePingFunction : public ExtensionFunction {
  DECLARE_EXTENSION_FUNCTION("xenonPrivate.ping", XENONPRIVATE_PING)
 protected:
  ~XenonPrivatePingFunction() override = default;

  // ExtensionFunction:
  ResponseAction Run() override;
};

}  // namespace extensions

#endif  // XENON_OVERLAY_CHROME_BROWSER_EXTENSIONS_API_XENON_PRIVATE_XENON_PRIVATE_API_H_
