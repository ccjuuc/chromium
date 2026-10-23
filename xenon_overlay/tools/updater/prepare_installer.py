#!/usr/bin/env python3
# Copyright 2026 The Xenon Overlay Authors. All rights reserved.
"""Wrap an application-only macOS PKG in a signed Omaha release.

PKG metadata is inspected without extraction; original bytes and native
permissions are preserved. Apple signing and notarization happen before
uploading the PKG. Windows updates use the separate JSON + ZIP tools.
"""

import argparse
import gzip
import hashlib
import io
import json
import lzma
import os
from pathlib import Path, PurePosixPath
import plistlib
import posixpath
import shutil
import stat
import struct
import sys
import tempfile
import xml.etree.ElementTree as ET
import zipfile
import zlib

import make_updater_crx as crx
from updater_packaging_common import parse_sha256_pin, run, sha256, version
from updater_host_util import find_openssl, make_private_directory

APP_ID = "org.chromium.Chromium"
MAX_FILES = 100000
MAX_EXPANDED_SIZE = 16 * 1024 * 1024 * 1024


def regular(path):
    info = Path(path).lstat()
    if (not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or
            getattr(info, "st_file_attributes", 0) & 0x400):
        raise ValueError(f"Expected a regular file without links: {path}")
    return info


def xml_document(data):
    if len(data) > 8 * 1024 * 1024 or b"<!DOCTYPE" in data or b"<!ENTITY" in data:
        raise ValueError("Unsafe package metadata XML")
    return ET.fromstring(data.decode("utf-8"))


class FileSlice(io.RawIOBase):
    """Read-only view into one bounded XAR data extent."""
    def __init__(self, path, offset, length):
        self.source = open(path, "rb")
        self.source.seek(offset)
        self.remaining = length

    def readable(self):
        return True

    def read(self, size=-1):
        size = self.remaining if size < 0 else min(size, self.remaining)
        data = self.source.read(size)
        if len(data) != size:
            raise ValueError("Truncated XAR member")
        self.remaining -= len(data)
        return data

    def readinto(self, buffer):
        data = self.read(len(buffer))
        buffer[:len(data)] = data
        return len(data)

    def close(self):
        self.source.close()
        super().close()


class Decompressed(io.RawIOBase):
    """Bounded zlib/gzip decoder for XAR's application/x-gzip encoding."""
    def __init__(self, source, limit):
        self.source = source
        self.decoder = zlib.decompressobj(47)
        self.pending = b""
        self.total = 0
        self.limit = limit
        self.finished = False

    def readable(self):
        return True

    def readinto(self, buffer):
        if self.finished:
            return 0
        while True:
            if not self.pending:
                self.pending = self.source.read(65536)
                if not self.pending:
                    raise ValueError("Truncated compressed XAR member")
            decoded = self.decoder.decompress(self.pending, len(buffer))
            self.pending = self.decoder.unconsumed_tail
            self.total += len(decoded)
            if self.total > self.limit:
                raise ValueError("Expanded XAR member exceeds the size limit")
            if self.decoder.eof:
                if self.decoder.unused_data or self.pending or self.source.read(1):
                    raise ValueError("Trailing bytes after compressed XAR member")
                self.finished = True
            if decoded or self.finished:
                buffer[:len(decoded)] = decoded
                return len(decoded)

    def close(self):
        self.source.close()
        super().close()


class PbzxReader(io.RawIOBase):
    """Decode bounded XZ/raw pbzx chunks without materializing the app tree."""
    CHUNK_LIMIT = 64 * 1024 * 1024

    def __init__(self, source):
        self.source = source
        if self.exact(4) != b"pbzx":
            raise ValueError("Invalid pbzx Payload header")
        self.flags = struct.unpack(">Q", self.exact(8))[0]
        self.pending = b""
        self.offset = 0
        self.total = 0
        self.finished = False

    def exact(self, size):
        data = self.source.read(size)
        if len(data) != size:
            raise ValueError("Truncated pbzx Payload")
        return data

    def readable(self):
        return True

    def readinto(self, buffer):
        if self.finished:
            return 0
        while self.offset == len(self.pending):
            if not self.flags & 0x1000000:
                if self.source.read(1):
                    raise ValueError("Trailing bytes after pbzx Payload")
                self.finished = True
                return 0
            self.flags, length = struct.unpack(">QQ", self.exact(16))
            if not 0 < length <= self.CHUNK_LIMIT:
                raise ValueError("pbzx chunk exceeds the size limit")
            chunk = self.exact(length)
            if chunk.startswith(b"\xfd7zXZ\0"):
                decoder = lzma.LZMADecompressor(format=lzma.FORMAT_XZ, memlimit=128 * 1024 * 1024)
                chunk = decoder.decompress(chunk, max_length=self.CHUNK_LIMIT + 1)
                if not decoder.eof or decoder.unused_data or len(chunk) > self.CHUNK_LIMIT:
                    raise ValueError("Invalid or oversized pbzx XZ chunk")
            self.total += len(chunk)
            if self.total > MAX_EXPANDED_SIZE:
                raise ValueError("Expanded pbzx Payload exceeds the size limit")
            self.pending, self.offset = chunk, 0
        count = min(len(buffer), len(self.pending) - self.offset)
        buffer[:count] = self.pending[self.offset:self.offset + count]
        self.offset += count
        return count

    def close(self):
        self.source.close()
        super().close()


def xar_entries(path):
    """Index data extents without extracting or rewriting the original PKG."""
    size = regular(path).st_size
    with open(path, "rb") as source:
        header = source.read(28)
        if len(header) != 28:
            raise ValueError("Truncated XAR header")
        magic, header_size, xar_version, compressed_size, xml_size, _ = struct.unpack(
            ">4sHHQQI", header)
        if (magic != b"xar!" or xar_version != 1 or header_size < 28 or
                not 0 < compressed_size <= 8 * 1024 * 1024 or
                not 0 < xml_size <= 8 * 1024 * 1024 or
                header_size + compressed_size > size):
            raise ValueError("Invalid or oversized XAR table of contents")
        source.seek(header_size)
        compressed = source.read(compressed_size)
        decoder = zlib.decompressobj()
        xml = decoder.decompress(compressed, xml_size + 1)
        if (len(xml) != xml_size or not decoder.eof or decoder.unused_data or
                decoder.unconsumed_tail):
            raise ValueError("Invalid compressed XAR table of contents")
    document = xml_document(xml)
    if document.tag != "xar" or document.find("toc") is None:
        raise ValueError("Missing XAR table of contents")
    heap = header_size + compressed_size
    entries, all_names, extents = {}, set(), []

    def walk(parent, prefix="", depth=0):
        if depth > 32:
            raise ValueError("XAR directory depth exceeds the limit")
        for entry in parent.findall("file"):
            name = entry.findtext("name", "")
            if (not name or name in (".", "..") or any(c in name for c in "/\\:\0") or
                    any(ord(c) < 32 for c in name)):
                raise ValueError("Unsafe XAR member name")
            full_name = prefix + name
            if full_name.casefold() in all_names or len(all_names) >= MAX_FILES:
                raise ValueError("Duplicate or excessive XAR members")
            all_names.add(full_name.casefold())
            kind = entry.findtext("type", "file")
            if kind == "directory":
                if entry.find("data") is not None:
                    raise ValueError("XAR directory cannot carry file data")
                walk(entry, full_name + "/", depth + 1)
                continue
            if kind != "file" or entry.find("link") is not None or entry.findall("file"):
                raise ValueError("XAR links and special members are forbidden")
            data = entry.find("data")
            if data is None:
                continue
            try:
                offset = int(data.findtext("offset", ""))
                length = int(data.findtext("length", ""))
                expanded = int(data.findtext("size", str(length)))
            except ValueError as error:
                raise ValueError("Invalid XAR member extent") from error
            if (offset < 0 or length < 0 or expanded < 0 or expanded > MAX_EXPANDED_SIZE or
                    heap + offset > size or length > size - heap - offset):
                raise ValueError("XAR member lies outside its container")
            encoding = data.find("encoding")
            style = encoding.get("style") if encoding is not None else "application/octet-stream"
            if style not in ("application/octet-stream", "application/x-gzip"):
                raise ValueError("Unsupported XAR member encoding")
            if length:
                extents.append((heap + offset, length))
            entries[full_name] = {"offset": heap + offset, "length": length,
                                  "size": expanded, "encoding": style}

    walk(document.find("toc"))
    previous_end = heap
    for offset, length in sorted(extents):
        if offset < previous_end:
            raise ValueError("Overlapping XAR member extents")
        previous_end = offset + length
    return entries


def xar_stream(path, entry):
    source = FileSlice(path, entry["offset"], entry["length"])
    if entry["encoding"] == "application/x-gzip":
        source = Decompressed(source, MAX_EXPANDED_SIZE)
    return io.BufferedReader(source)


def xar_metadata(path, entry):
    if entry["length"] > 8 * 1024 * 1024:
        raise ValueError("PKG metadata exceeds the size limit")
    with xar_stream(path, entry) as source:
        data = source.read(8 * 1024 * 1024 + 1)
    if len(data) > 8 * 1024 * 1024:
        raise ValueError("Expanded PKG metadata exceeds the size limit")
    return data


def macho_architectures(header):
    thin = {b"\xcf\xfa\xed\xfe": "<", b"\xfe\xed\xfa\xcf": ">",
            b"\xce\xfa\xed\xfe": "<", b"\xfe\xed\xfa\xce": ">"}
    cpu = {0x01000007: "x86_64", 0x0100000c: "arm64"}
    magic = bytes(header[:4])
    if magic in thin:
        if len(header) < 8:
            raise ValueError("Truncated Mach-O header")
        arch = cpu.get(struct.unpack_from(thin[magic] + "I", header, 4)[0])
        return {arch} if arch else set()
    fat = {b"\xca\xfe\xba\xbe": (">", 20), b"\xbe\xba\xfe\xca": ("<", 20),
           b"\xca\xfe\xba\xbf": (">", 32), b"\xbf\xba\xfe\xca": ("<", 32)}
    if magic not in fat:
        return None
    order, stride = fat[magic]
    if len(header) < 8:
        raise ValueError("Truncated universal Mach-O header")
    count = struct.unpack_from(order + "I", header, 4)[0]
    if not 0 < count <= 64 or 8 + count * stride > len(header):
        raise ValueError("Invalid universal Mach-O header")
    return {cpu[value] for index in range(count)
            if (value := struct.unpack_from(order + "I", header, 8 + index * stride)[0]) in cpu}


def inspect_cpio(source):
    """Read only metadata; preserve PKG's modes and symlinks by never extracting."""
    total, count = 0, 0
    seen, links = {}, {}
    plist, main_architectures = None, None
    common_architectures = {"arm64", "x86_64"}

    def read_exact(length):
        nonlocal total
        total += length
        if total > MAX_EXPANDED_SIZE:
            raise ValueError("Expanded PKG Payload exceeds the size limit")
        data = source.read(length)
        if len(data) != length:
            raise ValueError("Truncated PKG cpio Payload")
        return data

    while True:
        magic = read_exact(6)
        if magic == b"070707":
            header = magic + read_exact(70)
            mode, nlink = int(header[18:24], 8), int(header[36:42], 8)
            namesize, filesize = int(header[59:65], 8), int(header[65:76], 8)
            padding, checksum = 0, None
        elif magic in (b"070701", b"070702"):
            header = magic + read_exact(104)
            mode, nlink = int(header[14:22], 16), int(header[38:46], 16)
            filesize, namesize = int(header[54:62], 16), int(header[94:102], 16)
            padding = (-110 - namesize) % 4
            checksum = int(header[102:110], 16) if magic == b"070702" else None
        else:
            raise ValueError("Unsupported PKG Payload format; expected gzip or uncompressed odc/newc cpio")
        if not 1 <= namesize <= 4096 or filesize > MAX_EXPANDED_SIZE or count >= MAX_FILES:
            raise ValueError("PKG cpio member exceeds size or count limit")
        count += 1
        raw_name = read_exact(namesize)
        if not raw_name.endswith(b"\0") or b"\0" in raw_name[:-1]:
            raise ValueError("Invalid PKG cpio member name")
        name = raw_name[:-1].decode("utf-8")
        read_exact(padding)
        if name == "TRAILER!!!":
            if filesize:
                raise ValueError("Invalid PKG cpio trailer")
            # A cpio writer can pad the final block; another archive cannot be hidden here.
            tail = source.read(65537)
            if len(tail) > 65536 or any(tail):
                raise ValueError("Unexpected bytes after PKG cpio trailer")
            break
        while name.startswith("./"):
            name = name[2:]
        if name in ("", "."):
            if not stat.S_ISDIR(mode) or filesize:
                raise ValueError("Invalid PKG Payload root")
            continue
        components = name.split("/")
        if (len(components) > 128 or components[0] != "xl153.app" or any(part in ("", ".", "..") for part in components) or
                "\\" in name or any(ord(c) < 32 for c in name)):
            raise ValueError("PKG Payload must contain only safe paths under xl153.app")
        if name.casefold() in seen:
            raise ValueError("Duplicate PKG Payload path")
        if mode & 0o7022 or stat.S_IFMT(mode) not in (stat.S_IFREG, stat.S_IFDIR, stat.S_IFLNK):
            # Symlink permissions are not meaningful; macOS cpio commonly uses 0777.
            if not stat.S_ISLNK(mode):
                raise ValueError("Unsafe PKG Payload permissions or file type")
        if stat.S_ISREG(mode) and nlink != 1:
            raise ValueError("Hard-linked PKG Payload files are unsupported")
        if stat.S_ISDIR(mode) and filesize:
            raise ValueError("PKG Payload directory contains file data")
        capture = name == "xl153.app/Contents/Info.plist" or stat.S_ISLNK(mode)
        if capture and filesize > 8 * 1024 * 1024:
            raise ValueError("PKG app metadata or link exceeds the size limit")
        remaining, data, digest = filesize, bytearray(), 0
        while remaining:
            block = read_exact(min(remaining, 65536))
            digest = (digest + sum(block)) & 0xffffffff if checksum is not None else digest
            if capture:
                data.extend(block)
            elif len(data) < 4096:
                data.extend(block[:4096 - len(data)])
            remaining -= len(block)
        if checksum is not None and digest != checksum:
            raise ValueError("PKG cpio member checksum mismatch")
        if magic != b"070707":
            read_exact(-filesize % 4)
        seen[name.casefold()] = (name, mode)
        if stat.S_ISLNK(mode):
            target = data.decode("utf-8")
            resolved = posixpath.normpath(posixpath.join(posixpath.dirname(name), target))
            if (not target or target.startswith("/") or "\\" in target or "\0" in target or
                    not (resolved == "xl153.app" or resolved.startswith("xl153.app/"))):
                raise ValueError("Escaping PKG Payload symlink")
            links[name.casefold()] = resolved.casefold()
        elif stat.S_ISREG(mode):
            if name == "xl153.app/Contents/Info.plist":
                plist = plistlib.loads(bytes(data))
            architectures = macho_architectures(data)
            if architectures is not None:
                common_architectures &= architectures
            if name == "xl153.app/Contents/MacOS/xl153":
                if not mode & 0o111:
                    raise ValueError("PKG browser launcher is not executable")
                main_architectures = architectures
    for name, (_, mode) in seen.items():
        for parent in PurePosixPath(name).parents:
            value = seen.get(parent.as_posix())
            if value and not stat.S_ISDIR(value[1]):
                raise ValueError("PKG Payload path descends through a file or symlink")
    for name, target in links.items():
        visited = {name}
        while True:
            if target in visited:
                raise ValueError("Cyclic PKG Payload symlink")
            visited.add(target)
            if len(visited) > 128:
                raise ValueError("PKG Payload symlink chain is too deep")
            components = target.split("/")
            for index in range(1, len(components) + 1):
                prefix = "/".join(components[:index])
                if prefix in links:
                    target = "/".join([links[prefix], *components[index:]])
                    break
            else:
                break
        if target not in seen:
            raise ValueError("Broken PKG Payload symlink")
    if (not isinstance(plist, dict) or plist.get("CFBundleIdentifier") != APP_ID or
            plist.get("CFBundleExecutable") != "xl153" or not main_architectures):
        raise ValueError("PKG application identity or executable does not match the Xenon browser")
    version(plist.get("CFBundleShortVersionString"))
    if not common_architectures:
        raise ValueError("PKG application has no common supported architecture")
    return plist, common_architectures


def inspect_package(source):
    entries = xar_entries(source)
    payloads = [name for name in entries if PurePosixPath(name).name == "Payload"]
    infos = [name for name in entries if PurePosixPath(name).name == "PackageInfo"]
    if len(payloads) != 1 or len(infos) != 1 or PurePosixPath(payloads[0]).parent != PurePosixPath(infos[0]).parent:
        raise ValueError("Use an application-only PKG with one component; combined Updater installers are unsupported")
    if any("Scripts" in PurePosixPath(name).parts for name in entries):
        raise ValueError("Application PKG must not include install scripts")
    package_info = xml_document(xar_metadata(source, entries[infos[0]]))
    if (package_info.tag != "pkg-info" or package_info.get("install-location") != "/Applications" or
            package_info.find("scripts") is not None):
        raise ValueError("Application PKG must target /Applications without install scripts; combined installers are unsupported")
    if "Distribution" in entries:
        document = xml_document(xar_metadata(source, entries["Distribution"]))
        if any(item.tag in ("script", "installation-check", "volume-check") for item in document.iter()):
            raise ValueError("Executable package distribution scripts are unsupported")
        references = [item.text.strip() for item in document.findall("pkg-ref") if item.text and item.text.strip()]
        component = PurePosixPath(payloads[0]).parent.as_posix()
        if len(references) != 1 or references[0] not in (component, "#" + component):
            raise ValueError("PKG distribution must reference only its application component")
    with xar_stream(source, entries[payloads[0]]) as raw:
        if raw.peek(3)[:3] == b"\x1f\x8b\x08":
            with gzip.GzipFile(fileobj=raw) as payload:
                info, architectures = inspect_cpio(payload)
        elif raw.peek(4)[:4] == b"pbzx":
            with io.BufferedReader(PbzxReader(raw)) as payload:
                info, architectures = inspect_cpio(payload)
        else:
            info, architectures = inspect_cpio(raw)
    if package_info.get("version") != info["CFBundleShortVersionString"]:
        raise ValueError("PKG component and app versions differ")
    return info, architectures


def prepare_mac(source, stage, args, pin, *, command=run):
    info, architectures = inspect_package(source)
    product_version = info["CFBundleShortVersionString"]
    if args.version and version(args.version) != product_version:
        raise ValueError("Requested version differs from the PKG application version")
    architecture = args.arch or (next(iter(architectures)) if len(architectures) == 1 else None)
    if architecture not in architectures:
        raise ValueError("Select --arch from the PKG architectures: " + ", ".join(sorted(architectures)))
    artifacts = stage / "artifacts"
    artifacts.mkdir()
    suffix = ".exe" if os.name == "nt" else ""
    tools = Path(args.tools_dir).absolute()
    creator = tools / ("crx3_build_action" + suffix)
    verifier = tools / ("xenon_updater_verify" + suffix)
    if not creator.is_file() or not verifier.is_file():
        raise ValueError("Build release_tools into --tools-dir before importing installers")
    update = artifacts / f"update-{product_version}-mac-{architecture}.crx3"
    openssl = find_openssl(args.openssl)
    public = crx.public_key(args.key, pin, command=command, openssl=openssl)
    private = stage / "private"
    make_private_directory(private)
    key = private / "signing-key.pk8"
    shutil.copyfile(args.key, key)
    if os.name != "nt":
        key.chmod(0o600)
    if crx.public_key(key, pin, command=command, openssl=openssl) != public:
        raise ValueError("Signing key changed during preparation")
    package_hash = sha256(source)
    archive = stage / "payload.zip"
    with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_STORED, allowZip64=True) as bundle:
        script = zipfile.ZipInfo(".install", (2020, 1, 1, 0, 0, 0))
        script.create_system = 3
        script.external_attr = (stat.S_IFREG | 0o755) << 16
        bundle.writestr(script, crx.installer_script(info, package_hash))
        member = zipfile.ZipInfo("application.pkg", (2020, 1, 1, 0, 0, 0))
        member.create_system = 3
        member.external_attr = (stat.S_IFREG | 0o644) << 16
        with bundle.open(member, "w", force_zip64=True) as target, open(source, "rb") as incoming:
            shutil.copyfileobj(incoming, target, 1024 * 1024)
    # Explicitly verify byte preservation and POSIX launcher permissions on
    # every host. Never unpack/repack the .app; its native symlinks and modes stay
    # inside the original, signed/notarized PKG archive.
    with zipfile.ZipFile(archive) as bundle:
        if stat.S_IMODE(bundle.getinfo(".install").external_attr >> 16) != 0o755:
            raise ValueError("The macOS .install entry must retain executable permissions")
        packaged_hash = hashlib.sha256()
        with bundle.open("application.pkg") as incoming:
            for block in iter(lambda: incoming.read(1024 * 1024), b""):
                packaged_hash.update(block)
        if packaged_hash.hexdigest() != package_hash:
            raise ValueError("PKG bytes changed while wrapping the update")
    command([creator, "update.crx3", "payload.zip", "private/signing-key.pk8"], cwd=stage)
    generated = stage / "update.crx3"
    crx.verify_crx(generated, archive, public, command=command, openssl=openssl)
    full_hash = sha256(generated)
    command([verifier, "--crx=update.crx3", f"--key-sha256={pin}",
             f"--file-sha256={full_hash}"], cwd=stage)
    generated.rename(update)
    release = {"schema_version": 1, "appid": info["CFBundleIdentifier"],
               "version": product_version, "platform": "mac", "arch": architecture,
               "full": {"file": update.name, "size": update.stat().st_size, "sha256": full_hash}}
    (artifacts / "release.json").write_text(json.dumps(release, indent=2) + "\n")
    audit = {"schema_version": 1, "crx_public_key_sha256": pin,
             "crx": release["full"], "native_verified": True,
             "package_sha256": package_hash, "package_bytes_preserved": True,
             "apple_signature_verified": False,
             "apple_verification_note": "Upload a PKG signed and notarized on macOS; this service preserves its bytes but does not perform Apple trust verification."}
    (artifacts / "artifacts.json").write_text(json.dumps(audit, indent=2) + "\n")
    return artifacts, product_version, architecture


def prepare(args, *, command=run):
    source = Path(args.input).absolute()
    regular(source)
    suffix = source.suffix.lower()
    with source.open("rb") as stream:
        magic = stream.read(4)
    if suffix == ".exe" or magic[:2] == b"MZ":
        raise ValueError("Windows installers use JSON + ZIP updates; use make_directory_diff.js instead of CRX packaging")
    if suffix != ".pkg" or magic != b"xar!":
        raise ValueError("Expected an application-only flat macOS PKG")
    output = Path(args.out_dir).absolute()
    if os.path.lexists(output):
        raise ValueError("Output directory already exists; refusing to overwrite it")
    pin = parse_sha256_pin(args.expected_key_sha256).hex()
    output.parent.mkdir(parents=True, exist_ok=True)
    source_hash = sha256(source)
    with tempfile.TemporaryDirectory(prefix=".xenon-import-", dir=output.parent) as temporary:
        stage = Path(temporary)
        snapshot = stage / ("installer" + suffix)
        shutil.copyfile(source, snapshot)
        if sha256(snapshot) != source_hash:
            raise ValueError("Installer changed while snapshotting")
        print("Verifying the macOS application PKG...", file=sys.stderr)
        artifacts, product_version, architecture = prepare_mac(snapshot, stage, args, pin, command=command)
        if sha256(source) != source_hash:
            raise ValueError("Installer changed during release preparation")
        if os.path.lexists(output):
            raise ValueError("Output appeared while preparing the release")
        artifacts.rename(output)
    return {"manifest": str(output / "release.json"), "metadata": str(output / "artifacts.json"),
            "output": str(output), "version": product_version, "platform": "mac",
            "arch": architecture, "source_sha256": source_hash}


def parser():
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--input", required=True)
    result.add_argument("--out-dir", required=True)
    result.add_argument("--tools-dir", required=True)
    result.add_argument("--key", required=True)
    result.add_argument("--expected-key-sha256", required=True)
    result.add_argument("--openssl")
    result.add_argument("--version")
    result.add_argument("--arch", choices=("x86_64", "arm64"))
    result.add_argument("--team-id")
    result.add_argument("--development", action="store_true")
    return result


if __name__ == "__main__":
    try:
        print(json.dumps(prepare(parser().parse_args())))
    except (ValueError, OSError, ET.ParseError, zlib.error, lzma.LZMAError, struct.error) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        sys.exit(1)
