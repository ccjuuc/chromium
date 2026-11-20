#!/usr/bin/env python3
# Copyright 2026 The Xenon Overlay Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Stage a verified Electron application resource bundle for GN bundle_data."""

import argparse
import os
from pathlib import Path
import plistlib
import tempfile

from mac_resource_archive import extract_archive


def stage(archive, checksum, output):
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output.parent) as temp:
        extracted = Path(temp) / 'extracted'
        extract_archive(archive, checksum, extracted, output.name)
        bundle = extracted / output.name
        contents = bundle / 'Contents'
        info = plistlib.loads((contents / 'Info.plist').read_bytes())
        # Xenon starts the app main process with Contents/MacOS as its cwd,
        # including when removing the upstream launcher leaves that dir empty.
        if not (contents / 'MacOS').is_dir():
            raise ValueError(f'Missing application working directory: {contents / "MacOS"}')
        if (info.get('CFBundlePackageType') != 'BNDL' or
                'CFBundleExecutable' in info or
                not (contents / 'Resources/app.asar').is_file()):
            raise ValueError(f'Expected a hosted Electron resource bundle: {bundle}')

        # Ninja uses the directory mtime to decide whether to copy the bundle.
        os.utime(bundle, None)
        previous = Path(temp) / 'previous'
        if output.exists():
            output.rename(previous)
        try:
            bundle.rename(output)
        except OSError:
            if previous.exists():
                previous.rename(output)
            raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archive', required=True, type=Path)
    parser.add_argument('--checksum', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    stage(args.archive, args.checksum, args.output)


if __name__ == '__main__':
    main()
