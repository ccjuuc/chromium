#!/usr/bin/env python3
# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Validate externally synced Xenon payloads, then create the installer."""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path


_REQUIRED_PREFIX = '--xenon-required-file='


def main() -> int:
    forwarded_args = []
    required_files = []
    build_dir = None
    args = iter(sys.argv[1:])
    for arg in args:
        if arg.startswith(_REQUIRED_PREFIX):
            required_files.append(arg[len(_REQUIRED_PREFIX):])
            continue
        forwarded_args.append(arg)
        if arg == '--build_dir':
            try:
                build_dir = next(args)
            except StopIteration:
                print('--build_dir requires a value', file=sys.stderr)
                return 2
            forwarded_args.append(build_dir)
        elif arg.startswith('--build_dir='):
            build_dir = arg.partition('=')[2]

    if not build_dir:
        print('--build_dir is required', file=sys.stderr)
        return 2

    missing = [
        path for path in required_files
        if not os.path.isfile(os.path.join(build_dir, path))
    ]
    if missing:
        print(
            'Required Xenon installer payload is missing: ' +
            ', '.join(missing),
            file=sys.stderr)
        return 1

    archive_script = (
        Path(__file__).resolve().parents[2] / 'chrome' / 'tools' / 'build' /
        'win' / 'create_installer_archive.py')
    return subprocess.call(
        [sys.executable, str(archive_script), *forwarded_args])


if __name__ == '__main__':
    sys.exit(main())
