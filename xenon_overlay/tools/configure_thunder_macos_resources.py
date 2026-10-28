#!/usr/bin/env python3
# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Remove reserved TH outputs when a Mac build disables the optional payload."""

import argparse
import os
from pathlib import Path
import shutil
import sys


def configure_bundle(bundle, enabled):
    # This target remains in the graph in both modes. A changed command line
    # makes Ninja rerun it on every enable/disable transition, even when the
    # disabled-mode stamp from an older build still exists.
    bundle = Path(os.path.abspath(bundle))
    # Contents/MacOS contains signed executable code. Remove the legacy JSON
    # location in both modes before the finished app is signed.
    targets = [bundle / 'Contents/MacOS/thunder_2025.xenon.json']
    if not enabled:
        targets += [bundle / 'Contents/Resources/xenon-apps/Thunder.app',
                    bundle / 'Contents/Resources/xenon-apps/thunder_2025.xenon.json']
    for target in targets:
        for parent in target.parents:
            if parent.is_symlink():
                raise ValueError(f'Linked bundle ancestor: {parent}')
    # Only these current/legacy reserved entries belong to this resource target.
    # Unlink a leaf symlink itself; never recursively follow it outside the app.
    for target in targets:
        if target.is_symlink():
            target.unlink()
        elif target.is_dir():
            shutil.rmtree(target)
        elif target.exists():
            target.unlink()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bundle', required=True, type=Path)
    parser.add_argument('--enabled', required=True, choices=('true', 'false'))
    parser.add_argument('--stamp', required=True, type=Path)
    options = parser.parse_args(argv)
    try:
        configure_bundle(options.bundle, options.enabled == 'true')
        options.stamp.parent.mkdir(parents=True, exist_ok=True)
        options.stamp.write_text(f'Thunder resources enabled: {options.enabled}\n')
        return 0
    except (OSError, ValueError) as error:
        print(f'Error: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
