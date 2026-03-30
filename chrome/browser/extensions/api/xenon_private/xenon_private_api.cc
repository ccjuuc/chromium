// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/extensions/api/xenon_private/xenon_private_api.h"

#include <string>

#include "chrome/common/extensions/api/xenon_private.h"

namespace extensions {

namespace xenon_private = api::xenon_private;

ExtensionFunction::ResponseAction XenonPrivatePingFunction::Run() {
  // `ping` has no JSON parameters; the schema compiler does not emit
  // `Ping::Params`, only `Ping::Results` for the async callback.
  return RespondNow(ArgumentList(
      xenon_private::Ping::Results::Create(std::string("xenon-private-pong"))));
}

}  // namespace extensions
