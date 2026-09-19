#!/usr/bin/env python3
# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Compatibility entry point for importing a complete Thunder release.

Use --src <complete-release> --out <parent-containing-thunder_2025>.
This command no longer assembles SDKs, repacks archives or splits renderers.
"""

import sys

from import_electron_app import legacy_import_main


def main(argv: list[str] | None = None) -> int:
    return legacy_import_main('thunder_2025', argv)


if __name__ == '__main__':
    sys.exit(main())
