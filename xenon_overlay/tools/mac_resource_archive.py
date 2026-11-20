#!/usr/bin/env python3
# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Verify and extract a macOS resource bundle and its optional descriptor."""

import hashlib
import lzma
import os
import posixpath
import re
import tarfile
import unicodedata


MAX_ARCHIVE_BYTES = 4 * 1024 * 1024 * 1024
MAX_FILE_BYTES = 2 * 1024 * 1024 * 1024
MAX_EXPANDED_BYTES = 8 * 1024 * 1024 * 1024
MAX_MEMBERS = 50000
MAX_METADATA_BYTES = 64 * 1024


class BoundedTarInfo(tarfile.TarInfo):
    def _proc_member(self, archive):
        # GNU sparse extension blocks are parsed before TarFile yields the
        # member. Reject them before their potentially large map is consumed.
        if self.type == tarfile.GNUTYPE_SPARSE:
            raise ValueError('Sparse archive entries are unsupported')
        limit = MAX_METADATA_BYTES if self.type in (
            tarfile.XHDTYPE, tarfile.XGLTYPE, tarfile.SOLARIS_XHDTYPE,
            tarfile.GNUTYPE_LONGNAME, tarfile.GNUTYPE_LONGLINK) else MAX_FILE_BYTES
        if self.size < 0 or self.size > limit:
            raise ValueError('Archive member exceeds the size limit')
        return super()._proc_member(archive)

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


def _member_path(name, bundle, descriptor):
    relative = name[:-1] if name.endswith('/') else name
    parts = relative.split('/')
    if (not relative or '\\' in relative or '\0' in relative or
            any(part in ('', '.', '..') for part in parts) or
            (parts[0] != bundle and relative != descriptor)):
        raise ValueError(f'Unsafe or unexpected archive path: {name}')
    return relative


def extract_archive(archive, checksum, destination, bundle, descriptor=None):
    """Verify the exact compressed bytes, then extract a bounded portable tree."""
    for path in (archive, checksum):
        if path.is_symlink() or not path.is_file():
            raise ValueError(f'Expected a regular file: {path}')
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
                    relative = _member_path(member.name, bundle, descriptor)
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
                    if relative == descriptor and not member.isfile():
                        raise ValueError('The descriptor must be a regular file')
                    if relative == bundle and not member.isdir():
                        raise ValueError(f'{bundle} must be a directory')
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
                                not (resolved == bundle or resolved.startswith(bundle + '/')) or
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
            if not resolved.is_relative_to(destination / bundle):
                raise ValueError(f'Archive symlink escapes the payload: {relative}')
        for directory, mode in reversed(directories):
            directory.chmod(mode)
