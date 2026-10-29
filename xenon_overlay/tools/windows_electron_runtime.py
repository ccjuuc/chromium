#!/usr/bin/env python3
# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Explicit projection of a Windows Electron release onto the Xenon host.

Only root runtime assets are removed. Business addons, player libraries,
licenses, and the original executable (application identity/version metadata)
are retained. This policy is opt-in, never inferred from an application's name.
"""

import struct
from pathlib import PurePosixPath


RUNTIME_FILES = frozenset(name.casefold() for name in (
    'XDASKernel.dll', 'electron.exe', 'electron.manifest',
    'chrome_100_percent.pak', 'chrome_200_percent.pak', 'resources.pak',
    'icudtl.dat', 'snapshot_blob.bin', 'v8_context_snapshot.bin',
    'ffmpeg.dll', 'libEGL.dll', 'libGLESv2.dll',
    'vk_swiftshader.dll', 'vk_swiftshader_icd.json', 'vulkan-1.dll',
))
PROFILES = ('thunder', 'ple')


def excluded(relative, profile='thunder'):
    if profile not in PROFILES:
        raise ValueError(f'Unknown Windows Electron runtime profile: {profile}')
    path = PurePosixPath(str(relative).replace('\\', '/'))
    # PLE's root libplacebo imports Vulkan and can dynamically open EGL/GLES.
    # Preserve both graphics backends, including the OpenGL fallback.
    if profile == 'ple' and len(path.parts) == 1:
        if path.name.casefold() in ('vulkan-1.dll', 'libegl.dll', 'libglesv2.dll'):
            return False
        if path.name.casefold() == 'debug.log':
            return True
    return (len(path.parts) == 1 and path.name.casefold() in RUNTIME_FILES or
            bool(path.parts) and path.parts[0].casefold() == 'locales')


def project_inventory(record, profile='thunder'):
    return {key: [entry for entry in record[key]
                  if not excluded(entry['path'] if key == 'files' else entry,
                                  profile)]
            for key in ('directories', 'files')}


def pe_imports(path):
    """Read regular and delay imports without loading third-party code."""
    data = path.read_bytes()
    if data[:2] != b'MZ':
        raise ValueError(f'Expected a PE image: {path}')

    def read(fmt, offset):
        return struct.unpack_from(fmt, data, offset)

    pe, = read('<I', 0x3c)
    if data[pe:pe + 4] != b'PE\0\0':
        raise ValueError(f'Invalid PE signature: {path}')
    count, = read('<H', pe + 6)
    optional_size, = read('<H', pe + 20)
    optional = pe + 24
    magic, = read('<H', optional)
    if magic not in (0x10b, 0x20b):
        raise ValueError(f'Invalid PE optional header: {path}')
    directory = optional + (112 if magic == 0x20b else 96)
    image_base, = read('<Q' if magic == 0x20b else '<I',
                       optional + (24 if magic == 0x20b else 28))
    directory_count, = read('<I', directory - 4)
    sections = []
    for index in range(count):
        offset = optional + optional_size + index * 40
        virtual_size, virtual, raw_size, raw = read('<IIII', offset + 8)
        sections.append((virtual, max(virtual_size, raw_size), raw))

    def offset(rva):
        for virtual, size, raw in sections:
            if virtual <= rva < virtual + size:
                return raw + rva - virtual
        raise ValueError(f'Unmapped PE RVA {rva:x}: {path}')

    names = []
    for entry, width in ((1, 20), (13, 32)):
        if entry >= directory_count:
            continue
        rva, size = read('<II', directory + entry * 8)
        if not rva:
            continue
        start = offset(rva)
        for position in range(start, min(start + size, len(data)) - width + 1, width):
            descriptor = read('<' + 'I' * (width // 4), position)
            if not any(descriptor):
                break
            name = descriptor[3] if entry == 1 else descriptor[1]
            if entry == 13 and not descriptor[0] & 1:
                name -= image_base
            address = offset(name)
            names.append(data[address:data.index(0, address)].decode('ascii').casefold())
        else:
            raise ValueError(f'Unterminated PE import directory: {path}')
    return names


def validate_projection(root, record, profile='thunder'):
    """Reject reintroduced runtime and retained PEs that need removed DLLs."""
    names = {entry['path'].casefold() for entry in record['files']}
    for entry in record['files']:
        relative = entry['path']
        if excluded(relative, profile):
            raise ValueError(f'Electron runtime remains in hosted payload: {relative}')
        path = PurePosixPath(relative)
        if path.suffix.casefold() not in ('.exe', '.dll', '.node'):
            continue
        try:
            imports = pe_imports(root / relative)
        except (IndexError, UnicodeError, struct.error) as error:
            raise ValueError(f'Malformed retained PE image: {relative}') from error
        for name in imports:
            # A player's private Vulkan/codec copy is independent of Electron.
            candidates = {(path.parent / name).as_posix().casefold(), name}
            if name in RUNTIME_FILES and not candidates.intersection(names):
                raise ValueError(f'Retained native module needs removed runtime: {relative}: {name}')
