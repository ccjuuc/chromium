// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/extensions/api/shenzhen/shenzhen_api.h"

#include "chrome/common/extensions/api/shenzhen.h"

namespace extensions {

ExtensionFunction::ResponseAction
ShenzhenShowPlayerWindowFunction::Run() {
  // TODO(shenzhen): Implement showPlayerWindow.
  return RespondNow(NoArguments());
}

ExtensionFunction::ResponseAction
ShenzhenHidePlayerWindowFunction::Run() {
  // TODO(shenzhen): Implement hidePlayerWindow.
  return RespondNow(NoArguments());
}

ExtensionFunction::ResponseAction
ShenzhenIsPlayerWindowOpenFunction::Run() {
  // TODO(shenzhen): Implement isPlayerWindowOpen.
  return RespondNow(ArgumentList(
      api::shenzhen::IsPlayerWindowOpen::Results::Create(false)));
}

ExtensionFunction::ResponseAction
ShenzhenActivatePlayerWindowFunction::Run() {
  // TODO(shenzhen): Implement activatePlayerWindow.
  return RespondNow(NoArguments());
}

}  // namespace extensions
