#!/usr/bin/env python3
# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Remove reserved outputs when a Mac build disables an application group."""

import argparse
import os
from pathlib import Path
import shutil
import sys


RESOURCE_GROUPS = {
    'thunder': [('Thunder.app', 'thunder_2025.xenon.json')],
    'mac-electron-apps': [
        ('draw.io.app', 'drawio.xenon.json'),
        ('MarkText.app', 'marktext.xenon.json'),
        ('Motrix.app', 'motrix.xenon.json'),
    ],
}


def configure_bundle(bundle, enabled, resource_group='thunder'):
    # This target remains in the graph in both modes. A changed command line
    # makes Ninja rerun it on every enable/disable transition, even when the
    # disabled-mode stamp from an older build still exists.
    bundle = Path(os.path.abspath(bundle))
    # Contents/MacOS contains signed executable code. Remove the legacy JSON
    # location in both modes before the finished app is signed.
    resources = RESOURCE_GROUPS[resource_group]
    targets = [bundle / 'Contents/MacOS' / descriptor
               for _, descriptor in resources]
    aliases = []
    if resource_group == 'mac-electron-apps':
        aliases = [(bundle / 'Contents/Resources/xenon-apps' /
                    descriptor[:-len('.xenon.json')], application)
                   for application, descriptor in resources]
    if not enabled:
        targets += [bundle / 'Contents/Resources/xenon-apps' / name
                    for application, descriptor in resources
                    for name in (application, descriptor)]
    for target in targets + [alias for alias, _ in aliases]:
        for parent in target.parents:
            if parent.is_symlink():
                raise ValueError(f'Linked bundle ancestor: {parent}')
    # Old local deployments used lowercase aliases. Remove only the exact
    # links created by that deployment, preserving other files or link targets.
    for alias, application in aliases:
        if alias.is_symlink() and os.readlink(alias) == application:
            alias.unlink()
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
    parser.add_argument('--resource-group', choices=RESOURCE_GROUPS,
                        default='thunder')
    parser.add_argument('--bundle', required=True, type=Path)
    parser.add_argument('--enabled', required=True, choices=('true', 'false'))
    parser.add_argument('--stamp', required=True, type=Path)
    options = parser.parse_args(argv)
    try:
        configure_bundle(options.bundle, options.enabled == 'true',
                         options.resource_group)
        options.stamp.parent.mkdir(parents=True, exist_ok=True)
        label = ('Thunder' if options.resource_group == 'thunder'
                 else 'Mac Electron')
        options.stamp.write_text(f'{label} resources enabled: {options.enabled}\n')
        return 0
    except (OSError, ValueError) as error:
        print(f'Error: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
