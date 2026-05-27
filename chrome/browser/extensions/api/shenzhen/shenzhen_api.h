// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_EXTENSIONS_API_SHENZHEN_SHENZHEN_API_H_
#define CHROME_BROWSER_EXTENSIONS_API_SHENZHEN_SHENZHEN_API_H_

#include "extensions/browser/extension_function.h"

namespace extensions {

class ShenzhenShowPlayerWindowFunction : public ExtensionFunction {
 public:
  DECLARE_EXTENSION_FUNCTION("shenzhen.showPlayerWindow",
                             SHENZHEN_SHOWPLAYERWINDOW)
 protected:
  ~ShenzhenShowPlayerWindowFunction() override = default;

  // ExtensionFunction:
  ResponseAction Run() override;
};

class ShenzhenHidePlayerWindowFunction : public ExtensionFunction {
 public:
  DECLARE_EXTENSION_FUNCTION("shenzhen.hidePlayerWindow",
                             SHENZHEN_HIDEPLAYERWINDOW)
 protected:
  ~ShenzhenHidePlayerWindowFunction() override = default;

  // ExtensionFunction:
  ResponseAction Run() override;
};

class ShenzhenIsPlayerWindowOpenFunction : public ExtensionFunction {
 public:
  DECLARE_EXTENSION_FUNCTION("shenzhen.isPlayerWindowOpen",
                             SHENZHEN_ISPLAYERWINDOWOPEN)
 protected:
  ~ShenzhenIsPlayerWindowOpenFunction() override = default;

  // ExtensionFunction:
  ResponseAction Run() override;
};

class ShenzhenActivatePlayerWindowFunction : public ExtensionFunction {
 public:
  DECLARE_EXTENSION_FUNCTION("shenzhen.activatePlayerWindow",
                             SHENZHEN_ACTIVATEPLAYERWINDOW)
 protected:
  ~ShenzhenActivatePlayerWindowFunction() override = default;

  // ExtensionFunction:
  ResponseAction Run() override;
};

}  // namespace extensions

#endif  // CHROME_BROWSER_EXTENSIONS_API_SHENZHEN_SHENZHEN_API_H_
