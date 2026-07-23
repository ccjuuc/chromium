// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_APP_PLAYER_CONTAINER_PROCESS_WIN_H_
#define XENON_OVERLAY_CHROME_APP_PLAYER_CONTAINER_PROCESS_WIN_H_

#include <optional>

namespace base {
class CommandLine;
}

namespace xenon {

// Runs the native player container when the command line is a player child
// process request. Returns the container exit code, or nullopt for normal Xenon
// process startup.
std::optional<int> MaybeRunPlayerContainerProcess(
    const base::CommandLine& command_line);

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_APP_PLAYER_CONTAINER_PROCESS_WIN_H_
