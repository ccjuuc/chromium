// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_SYSTEM_LANGUAGES_H_
#define XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_SYSTEM_LANGUAGES_H_

#include <string>
#include <vector>

#include "base/types/expected.h"

namespace xenon::ipc {

// Reads the user's ordered OS language preferences, independently of the
// application's locale. An empty list is valid for a Linux C-only environment.
base::expected<std::vector<std::string>, std::string>
GetPreferredSystemLanguages();

}  // namespace xenon::ipc

#endif  // XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_SYSTEM_LANGUAGES_H_
