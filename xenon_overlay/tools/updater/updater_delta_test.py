#!/usr/bin/env python3
# Copyright 2026 The Xenon Overlay Authors. All rights reserved.
"""Offline delta validation. Set XENON_PUFFIN_TOOL for the native round-trip test."""

import argparse
import gzip
import hashlib
import io
import os
from pathlib import Path
import random
import struct
import tempfile
import unittest
import xml.etree.ElementTree as ET
import zipfile
import zlib

import make_updater_delta as delta
from make_updater_crx import verify_crx
from updater_packaging_common import run
from system_updater_packaging_test import field
from updater_payload_ranges import MAX_TOC_SIZE, nested_gzip_ranges


def flat_package(payload, *, offset=0, length=None, duplicate=False):
    root = ET.Element("xar")
    toc = ET.SubElement(root, "toc")
    component = ET.SubElement(toc, "file", id="1")
    ET.SubElement(component, "name").text = "component.pkg"
    for number in range(2 if duplicate else 1):
        entry = ET.SubElement(component, "file", id=str(number + 2))
        ET.SubElement(entry, "name").text = "Payload"
        data = ET.SubElement(entry, "data")
        ET.SubElement(data, "offset").text = str(offset)
        ET.SubElement(data, "length").text = str(len(payload) if length is None else length)
        ET.SubElement(data, "encoding", style="application/octet-stream")
    xml = ET.tostring(root)
    compressed = zlib.compress(xml)
    return struct.pack(">4sHHQQI", b"xar!", 28, 1, len(compressed), len(xml), 0) + compressed + payload


def crx_fixture(path, payload, *, name="application.pkg", compression=zipfile.ZIP_STORED):
    archive = io.BytesIO()
    with zipfile.ZipFile(archive, "w", compression=compression) as zipped:
        zipped.writestr(".install", b"offline test; never execute")
        entry = zipfile.ZipInfo(name)
        entry.compress_type = compression
        with zipped.open(entry, "w", force_zip64=True) as member:
            member.write(payload)
    path.write_bytes(b"Cr24" + struct.pack("<II", 3, 1) + b"x" + archive.getvalue())


class DeltaTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="xenon-delta-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.old, self.new = self.root / "old.crx3", self.root / "new.crx3"
        # The mocks only need a well-formed CRX envelope; the native test below
        # creates and verifies complete RSA-signed CRXs.
        crx_fixture(self.old, b"old" * 100, name="data.bin")
        crx_fixture(self.new, b"new" * 100, name="data.bin")
        self.output = self.root / "update.puff"
        self.args = argparse.Namespace(old_crx=self.old, new_crx=self.new,
                                       from_version="1.0.0.1", to_version="1.0.0.2",
                                       out=self.output, puffin="puffin", metadata=None,
                                       compression="auto")

    def test_equal_or_larger_patch_is_not_recommended(self):
        def mock(argv, **kwargs):
            if argv[1] == "-puffdiff":
                Path(argv[4]).write_bytes(Path(argv[3]).read_bytes())
            else:
                Path(argv[3]).write_bytes(Path(argv[4]).read_bytes())
        result = delta.build_delta(self.args, command=mock)
        self.assertFalse(result["recommended"])
        self.assertEqual(result["default_payload"], "full")
        self.assertTrue(result["roundtrip_verified"])
        self.assertEqual(result["previous_sha256"], hashlib.sha256(self.old.read_bytes()).hexdigest())

    def test_smaller_verified_patch_is_recommended(self):
        def mock(argv, **kwargs):
            if argv[1] == "-puffdiff":
                Path(argv[4]).write_bytes(b"patch")
            else:
                Path(argv[3]).write_bytes(self.new.read_bytes())
        self.args.metadata = self.root / "metadata.json"
        result = delta.build_delta(self.args, command=mock)
        self.assertTrue(result["recommended"])
        self.assertEqual(result["type"], "puff")
        self.assertEqual(result["file"], "update.puff")
        self.assertEqual(result["from_version"], "1.0.0.1")
        self.assertTrue(self.args.metadata.is_file())

    def test_failed_roundtrip_never_replaces_existing_release(self):
        self.output.write_bytes(b"previous approved artifact")
        def mock(argv, **kwargs):
            Path(argv[4] if argv[1] == "-puffdiff" else argv[3]).write_bytes(b"wrong")
        with self.assertRaisesRegex(ValueError, "round-trip"):
            delta.build_delta(self.args, command=mock)
        self.assertEqual(self.output.read_bytes(), b"previous approved artifact")

    def test_changed_source_does_not_publish(self):
        def mock(argv, **kwargs):
            if argv[1] == "-puffdiff":
                Path(argv[4]).write_bytes(b"patch")
            else:
                Path(argv[3]).write_bytes(self.new.read_bytes())
                self.old.write_bytes(b"changed")
        with self.assertRaisesRegex(ValueError, "changed"):
            delta.build_delta(self.args, command=mock)
        self.assertFalse(self.output.exists())

    def test_rejects_downgrade_or_metadata_overwriting_input(self):
        self.args.to_version = "1.0.0.0"
        with self.assertRaisesRegex(ValueError, "newer"):
            delta.build_delta(self.args)
        self.args.to_version = "1.0.0.2"
        self.args.metadata = self.new
        with self.assertRaisesRegex(ValueError, "Metadata output"):
            delta.build_delta(self.args)

    def test_embedded_payload_ranges_are_absolute_with_crx_and_zip64_headers(self):
        payload = gzip.compress(b"application contents" * 100, mtime=0)
        crx_fixture(self.old, flat_package(payload))
        ranges = nested_gzip_ranges(self.old)
        self.assertEqual(len(ranges), 1)
        offset, length = ranges[0]
        self.assertEqual(self.old.read_bytes()[offset:offset + length], payload)

    def test_non_gzip_payload_or_compressed_zip_keeps_legacy_path(self):
        for payload, compression in ((b"pbzx" + b"x" * 100, zipfile.ZIP_STORED),
                                     (gzip.compress(b"contents"), zipfile.ZIP_DEFLATED)):
            with self.subTest(compression=compression):
                crx_fixture(self.old, flat_package(payload), compression=compression)
                self.assertEqual(nested_gzip_ranges(self.old), [])

    def test_rejects_out_of_bounds_and_overlapping_pkg_members(self):
        payload = gzip.compress(b"contents" * 100)
        for options in ({"offset": -1}, {"offset": 1}, {"length": -1},
                        {"length": len(payload) + 1}, {"duplicate": True}):
            with self.subTest(options=options):
                crx_fixture(self.old, flat_package(payload, **options))
                with self.assertRaises(ValueError):
                    nested_gzip_ranges(self.old)

    def test_rejects_truncated_or_oversized_toc_before_publishing(self):
        package = flat_package(gzip.compress(b"contents" * 100))
        for invalid in (package[:20], package[:16] + struct.pack(">Q", MAX_TOC_SIZE + 1) + package[24:],
                        package[:28] + b"broken compressed XML"):
            with self.subTest(size=len(invalid)):
                crx_fixture(self.old, invalid)
                self.output.write_bytes(b"previous approved artifact")
                with self.assertRaises(ValueError):
                    delta.build_delta(self.args)
                self.assertEqual(self.output.read_bytes(), b"previous approved artifact")

    def test_rejects_truncated_gzip_envelope(self):
        crx_fixture(self.old, flat_package(b"\x1f\x8b\x08" + b"\0" * 12))
        with self.assertRaisesRegex(ValueError, "Truncated gzip"):
            nested_gzip_ranges(self.old)

    @unittest.skipUnless(os.environ.get("XENON_PUFFIN_TOOL"), "Set XENON_PUFFIN_TOOL to run native Puffin")
    def test_native_embedded_gzip_saves_bandwidth_and_old_decoder_reconstructs_exactly(self):
        generator = random.Random(42)
        resources = [generator.randbytes(32) for unused in range(128)]
        shared = b"".join(generator.choices(resources, k=8000))
        for number, output in enumerate((self.old, self.new)):
            contents = b"new resources\n" * number + shared
            payload = gzip.compress(contents, mtime=number)
            crx_fixture(output, flat_package(payload))
        self.args.puffin = os.environ["XENON_PUFFIN_TOOL"]
        result = delta.build_delta(self.args)
        self.assertEqual(result["compression"]["source_gzip_streams"], 1)
        self.assertEqual(result["compression"]["target_gzip_streams"], 1)
        optimized = self.output.read_bytes()
        self.args.compression = "opaque"
        legacy = delta.build_delta(self.args)
        self.assertLess(len(optimized), legacy["size"] // 2)
        self.output.write_bytes(optimized)
        restored = self.root / "restored.crx3"
        decoder = os.environ.get("XENON_PUFFIN_DECODER", self.args.puffin)
        run([decoder, "-puffpatch", self.old, restored, self.output], cwd=self.root)
        self.assertEqual(restored.read_bytes(), self.new.read_bytes())

    @unittest.skipUnless(os.environ.get("XENON_PUFFIN_TOOL"), "Set XENON_PUFFIN_TOOL to run native Puffin")
    def test_native_rejects_invalid_ranges_without_publishing(self):
        payload = gzip.compress(b"contents" * 100)
        source = self.root / "input.raw"
        source.write_bytes(payload)
        for ranges in ("", "0", "-1:20", "0:0", f"0:{len(payload) + 1}",
                       f"0:{len(payload)},0:{len(payload)}", "18446744073709551616:20"):
            with self.subTest(ranges=ranges):
                with self.assertRaisesRegex(ValueError, r"failed \(1\)"):
                    run([os.environ["XENON_PUFFIN_TOOL"], "-puffdiff-gzip", source, source,
                         self.output, ranges, "none"], cwd=self.root)
                self.assertFalse(self.output.exists())

    @unittest.skipUnless(os.environ.get("XENON_PUFFIN_TOOL"), "Set XENON_PUFFIN_TOOL to run native Puffin")
    def test_native_rejects_truncated_gzip_header_and_footer(self):
        malformed_header = b"\x1f\x8b\x08\x0a" + b"\0" * 6 + b"name!!!\0"
        for payload in (malformed_header, gzip.compress(b"contents" * 100)[:-1]):
            with self.subTest(payload=payload):
                crx_fixture(self.old, flat_package(payload))
                self.args.puffin = os.environ["XENON_PUFFIN_TOOL"]
                with self.assertRaisesRegex(ValueError, r"failed \(1\)"):
                    delta.build_delta(self.args)
                self.assertFalse(self.output.exists())

    @unittest.skipUnless(os.environ.get("XENON_PUFFIN_TOOL"), "Set XENON_PUFFIN_TOOL to run native Puffin")
    def test_native_concatenated_gzip_members_roundtrip(self):
        for number, output in enumerate((self.old, self.new)):
            payload = (gzip.compress(b"first resource" * 100, mtime=0) +
                       gzip.compress(b"second resource" * 100 + bytes([number]), mtime=0))
            crx_fixture(output, flat_package(payload))
        self.args.puffin = os.environ["XENON_PUFFIN_TOOL"]
        self.assertTrue(delta.build_delta(self.args)["roundtrip_verified"])

    @unittest.skipUnless(os.environ.get("XENON_PUFFIN_TOOL"), "Set XENON_PUFFIN_TOOL to run native Puffin")
    def test_native_puffin_roundtrip_with_real_signed_crxs(self):
        key = self.root / "key.der"
        pem = run(["/usr/bin/openssl", "genrsa", "2048"])
        key.write_bytes(run(["/usr/bin/openssl", "pkcs8", "-topk8", "-nocrypt",
                             "-outform", "DER"], input=pem))
        key.chmod(0o600)
        public = run(["/usr/bin/openssl", "pkey", "-inform", "DER", "-in", key,
                      "-pubout", "-outform", "DER"])
        shared_payload = random.Random(17).randbytes(128 * 1024)
        for number, output in enumerate((self.old, self.new)):
            archive = self.root / f"{number}.zip"
            with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_STORED) as zipped:
                zipped.writestr(".install", b"offline fixture; never execute\n" + bytes([number]))
                zipped.writestr("application.pkg", flat_package(gzip.compress(
                    shared_payload + b"resources" * 1000 + bytes([number]), mtime=0)))
            native_creator = os.environ.get("XENON_CRX_TOOL")
            if native_creator:
                run([native_creator, output, archive, key])
            else:
                signed_header = field(1, hashlib.sha256(public).digest()[:16])
                signed_data = (b"CRX3 SignedData\0" + struct.pack("<I", len(signed_header)) +
                               signed_header + archive.read_bytes())
                signature = run(["/usr/bin/openssl", "dgst", "-sha256", "-sign", key,
                                 "-keyform", "DER"], input=signed_data)
                header = field(2, field(1, public) + field(2, signature)) + field(10000, signed_header)
                output.write_bytes(b"Cr24" + struct.pack("<II", 3, len(header)) + header + archive.read_bytes())
            verify_crx(output, archive, public)
            verifier = os.environ.get("XENON_VERIFY_TOOL")
            if verifier:
                arguments = [verifier, f"--crx={output}",
                             f"--file-sha256={hashlib.sha256(output.read_bytes()).hexdigest()}"]
                run([*arguments, f"--key-sha256={hashlib.sha256(public).hexdigest()}"])
                with self.assertRaisesRegex(ValueError, "CRX rejected"):
                    run([*arguments, "--key-sha256=" + "00" * 32])
        self.args.puffin = os.environ["XENON_PUFFIN_TOOL"]
        result = delta.build_delta(self.args)
        self.assertTrue(result["roundtrip_verified"])
        self.assertTrue(result["recommended"], result)


if __name__ == "__main__":
    unittest.main(verbosity=2)
