#!/usr/bin/env python3
# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Stage only the TH resource bundle and its host-relative Mac descriptor."""

import argparse
import hashlib
import json
import lzma
import os
from pathlib import Path
import plistlib
import posixpath
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unicodedata


sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'build'))
from action_helpers import write_depfile


BUNDLE = 'Thunder.app'
DESCRIPTOR = 'thunder_2025.xenon.json'
HOST_APPLICATION = './Thunder.app'
ADAPTER = {'schemaVersion': 1,
           'adapter': {'id': 'xunlei.mac-player', 'version': 1}}
MAX_ARCHIVE_BYTES = 4 * 1024 * 1024 * 1024
MAX_FILE_BYTES = 2 * 1024 * 1024 * 1024
MAX_EXPANDED_BYTES = 8 * 1024 * 1024 * 1024
MAX_MEMBERS = 50000
MAX_METADATA_BYTES = 64 * 1024


class BoundedTarInfo(tarfile.TarInfo):
    @classmethod
    def frombuf(cls, buf, encoding, errors):
        member = super().frombuf(buf, encoding, errors)
        # GNU sparse extension blocks are parsed before TarFile yields the
        # member. Reject them before their potentially large map is consumed.
        if member.type == tarfile.GNUTYPE_SPARSE:
            raise ValueError('Sparse archive entries are unsupported')
        limit = MAX_METADATA_BYTES if member.type in (
            tarfile.XHDTYPE, tarfile.XGLTYPE, tarfile.SOLARIS_XHDTYPE,
            tarfile.GNUTYPE_LONGNAME, tarfile.GNUTYPE_LONGLINK) else MAX_FILE_BYTES
        if member.size < 0 or member.size > limit:
            raise ValueError('Archive member exceeds the size limit')
        return member

    # tarfile expands PAX sparse maps before yielding a member, so rejecting
    # member.sparse in the extraction loop is too late. The private hooks have
    # different signatures across Python versions; their arguments are unused.
    def _proc_gnusparse_00(self, *_):
        raise ValueError('Sparse archive entries are unsupported')

    def _proc_gnusparse_01(self, *_):
        raise ValueError('Sparse archive entries are unsupported')

    def _proc_gnusparse_10(self, *_):
        raise ValueError('Sparse archive entries are unsupported')


class BoundedReader:
    def __init__(self, stream):
        self.stream = stream
        self.total = 0

    def read(self, size):
        data = self.stream.read(min(size, MAX_EXPANDED_BYTES + 1 - self.total))
        self.total += len(data)
        if self.total > MAX_EXPANDED_BYTES:
            raise ValueError('Archive exceeds the expanded size limit')
        return data


def _member_path(name):
    relative = name[:-1] if name.endswith('/') else name
    parts = relative.split('/')
    if (not relative or '\\' in relative or '\0' in relative or
            any(part in ('', '.', '..') for part in parts) or
            (parts[0] != BUNDLE and relative != DESCRIPTOR)):
        raise ValueError(f'Unsafe or unexpected archive path: {name}')
    return relative


def extract_archive(archive, checksum, destination):
    """Verify the exact compressed bytes, then extract a bounded portable tree."""
    _file(archive)
    _file(checksum)
    if checksum.stat().st_size > 512:
        raise ValueError('Invalid archive checksum file')
    expected = re.fullmatch(r'([0-9a-f]{64})  ' + re.escape(archive.name) + r'\n',
                            checksum.read_text())
    if expected is None:
        raise ValueError('Invalid archive checksum format or filename')
    if destination.exists():
        raise ValueError('Archive extraction destination must not exist')
    with archive.open('rb') as compressed:
        if os.fstat(compressed.fileno()).st_size > MAX_ARCHIVE_BYTES:
            raise ValueError('Archive exceeds the compressed size limit')
        digest = hashlib.sha256()
        for chunk in iter(lambda: compressed.read(1024 * 1024), b''):
            digest.update(chunk)
        if digest.hexdigest() != expected.group(1):
            raise ValueError('Archive SHA256 does not match its checksum')
        compressed.seek(0)
        destination.mkdir()
        # macOS temporary directories can be spelled through /var, while
        # Path.resolve() returns /private/var. Compare the final link graph
        # against the same canonical extraction root.
        destination = destination.resolve(strict=True)
        seen, spellings, links, directories = set(), {}, [], []
        total = 0
        with lzma.LZMAFile(compressed) as expanded:
            with tarfile.open(fileobj=BoundedReader(expanded), mode='r|',
                              tarinfo=BoundedTarInfo) as package:
                for index, member in enumerate(package):
                    if index >= MAX_MEMBERS:
                        raise ValueError('Archive exceeds the entry count limit')
                    relative = _member_path(member.name)
                    identity = unicodedata.normalize('NFC', relative).casefold()
                    if identity in seen:
                        raise ValueError(f'Duplicate archive entry: {relative}')
                    seen.add(identity)
                    parts = relative.split('/')
                    for end in range(1, len(parts) + 1):
                        prefix = '/'.join(parts[:end])
                        folded = unicodedata.normalize('NFC', prefix).casefold()
                        if folded in spellings and spellings[folded] != prefix:
                            raise ValueError(f'Conflicting archive path casing: {relative}')
                        spellings[folded] = prefix
                    if (member.sparse is not None or
                            any(key.startswith('GNU.sparse') for key in member.pax_headers)):
                        raise ValueError(f'Sparse archive entries are unsupported: {relative}')
                    if member.mode & ~0o777:
                        raise ValueError(f'Special archive permissions are unsupported: {relative}')
                    if relative == DESCRIPTOR and not member.isfile():
                        raise ValueError('The descriptor must be a regular file')
                    if relative == BUNDLE and not member.isdir():
                        raise ValueError('Thunder.app must be a directory')
                    if any(relative.startswith(link + '/') for link, _ in links):
                        raise ValueError(f'Archive entry has a symlink parent: {relative}')
                    target = destination / relative
                    target.parent.mkdir(parents=True, exist_ok=True)
                    if member.isdir():
                        target.mkdir(exist_ok=True)
                        directories.append((target, member.mode))
                    elif member.isfile():
                        if member.size < 0 or member.size > MAX_FILE_BYTES:
                            raise ValueError('Archive member exceeds the size limit')
                        total += member.size
                        if total > MAX_EXPANDED_BYTES:
                            raise ValueError('Archive exceeds the expanded size limit')
                        with package.extractfile(member) as original, target.open('xb') as copied:
                            remaining = member.size
                            while remaining:
                                chunk = original.read(min(1024 * 1024, remaining))
                                if not chunk:
                                    raise ValueError(f'Truncated archive member: {relative}')
                                copied.write(chunk)
                                remaining -= len(chunk)
                        target.chmod(member.mode)
                    elif member.issym():
                        link = member.linkname
                        resolved = posixpath.normpath(posixpath.join(
                            posixpath.dirname(relative), link))
                        if (not link or link.startswith('/') or '\\' in link or '\0' in link or
                                not (resolved == BUNDLE or resolved.startswith(BUNDLE + '/')) or
                                target.exists()):
                            raise ValueError(f'Unsafe archive symlink: {relative}')
                        links.append((relative, link))
                    else:
                        raise ValueError(f'Unsupported archive entry type: {relative}')
        # Delay every link until regular files are finished, then verify the
        # final link graph. No archive write can traverse a symbolic link.
        for relative, link in links:
            (destination / relative).symlink_to(link)
        for relative, _ in links:
            try:
                resolved = (destination / relative).resolve(strict=True)
            except (OSError, RuntimeError) as error:
                raise ValueError(f'Invalid archive symlink: {relative}') from error
            if not _within(resolved, destination / BUNDLE):
                raise ValueError(f'Archive symlink escapes the payload: {relative}')
        for directory, mode in reversed(directories):
            directory.chmod(mode)
    inspect_source(destination)


def stage_archive(archive, checksum, destination):
    # Extraction stays outside the committed stage; validation failure leaves
    # the previous generated resources available for inspection or retry.
    with tempfile.TemporaryDirectory(prefix='thunder-archive-') as temporary:
        extracted = Path(temporary) / 'payload'
        extract_archive(archive, checksum, extracted)
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
