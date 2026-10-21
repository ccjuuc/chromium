"""Locate embedded gzip streams without changing signed CRX or PKG bytes."""

import struct
import xml.etree.ElementTree as ET
import zipfile
import zlib


MAX_TOC_SIZE = 8 * 1024 * 1024


def read_at(source, offset, length, end):
    if offset < 0 or length < 0 or offset > end or length > end - offset:
        raise ValueError("Archive member lies outside its container")
    source.seek(offset)
    result = source.read(length)
    if len(result) != length:
        raise ValueError("Truncated archive member")
    return result


def xar_gzip_ranges(source, start, size):
    end = start + size
    header = read_at(source, start, 28, end)
    magic, header_size, version, compressed_size, toc_size, _ = struct.unpack(
        ">4sHHQQI", header)
    if magic != b"xar!" or version != 1 or header_size < 28:
        raise ValueError("Invalid flat PKG XAR header")
    if not 0 < toc_size <= MAX_TOC_SIZE or not 0 < compressed_size <= MAX_TOC_SIZE:
        raise ValueError("PKG table of contents exceeds the size limit")
    compressed = read_at(source, start + header_size, compressed_size, end)
    decoder = zlib.decompressobj()
    try:
        xml = decoder.decompress(compressed, toc_size + 1)
    except zlib.error as error:
        raise ValueError("Invalid compressed PKG table of contents") from error
    if (len(xml) != toc_size or not decoder.eof or decoder.unused_data or
            decoder.unconsumed_tail):
        raise ValueError("Invalid PKG table of contents size")
    if b"<!DOCTYPE" in xml or b"<!ENTITY" in xml:
        raise ValueError("PKG table of contents must not contain XML entities")
    try:
        root = ET.fromstring(xml)
    except ET.ParseError as error:
        raise ValueError("Invalid PKG table of contents XML") from error
    if root.tag != "xar" or root.find("toc") is None:
        raise ValueError("Missing PKG table of contents")
    heap = start + header_size + compressed_size
    members = []
    ranges = []
    for entry in root.findall(".//file"):
        data = entry.find("data")
        if data is None:
            continue
        try:
            offset = int(data.findtext("offset", ""))
            length = int(data.findtext("length", ""))
        except ValueError as error:
            raise ValueError("Invalid PKG member extent") from error
        if offset < 0 or length < 0 or heap + offset > end or length > end - heap - offset:
            raise ValueError("PKG member lies outside its container")
        absolute = heap + offset
        if length:
            members.append((absolute, length))
        encoding = data.find("encoding")
        if (entry.findtext("name") == "Payload" and encoding is not None and
                encoding.get("style") == "application/octet-stream" and length >= 3 and
                read_at(source, absolute, 3, end) == b"\x1f\x8b\x08"):
            if length < 18:
                raise ValueError("Truncated gzip Payload")
            ranges.append((absolute, length))
    previous_end = heap
    for offset, length in sorted(members):
        if offset < previous_end:
            raise ValueError("Overlapping PKG members")
        previous_end = offset + length
    return sorted(ranges)


def nested_gzip_ranges(path):
    """Return absolute byte ranges for gzip Payloads in stored application.pkg."""
    try:
        with zipfile.ZipFile(path) as archive, path.open("rb") as source:
            packages = [entry for entry in archive.infolist() if entry.filename == "application.pkg"]
            if not packages:
                return []
            if len(packages) != 1:
                raise ValueError("Duplicate application.pkg in CRX")
            package = packages[0]
            if package.compress_type != zipfile.ZIP_STORED:
                return []
            if package.flag_bits & 1 or package.compress_size != package.file_size:
                raise ValueError("Invalid stored application.pkg")
            with archive.open(package) as member:
                if member.read(4) != b"xar!":
                    raise ValueError("application.pkg is not a flat XAR archive")
            header = read_at(source, package.header_offset, 30, archive.start_dir)
            if header[:4] != b"PK\x03\x04" or struct.unpack_from("<H", header, 8)[0] != 0:
                raise ValueError("Invalid stored PKG local ZIP header")
            name_size, extra_size = struct.unpack_from("<HH", header, 26)
            start = package.header_offset + 30 + name_size + extra_size
            if start > archive.start_dir or package.file_size > archive.start_dir - start:
                raise ValueError("PKG overlaps the ZIP directory")
            return xar_gzip_ranges(source, start, package.file_size)
    except (zipfile.BadZipFile, NotImplementedError) as error:
        raise ValueError("Invalid CRX ZIP payload") from error
