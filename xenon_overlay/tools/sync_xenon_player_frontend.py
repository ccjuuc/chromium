#!/usr/bin/env python3
# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Retired: frontend-only imports cannot preserve an Electron release layout."""

import sys


def main(argv: list[str] | None = None) -> int:
    print('error: frontend-only synchronization is retired. Import a complete '
          'release with import_electron_app.py --src <complete-release> '
          '--out <application-directory>, or sync_xenon_player.py '
          '--src <complete-release> --out <parent-directory>.', file=sys.stderr)
    return 2


if __name__ == '__main__':
    sys.exit(main())
