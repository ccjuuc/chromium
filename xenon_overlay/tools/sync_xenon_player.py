#!/usr/bin/env python3
# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Compatibility entry point for importing a complete Player Electron release.

Use --src <complete-release> --out <parent-containing-xenon_player>.
This command no longer assembles SDKs, decodes preloads or mirrors native addons.
"""

import sys

from import_electron_app import legacy_import_main


def main(argv: list[str] | None = None) -> int:
    return legacy_import_main('xenon_player', argv)


if __name__ == '__main__':
    sys.exit(main())
