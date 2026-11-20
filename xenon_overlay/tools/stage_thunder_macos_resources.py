#!/usr/bin/env python3
# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Stage only the TH resource bundle and its host-relative Mac descriptor."""

import argparse
import json
import lzma
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys
import tarfile
import tempfile

from mac_resource_archive import extract_archive


sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'build'))
from action_helpers import write_depfile


BUNDLE = 'Thunder.app'
DESCRIPTOR = 'thunder_2025.xenon.json'
HOST_APPLICATION = './Thunder.app'
ADAPTER = {'schemaVersion': 1,
           'adapter': {'id': 'xunlei.mac-player', 'version': 1}}


def stage_archive(archive, checksum, destination):
    # Extraction stays outside the committed stage; validation failure leaves
    # the previous generated resources available for inspection or retry.
    with tempfile.TemporaryDirectory(prefix='thunder-archive-') as temporary:
        extracted = Path(temporary) / 'payload'
        extract_archive(archive, checksum, extracted, BUNDLE, DESCRIPTOR)
        stage_resources(extracted, destination)
    return [archive.resolve(), checksum.resolve()]


def _within(path, root):
    return path == root or root in path.parents


def _directory(path):
    if path.is_symlink() or not path.is_dir():
        raise ValueError(f'Expected a real directory: {path}')


def _file(path):
    if path.is_symlink() or not path.is_file():
        raise ValueError(f'Expected a regular file: {path}')


def inspect_source(source):
    """Validate first; directory dependencies rerun staging on additions/removals."""
    _directory(source)
    source = source.resolve(strict=True)
    bundle, descriptor = source / BUNDLE, source / DESCRIPTOR
    for relative in ('', 'Contents', 'Contents/MacOS', 'Contents/Resources',
                     'Contents/Resources/app'):
        _directory(bundle / relative)
    info = bundle / 'Contents/Info.plist'
    declaration = bundle / 'Contents/Resources/xenon-native-player.json'
    for path in (descriptor, info, declaration, bundle / 'Contents/Resources/app/out.asar'):
        _file(path)
    identity = plistlib.loads(info.read_bytes())
    if (not isinstance(identity, dict) or
            identity.get('CFBundlePackageType') != 'BNDL' or 'CFBundleExecutable' in identity):
        raise ValueError('Thunder.app must be a resource BNDL without an executable')
    if (bundle / 'Contents/Frameworks/Electron Framework.framework').exists():
        raise ValueError('Thunder.app must not contain the Electron runtime')
    if json.loads(declaration.read_text()) != ADAPTER:
        raise ValueError('Unsupported native player adapter declaration')
    config = json.loads(descriptor.read_text())
    if (not isinstance(config, dict) or config.get('application') != './Thunder.app' or
            'executable' in config or config.get('main') != 'out.asar/main.js'):
        raise ValueError('Expected the exported resource descriptor for ./Thunder.app')
    config['application'] = HOST_APPLICATION
    # Older exported resource bundles predate the explicit compatibility
    # policy. Preserve this existing integration's files when staging them;
    # the generic runtime itself has no application-name exceptions.
    config.setdefault('userDataPolicy', 'legacy-host')

    inputs = [descriptor, bundle]
    for directory, directories, files in os.walk(bundle, followlinks=False):
        for name in sorted(directories + files):
            item = Path(directory) / name
            if item.is_symlink():
                # Absolute links would still reference the source after copying.
                if os.path.isabs(os.readlink(item)) or not _within(
                        item.resolve(strict=True), bundle):
                    raise ValueError(f'Non-relocatable bundle symlink: {item}')
            elif not item.is_file() and not item.is_dir():
                raise ValueError(f'Unsupported bundle entry: {item}')
            inputs.append(item)
    return bundle, config, inputs


def _copy_bundle(source, destination):
    if sys.platform == 'darwin':
        # APFS clones are independent inodes and preserve the framework links.
        cloned = subprocess.run(['cp', '-cRp', str(source), str(destination)],
                                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        if cloned.returncode == 0:
            return
        if destination.exists():
            shutil.rmtree(destination)
    shutil.copytree(source, destination, symlinks=True)


def stage_resources(source, destination):
    bundle, descriptor, inputs = inspect_source(source)
    source = source.resolve(strict=True)
    # Do not follow a generated output or its ancestors into another directory.
    destination = Path(os.path.abspath(destination))
    for candidate in (*reversed(destination.parents), destination):
        if candidate.is_symlink():
            raise ValueError(f'Linked staging destination: {candidate}')
    destination = destination.resolve()
    if _within(destination, source) or _within(source, destination):
        raise ValueError('Source and staging directories must not overlap')
    if destination.exists():
        _directory(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = Path(tempfile.mkdtemp(prefix='.thunder-resources-', dir=destination.parent))
    staged, previous = temporary / 'payload', temporary / 'previous'
    committed = False
    preserve_temporary = False
    try:
        staged.mkdir()
        _copy_bundle(bundle, staged / BUNDLE)
        (staged / DESCRIPTOR).write_text(json.dumps(descriptor, indent=2) + '\n')
        # bundle_data consumes the directory as one input. Touch its root even
        # for changes deep inside ASAR/plugin trees so Ninja recopies the bundle.
        os.utime(staged / BUNDLE, None)
        if destination.exists():
            destination.rename(previous)
        try:
            staged.rename(destination)
            committed = True
        except BaseException:
            if previous.exists():
                try:
                    previous.rename(destination)
                except BaseException as rollback_error:
                    preserve_temporary = True
                    raise RuntimeError(f'Staging rollback failed; prior output is in {previous}') from rollback_error
            raise
        return inputs
    finally:
        if not preserve_temporary:
            # Once committed, failure to delete an obsolete backup cannot make
            # an installed stage invalid. It is safe to retain it for inspection.
            try:
                shutil.rmtree(temporary)
            except OSError:
                if not committed:
                    raise
                print(f'Warning: obsolete staging backup retained at {temporary}', file=sys.stderr)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    inputs = parser.add_mutually_exclusive_group(required=True)
    inputs.add_argument('--source', type=Path, help='Unpacked fixture/source directory')
    inputs.add_argument('--archive', type=Path, help='Verified tar.xz resource archive')
    parser.add_argument('--checksum', type=Path)
    parser.add_argument('--destination', required=True, type=Path)
    parser.add_argument('--stamp', required=True, type=Path)
    parser.add_argument('--depfile', required=True, type=Path)
    options = parser.parse_args(argv)
    try:
        if options.archive:
            if not options.checksum:
                raise ValueError('--archive requires --checksum')
            inputs = stage_archive(options.archive, options.checksum, options.destination)
        else:
            if options.checksum:
                raise ValueError('--checksum requires --archive')
            inputs = stage_resources(options.source, options.destination)
        write_depfile(str(options.depfile), os.path.relpath(options.stamp),
                      [os.path.relpath(path) for path in inputs])
        options.stamp.parent.mkdir(parents=True, exist_ok=True)
        options.stamp.write_text('Staged Thunder resource bundle and host descriptor.\n')
        return 0
    except (OSError, ValueError, tarfile.TarError, lzma.LZMAError) as error:
        print(f'Error: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
