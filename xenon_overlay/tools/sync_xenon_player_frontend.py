#!/usr/bin/env python3
# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Copy xmp_xdas_2 Electron main-renderer build into chrome.exe's
xenon_player/frontend/ directory (or a custom dest).

Example:
  python xenon_overlay/tools/sync_xenon_player_frontend.py \\
      --src F:/xl-player/xmp_xdas_2/app/build/main-renderer \\
      --dst out/release_64/xenon_player/frontend
"""

from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        '--src',
        type=Path,
        default=Path(r'F:/xl-player/xmp_xdas_2/app/build/main-renderer'),
        help='Electron main-renderer build output')
    parser.add_argument(
        '--dst',
        type=Path,
        required=True,
        help='Destination directory (usually <out>/xenon_player/frontend)')
    parser.add_argument(
        '--include-maps',
        action='store_true',
        help='Also copy *.map source maps (large)')
    args = parser.parse_args()

    if not args.src.is_dir():
        print(f'source not found: {args.src}', file=sys.stderr)
        return 1

    if args.dst.exists():
        shutil.rmtree(args.dst)
    args.dst.mkdir(parents=True)

    copied = 0
    skipped = 0
    for path in args.src.rglob('*'):
        if not path.is_file():
            continue
        if not args.include_maps and path.suffix == '.map':
            skipped += 1
            continue
        rel = path.relative_to(args.src)
        out = args.dst / rel
        out.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, out)
        copied += 1

    print(f'copied {copied} files to {args.dst} (skipped maps={skipped})')
    print('Launch Chrome with the frontend next to chrome.exe, or:')
    print(f'  --xenon-player-frontend-dir={args.dst}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
