// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_UI_XENON_MAC_PLAYER_CALLBACKS_H_
#define XENON_OVERLAY_CHROME_BROWSER_UI_XENON_MAC_PLAYER_CALLBACKS_H_

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/containers/span.h"
#include "base/values.h"

namespace xenon {

struct MacPlayerCallbackContract {
  std::string_view method;
  std::string_view provider;
  size_t argument_count;
  // A null factory denotes a notification with no native completion.
  base::ListValue (*failure_arguments)();
};

struct MacPlayerCallbackRegistration {
  int callback_id = 0;
  std::string method;
  std::string provider;
};

struct MacPlayerCompletion {
  int instance_id = 0;
  std::string owner_token;
  base::ListValue failure_args;
};

// Validates registrations against the selected SDK adapter's contract. Unknown
// providers and malformed wires fail without returning partial output.
bool ReadMacPlayerCallbackRegistrations(
    base::span<const MacPlayerCallbackContract> contracts,
    std::string_view method,
    const base::ListValue& args,
    std::vector<MacPlayerCallbackRegistration>* registrations,
    std::string* error);

// Validates the native callback's arity and locates its one-shot completion.
// Notification contracts succeed with an empty optional. All
// completion descriptors must be consumed exactly once, including on owner
// disconnect, before releasing their native instance. A descriptor release
// alone does not resume the SDK's suspended coroutine.
bool ReadMacPlayerCompletion(
    base::span<const MacPlayerCallbackContract> contracts,
    const MacPlayerCallbackRegistration& registration,
    const base::ListValue& args,
    std::optional<MacPlayerCompletion>* completion,
    std::string* error);

// Every request returns a receipt, including validation failures. The IDs are
// callbacks actually adopted by the native executor, not merely transmitted.
base::Value MakeMacPlayerReply(base::Value result,
                               const std::string& error,
                               base::ListValue accepted_callback_ids = {});

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_UI_XENON_MAC_PLAYER_CALLBACKS_H_
