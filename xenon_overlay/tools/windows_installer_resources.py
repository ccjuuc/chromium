# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Read installer PE resources without executing installer code."""

import hashlib
import mmap
import struct

CHUNK_SIZE = 8 * 1024 * 1024


def packed_resources(path):
    """Read the installer's B7/CHROME.PACKED.7Z PE resources using stdlib."""
    with path.open("rb") as stream, mmap.mmap(
            stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
        def read(fmt, offset):
            size = struct.calcsize(fmt)
            if offset < 0 or offset + size > len(data):
                raise ValueError("Truncated PE structure")
            return struct.unpack_from(fmt, data, offset)

        if data[:2] != b"MZ":
            raise ValueError("Installer has no DOS signature")
        pe_offset, = read("<I", 0x3c)
        if data[pe_offset:pe_offset + 4] != b"PE\0\0":
            raise ValueError("Installer has no PE signature")
        section_count, = read("<H", pe_offset + 6)
        optional_size, = read("<H", pe_offset + 20)
        optional = pe_offset + 24
        magic, = read("<H", optional)
        if magic == 0x20b:
            directory_start, directory_count_offset = 112, 108
        elif magic == 0x10b:
            directory_start, directory_count_offset = 96, 92
        else:
            raise ValueError("Unsupported PE optional-header magic")
        if optional_size < directory_start + 24:
            raise ValueError("PE optional header has no resource directory")
        directory_count, = read("<I", optional + directory_count_offset)
        if directory_count <= 2 or not 0 < section_count <= 96:
            raise ValueError("Invalid PE data-directory or section count")
        resource_rva, resource_size = read("<II", optional + directory_start + 16)
        header_size, = read("<I", optional + 60)
        sections = []
        for index in range(section_count):
            section = optional + optional_size + 40 * index
            virtual_address, raw_size, raw_offset = read("<III", section + 12)
            sections.append((virtual_address, raw_size, raw_offset))

        def rva_to_offset(rva, size):
            if 0 <= rva < header_size and rva + size <= header_size:
                offset = rva
            else:
                matches = [raw + rva - virtual
                           for virtual, raw_size, raw in sections
                           if virtual <= rva and rva + size <= virtual + raw_size]
                if len(matches) != 1:
                    raise ValueError("PE resource RVA is outside raw sections")
                offset = matches[0]
            if size < 0 or offset + size > len(data):
                raise ValueError("PE resource exceeds the installer file")
            return offset

        if not resource_rva or resource_size < 16:
            raise ValueError("Installer has no PE resources")
        base = rva_to_offset(resource_rva, resource_size)

        def resource_offset(relative, size):
            if relative < 0 or relative + size > resource_size:
                raise ValueError("PE resource directory offset is invalid")
            return base + relative

        def resource_name(value):
            if not value & 0x80000000:
                return value
            relative = value & 0x7fffffff
            count, = read("<H", resource_offset(relative, 2))
            offset = resource_offset(relative + 2, count * 2)
            return data[offset:offset + count * 2].decode("utf-16le")

        matches = []

        def walk(relative, labels, ancestors):
            if relative in ancestors or len(labels) >= 3:
                raise ValueError("Cyclic or unexpectedly deep PE resource tree")
            offset = resource_offset(relative, 16)
            named, numbered = read("<HH", offset + 12)
            count = named + numbered
            resource_offset(relative + 16, count * 8)
            for index in range(count):
                name_value, target = read("<II", offset + 16 + index * 8)
                current = labels + [resource_name(name_value)]
                target_offset = target & 0x7fffffff
                if target & 0x80000000:
                    walk(target_offset, current, ancestors | {relative})
                    continue
                if len(current) != 3:
                    raise ValueError("Unexpected PE resource leaf depth")
                data_rva, size, code_page, _ = read(
                    "<IIII", resource_offset(target_offset, 16))
                data_offset = rva_to_offset(data_rva, size)
                if (str(current[0]).casefold() == "b7" and
                        str(current[1]).casefold() == "chrome.packed.7z"):
                    digest = hashlib.sha256()
                    for start in range(data_offset, data_offset + size, CHUNK_SIZE):
                        digest.update(data[start:min(start + CHUNK_SIZE,
                                                     data_offset + size)])
                    matches.append({
                        "type": current[0], "name": current[1],
                        "language": current[2], "code_page": code_page,
                        "file_offset": data_offset, "bytes": size,
                        "sha256": digest.hexdigest(),
                    })

        walk(0, [], set())
        return matches
