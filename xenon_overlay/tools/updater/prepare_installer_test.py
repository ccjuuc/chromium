#!/usr/bin/env python3
# Copyright 2026 The Xenon Overlay Authors. All rights reserved.
"""Installer import boundaries and portable macOS PKG byte preservation."""

import gzip
import hashlib
import io
import json
import lzma
import os
from pathlib import Path
import plistlib
import stat
import struct
import tempfile
import unittest
import xml.etree.ElementTree as ET
import zipfile
import zlib

import prepare_installer as prepare
from updater_packaging_common import run
from updater_host_util import check_private_key_acl, make_private_directory


def cpio(entries, *, odc=False):
    result = bytearray()
    for index, (name, mode, data) in enumerate([*entries, ("TRAILER!!!", 0, b"")]):
        name = name.encode() + b"\0"
        if odc:
            values = [0, index, mode, 0, 0, 1, 0]
            header = "070707" + "".join(f"{value:06o}" for value in values)
            header += f"{0:011o}{len(name):06o}{len(data):011o}"
            result.extend(header.encode() + name + data)
        else:
            values = [index, mode, 0, 0, 1, 0, len(data), 0, 0, 0, 0, len(name), 0]
            result.extend(("070701" + "".join(f"{value:08x}" for value in values)).encode())
            result.extend(name)
            result.extend(b"\0" * ((-110 - len(name)) % 4))
            result.extend(data)
            result.extend(b"\0" * (-len(data) % 4))
    return bytes(result)


def app_entries(version="1.0.0.2", *, machine=0x01000007):
    info = {"CFBundleIdentifier": "org.chromium.Chromium", "CFBundleExecutable": "xl153",
            "CFBundleShortVersionString": version}
    return [
        ("xl153.app", stat.S_IFDIR | 0o755, b""),
        ("xl153.app/Contents", stat.S_IFDIR | 0o755, b""),
        ("xl153.app/Contents/MacOS", stat.S_IFDIR | 0o755, b""),
        ("xl153.app/Contents/Info.plist", stat.S_IFREG | 0o644, plistlib.dumps(info)),
        ("xl153.app/Contents/MacOS/xl153", stat.S_IFREG | 0o755,
         b"\xcf\xfa\xed\xfe" + struct.pack("<I", machine) + bytes(100)),
    ]


def xar(path, entries):
    root = ET.Element("xar")
    toc = ET.SubElement(root, "toc")
    directories, data = {"": toc}, bytearray()
    for name, contents, encoded in entries:
        parts = name.split("/")
        parent = toc
        prefix = ""
        for part in parts[:-1]:
            prefix += part + "/"
            if prefix not in directories:
                folder = ET.SubElement(parent, "file")
                ET.SubElement(folder, "name").text = part
                ET.SubElement(folder, "type").text = "directory"
                directories[prefix] = folder
            parent = directories[prefix]
        entry = ET.SubElement(parent, "file")
        ET.SubElement(entry, "name").text = parts[-1]
        ET.SubElement(entry, "type").text = "file"
        extent = ET.SubElement(entry, "data")
        stored = zlib.compress(contents) if encoded else contents
        ET.SubElement(extent, "offset").text = str(len(data))
        ET.SubElement(extent, "length").text = str(len(stored))
        ET.SubElement(extent, "size").text = str(len(contents))
        ET.SubElement(extent, "encoding", style="application/x-gzip" if encoded else "application/octet-stream")
        data.extend(stored)
    xml = ET.tostring(root)
    packed = zlib.compress(xml)
    path.write_bytes(struct.pack(">4sHHQQI", b"xar!", 28, 1, len(packed), len(xml), 0) + packed + data)


def pkg(path, *, entries=None, odc=False, compressed=True, location="/Applications", scripts=False):
    members = app_entries() if entries is None else entries
    payload = cpio(members, odc=odc)
    info = f'<pkg-info version="1.0.0.2" install-location="{location}">' + ("<scripts/>" if scripts else "") + "</pkg-info>"
    xar(path, [("Distribution", b"<installer-gui-script><pkg-ref id='app'>component.pkg</pkg-ref></installer-gui-script>", True),
               ("component.pkg/PackageInfo", info.encode(), True),
               ("component.pkg/Payload", gzip.compress(payload) if compressed else payload, False)])


class InstallerTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def test_windows_installers_are_rejected_before_any_tool_runs(self):
        def unexpected_command(*args, **kwargs):
            self.fail("Windows input must not run a packaging tool")
        for name, contents in [("setup.exe", b"MZfixture"),
                               ("setup.EXE", b"xar!fixture"),
                               ("disguised.pkg", b"MZfixture")]:
            with self.subTest(name=name):
                source = self.root / name
                source.write_bytes(contents)
                output = self.root / "result"
                args = prepare.parser().parse_args([
                    "--input", str(source), "--out-dir", str(output),
                    "--tools-dir", str(self.root / "missing-tools"),
                    "--key", str(self.root / "missing-key"),
                    "--expected-key-sha256", "00" * 32])
                with self.assertRaisesRegex(ValueError, r"Windows installers use JSON \+ ZIP"):
                    prepare.prepare(args, command=unexpected_command)
                self.assertFalse(output.exists())

    @unittest.skipUnless(os.name == "nt", "Windows hosting ACL checks")
    def test_host_key_permissions_reject_broad_acl(self):
        private = self.root / "private"
        make_private_directory(private)
        key = private / "key.pk8"
        key.write_bytes(b"permission fixture")
        check_private_key_acl(key)
        run(["icacls", key, "/grant", "*S-1-5-32-545:R"])
        with self.assertRaisesRegex(ValueError, "private Windows ACL"):
            check_private_key_acl(key)

    def test_portable_pkg_metadata_gzip_newc_and_raw_odc(self):
        package = self.root / "app.pkg"
        for odc, compressed in [(False, True), (True, False)]:
            pkg(package, odc=odc, compressed=compressed)
            before = package.read_bytes()
            info, arches = prepare.inspect_package(package)
            self.assertEqual(info["CFBundleShortVersionString"], "1.0.0.2")
            self.assertEqual(arches, {"x86_64"})
            self.assertEqual(package.read_bytes(), before)

    def test_pbzx_payload_streaming_and_truncation(self):
        payload = cpio(app_entries())
        compressed = lzma.compress(payload)
        encoded = b"pbzx" + struct.pack(">QQQ", 0x1000000, 0, len(compressed)) + compressed
        with io.BufferedReader(prepare.PbzxReader(io.BytesIO(encoded))) as decoded:
            self.assertEqual(prepare.inspect_cpio(decoded)[1], {"x86_64"})
        for bad in [encoded[:-1], encoded + b"trailing", b"pbzx" + struct.pack(">QQQ", 0x1000000, 0, 10**12)]:
            with self.subTest(size=len(bad)), self.assertRaises(ValueError):
                with io.BufferedReader(prepare.PbzxReader(io.BytesIO(bad))) as decoded:
                    prepare.inspect_cpio(decoded)

    def test_pkg_payload_links_are_inspected_without_changing_permissions(self):
        package = self.root / "app.pkg"
        entries = app_entries() + [
            ("xl153.app/Contents/link", stat.S_IFLNK | 0o777, b"MacOS/xl153")]
        pkg(package, entries=entries)
        self.assertEqual(prepare.inspect_package(package)[1], {"x86_64"})
        # Frameworks commonly link through a symlinked intermediate directory.
        entries += [
            ("xl153.app/Contents/Versions", stat.S_IFDIR | 0o755, b""),
            ("xl153.app/Contents/Versions/A", stat.S_IFDIR | 0o755, b""),
            ("xl153.app/Contents/Versions/A/library", stat.S_IFREG | 0o644, b"data"),
            ("xl153.app/Contents/Versions/Current", stat.S_IFLNK | 0o777, b"A"),
            ("xl153.app/Contents/library", stat.S_IFLNK | 0o777, b"Versions/Current/library")]
        pkg(package, entries=entries)
        self.assertEqual(prepare.inspect_package(package)[1], {"x86_64"})
        for target in [b"../../../outside", b"/etc/passwd", b"missing", b"link"]:
            pkg(package, entries=app_entries() + [("xl153.app/Contents/link", stat.S_IFLNK | 0o777, target)])
            with self.subTest(target=target), self.assertRaises(ValueError):
                prepare.inspect_package(package)

    def test_pkg_rejects_combined_scripts_and_unrelated_payloads(self):
        package = self.root / "app.pkg"
        for options in [{"location": "/"}, {"scripts": True},
                        {"entries": app_entries() + [("Library/evil", stat.S_IFREG | 0o644, b"bad")]},
                        {"entries": app_entries() + [("xl153.app/../evil", stat.S_IFREG | 0o644, b"bad")]},
                        {"entries": app_entries() + [("xl153.app/unsafe", stat.S_IFREG | 0o4777, b"bad")]}]:
            pkg(package, **options)
            with self.subTest(options=options), self.assertRaises(ValueError):
                prepare.inspect_package(package)

    def test_pkg_rejects_mismatched_version_arch_and_malformed_xar(self):
        package = self.root / "app.pkg"
        pkg(package, entries=app_entries("1.0.0.3"))
        with self.assertRaisesRegex(ValueError, "versions differ"):
            prepare.inspect_package(package)
        pkg(package, entries=app_entries(machine=1))
        with self.assertRaises(ValueError):
            prepare.inspect_package(package)
        package.write_bytes(b"xar!bad")
        with self.assertRaisesRegex(ValueError, "Truncated"):
            prepare.inspect_package(package)

    def test_xar_rejects_entity_and_oversized_metadata(self):
        with self.assertRaisesRegex(ValueError, "Unsafe"):
            prepare.xml_document(b"<!DOCTYPE xar [<!ENTITY x 'bad'>]><xar/>")
        package = self.root / "app.pkg"
        package.write_bytes(struct.pack(">4sHHQQI", b"xar!", 28, 1, 8, 10**12, 0) + b"x" * 8)
        with self.assertRaises(ValueError):
            prepare.xar_entries(package)

    @unittest.skipUnless(os.environ.get("XENON_RELEASE_TOOLS"), "Set XENON_RELEASE_TOOLS for native CRX signing/verification")
    def test_native_mac_crx_preserves_original_pkg_and_posix_launcher_mode(self):
        package = self.root / "app.pkg"
        pkg(package)
        original = package.read_bytes()
        private = self.root / "private"
        make_private_directory(private)
        openssl = prepare.find_openssl()
        key = private / "test.pk8"
        run([openssl, "genpkey", "-algorithm", "RSA", "-pkeyopt", "rsa_keygen_bits:2048", "-out", private / "key.pem"])
        run([openssl, "pkcs8", "-topk8", "-nocrypt", "-in", private / "key.pem", "-outform", "DER", "-out", key])
        if os.name != "nt":
            key.chmod(0o600)
        public = run([openssl, "pkey", "-inform", "DER", "-in", key, "-pubout", "-outform", "DER"])
        args = prepare.parser().parse_args(["--input", str(package), "--out-dir", str(self.root / "result"),
            "--tools-dir", os.environ["XENON_RELEASE_TOOLS"], "--key", str(key),
            "--expected-key-sha256", hashlib.sha256(public).hexdigest(), "--openssl", str(openssl)])
        result = prepare.prepare(args)
        release = json.loads(Path(result["manifest"]).read_text())
        artifact = Path(result["output"]) / release["full"]["file"]
        with zipfile.ZipFile(artifact) as bundle:
            self.assertEqual(bundle.read("application.pkg"), original)
            self.assertEqual(stat.S_IMODE(bundle.getinfo(".install").external_attr >> 16), 0o755)
            self.assertEqual(bundle.getinfo(".install").create_system, 3)
        audit = json.loads(Path(result["metadata"]).read_text())
        self.assertTrue(audit["native_verified"])
        self.assertTrue(audit["package_bytes_preserved"])
        self.assertFalse(audit["apple_signature_verified"])


if __name__ == "__main__":
    unittest.main()
