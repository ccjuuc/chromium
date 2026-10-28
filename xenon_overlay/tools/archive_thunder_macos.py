#!/usr/bin/env python3
# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Create a reproducible, relocatable archive of the two Mac TH runtime inputs."""

import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import plistlib
import shutil
import stat
import sys
import tarfile
import tempfile
import uuid


BUNDLE = 'Thunder.app'
DESCRIPTOR = 'thunder_2025.xenon.json'


class PublicationRollbackError(RuntimeError):
    """Publication failed and recovery files must be retained for inspection."""


def sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def _inside(path, root):
    return path == root or root in path.parents


def checked_output(path):
    path = Path(os.path.abspath(path))
    for ancestor in (*reversed(path.parents), path):
        if ancestor.is_symlink():
            raise ValueError(f'Linked publication path: {ancestor}')
    return path


def archive_paths(output):
    return (output.with_name(output.name + '.tar.xz'),
            output.with_name(output.name + '.sha256'))


def _regular_file(path):
    metadata = path.lstat()
    if not stat.S_ISREG(metadata.st_mode) or metadata.st_nlink != 1:
        raise ValueError(f'Expected an independent regular file: {path}')


def _payload_entries(source):
    if source.is_symlink() or not source.is_dir():
        raise ValueError('Archive source must be a real directory')
    source = source.resolve(strict=True)
    bundle = source / BUNDLE
    for relative in ('', 'Contents', 'Contents/MacOS', 'Contents/Resources',
                     'Contents/Resources/app'):
        path = bundle / relative
        if path.is_symlink() or not path.is_dir():
            raise ValueError(f'Expected a bundle directory: {path}')
    info = bundle / 'Contents/Info.plist'
    declaration = bundle / 'Contents/Resources/xenon-native-player.json'
    descriptor = source / DESCRIPTOR
    for path in (info, declaration, descriptor, bundle / 'Contents/Resources/app/out.asar'):
        _regular_file(path)
    identity = plistlib.loads(info.read_bytes())
    if (not isinstance(identity, dict) or
            identity.get('CFBundlePackageType') != 'BNDL' or 'CFBundleExecutable' in identity):
        raise ValueError('Expected a TH resource bundle without an executable')
    if (bundle / 'Contents/Frameworks/Electron Framework.framework').exists():
        raise ValueError('TH resource bundle must not contain Electron')
    if json.loads(declaration.read_text()) != {
            'schemaVersion': 1, 'adapter': {'id': 'xunlei.mac-player', 'version': 1}}:
        raise ValueError('Unsupported player adapter declaration')
    config = json.loads(descriptor.read_text())
    if (not isinstance(config, dict) or config.get('application') != './Thunder.app' or
            config.get('main') != 'out.asar/main.js' or 'executable' in config):
        raise ValueError('Expected a relative resource descriptor for ./Thunder.app')

    entries = []

    def visit(path):
        metadata = path.lstat()
        relative = path.relative_to(source).as_posix()
        mode = stat.S_IMODE(metadata.st_mode)
        if mode & 0o7000:
            raise ValueError(f'Special permission bits are not supported: {relative}')
        entry = {'name': relative, 'mode': mode}
        if stat.S_ISLNK(metadata.st_mode):
            target = os.readlink(path)
            if os.path.isabs(target) or not _inside(path.resolve(strict=True), bundle):
                raise ValueError(f'Non-relocatable bundle link: {relative}')
            entry.update(kind='link', target=target)
        elif stat.S_ISDIR(metadata.st_mode):
            entry.update(kind='directory')
        elif stat.S_ISREG(metadata.st_mode) and metadata.st_nlink == 1:
            entry.update(kind='file', size=metadata.st_size, sha256=sha256(path))
        else:
            raise ValueError(f'Unsupported entry or hardlink: {relative}')
        entries.append(entry)
        if entry['kind'] == 'directory':
            for child in sorted(path.iterdir()):
                visit(child)

    visit(bundle)
    visit(descriptor)
    return source, entries


def _verify_archive(path, entries):
    expected = {entry['name']: entry for entry in entries}
    with tarfile.open(path, 'r:xz') as archive:
        members = archive.getmembers()
        if [member.name for member in members] != list(expected):
            raise ValueError('Archive members differ from the inspected payload')
        for member in members:
            entry = expected[member.name]
            if ((member.uid, member.gid, member.uname, member.gname, member.mtime) !=
                    (0, 0, '', '', 0) or member.mode != entry['mode']):
                raise ValueError(f'Archive metadata changed: {member.name}')
            if entry['kind'] == 'directory':
                valid = member.isdir()
            elif entry['kind'] == 'link':
                valid = member.issym() and member.linkname == entry['target']
            else:
                valid = member.isfile() and member.size == entry['size']
                if valid:
                    digest = hashlib.sha256()
                    with archive.extractfile(member) as stream:
                        for block in iter(lambda: stream.read(1024 * 1024), b''):
                            digest.update(block)
                    valid = digest.hexdigest() == entry['sha256']
            if not valid:
                raise ValueError(f'Archive payload changed: {member.name}')


def create_archive(source, archive, checksum):
    """Write new staging files; validate bytes and links before publication."""
    archive, checksum = checked_output(archive), checked_output(checksum)
    source, entries = _payload_entries(source)
    if (archive == checksum or archive.parent != checksum.parent or
            any(_inside(path, source) for path in (archive, checksum))):
        raise ValueError('Archive and checksum must be distinct sibling files outside source')
    if '\n' in archive.name or '\r' in archive.name:
        raise ValueError('Archive filename must fit a checksum line')
    if archive.exists() or checksum.exists():
        raise ValueError('Archive staging files must not already exist')
    archive.parent.mkdir(parents=True, exist_ok=True)
    created = {}
    try:
        # Hand-built headers exclude xattrs, host ownership and local timestamps.
        # Mach-O signatures and CodeResources stay intact as ordinary file bytes.
        with archive.open('xb') as stream:
            metadata = os.fstat(stream.fileno())
            created[archive] = (metadata.st_dev, metadata.st_ino)
            with tarfile.open(fileobj=stream, mode='w:xz', format=tarfile.PAX_FORMAT,
                              preset=6) as payload:
                for entry in entries:
                    header = tarfile.TarInfo(entry['name'])
                    header.mode = entry['mode']
                    header.uid = header.gid = header.mtime = 0
                    header.uname = header.gname = ''
                    if entry['kind'] == 'directory':
                        header.type = tarfile.DIRTYPE
                        payload.addfile(header)
                    elif entry['kind'] == 'link':
                        header.type = tarfile.SYMTYPE
                        header.linkname = entry['target']
                        payload.addfile(header)
                    else:
                        header.size = entry['size']
                        original = source / entry['name']
                        _regular_file(original)
                        with original.open('rb') as contents:
                            payload.addfile(header, contents)
        _verify_archive(archive, entries)
        if _payload_entries(source)[1] != entries:
            raise ValueError('Payload changed while creating archive')
        digest = sha256(archive)
        with checksum.open('x', encoding='ascii') as stream:
            metadata = os.fstat(stream.fileno())
            created[checksum] = (metadata.st_dev, metadata.st_ino)
            stream.write(f'{digest}  {archive.name}\n')
        return {'sha256': digest, 'bytes': archive.stat().st_size,
                'entries': len(entries), 'format': 'tar.xz',
                'roots': [BUNDLE, DESCRIPTOR]}
    except BaseException:
        for path, identity in created.items():
            try:
                metadata = path.lstat()
                if (metadata.st_dev, metadata.st_ino) == identity:
                    path.unlink()
            except FileNotFoundError:
                pass
        raise


def validate_existing_pair(archive, checksum):
    archive, checksum = checked_output(archive), checked_output(checksum)
    if archive.exists() != checksum.exists():
        raise ValueError('Existing archive and checksum must both be present or both absent')
    if archive.exists():
        _regular_file(archive)
        _regular_file(checksum)
        expected = f'{sha256(archive)}  {archive.name}\n'
        if checksum.read_text(encoding='ascii') != expected:
            raise ValueError('Existing archive checksum does not match; refusing to overwrite')


def publish_outputs(pairs):
    """Rename a set together; restore every prior output on a publication error."""
    pairs = [(checked_output(staged), checked_output(output)) for staged, output in pairs]
    paths = [path for pair in pairs for path in pair]
    if len(set(paths)) != len(paths) or any(
            first in second.parents for first in paths for second in paths if first != second):
        raise ValueError('Publication paths must be distinct and non-overlapping')
    for staged, output in pairs:
        if not staged.exists():
            raise ValueError(f'Missing staged publication: {staged}')
        if not output.parent.is_dir():
            raise ValueError(f'Missing publication parent: {output.parent}')
    backups, installed = [], []
    try:
        for staged, output in pairs:
            if output.exists():
                backup = output.with_name(output.name + '.previous-' + uuid.uuid4().hex)
                output.rename(backup)
                backups.append((output, backup))
            staged.rename(output)
            installed.append((staged, output))
    except BaseException as original:
        failures = []
        for staged, output in reversed(installed):
            try:
                output.rename(staged)
            except OSError as error:
                failures.append(str(error))
        for output, backup in reversed(backups):
            try:
                # A failed reversal may leave the new output in place. Keep its
                # backup intact instead of replacing either recovery artifact.
                if output.exists() or output.is_symlink():
                    raise OSError(f'Publication path is still occupied: {output}')
                backup.rename(output)
            except OSError as error:
                failures.append(f'{backup}: {error}')
        if failures:
            raise PublicationRollbackError(
                'Publication rollback incomplete; preserve staging and backups: ' +
                '; '.join(failures)) from original
        raise
    for _, backup in backups:
        try:
            if backup.is_dir():
                shutil.rmtree(backup)
            else:
                backup.unlink()
        except OSError:
            print(f'Warning: publication succeeded; previous output retained: {backup}',
                  file=sys.stderr)


def archive_and_publish(source, archive, checksum):
    source = Path(os.path.abspath(source))
    if source.is_symlink():
        raise ValueError('Archive source must be a real directory')
    source = source.resolve(strict=True)
    archive, checksum = checked_output(archive), checked_output(checksum)
    if archive.parent != checksum.parent or any(
            _inside(path, source) for path in (archive, checksum)):
        raise ValueError('Archive outputs must be siblings outside the source directory')
    validate_existing_pair(archive, checksum)
    archive.parent.mkdir(parents=True, exist_ok=True)
    temporary = Path(tempfile.mkdtemp(prefix='.thunder-archive-', dir=archive.parent))
    preserve = False
    try:
        staged_archive, staged_checksum = temporary / archive.name, temporary / checksum.name
        result = create_archive(source, staged_archive, staged_checksum)
        publish_outputs([(staged_archive, archive), (staged_checksum, checksum)])
        return result
    except PublicationRollbackError:
        preserve = True
        print(f'Archive recovery files retained: {temporary}', file=sys.stderr)
        raise
    finally:
        if not preserve:
            try:
                shutil.rmtree(temporary)
            except OSError:
                print(f'Warning: archive staging directory retained: {temporary}', file=sys.stderr)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', required=True, type=Path)
    parser.add_argument('--archive', required=True, type=Path)
    parser.add_argument('--checksum', required=True, type=Path)
    options = parser.parse_args(argv)
    try:
        archive = checked_output(options.archive)
        archive.parent.mkdir(parents=True, exist_ok=True)
        stem = archive.name[:-7] if archive.name.endswith('.tar.xz') else archive.name
        lock_path = archive.parent / ('.' + stem + '.build.lock')
        checked_output(lock_path)
        with lock_path.open('a') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            result = archive_and_publish(options.source, archive, options.checksum)
        print(json.dumps(result, indent=2))
        return 0
    except (OSError, ValueError, RuntimeError, tarfile.TarError) as error:
        print(f'Archive failed: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
