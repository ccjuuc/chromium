#!/usr/bin/env python3
# Copyright 2026 The Xenon Overlay Authors. All rights reserved.
"""Offline delta validation. Set XENON_PUFFIN_TOOL for the native round-trip test."""

import argparse
import hashlib
import os
from pathlib import Path
import random
import struct
import tempfile
import unittest
import zipfile

import make_updater_delta as delta
from make_updater_crx import verify_crx
from updater_packaging_common import run
from system_updater_packaging_test import field


class DeltaTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="xenon-delta-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.old, self.new = self.root / "old.crx3", self.root / "new.crx3"
        # The mocks only need a well-formed CRX envelope; the native test below
        # creates and verifies complete RSA-signed CRXs.
        self.old.write_bytes(b"Cr24" + struct.pack("<II", 3, 1) + b"x" + b"old" * 100)
        self.new.write_bytes(b"Cr24" + struct.pack("<II", 3, 1) + b"x" + b"new" * 100)
        self.output = self.root / "update.puff"
        self.args = argparse.Namespace(old_crx=self.old, new_crx=self.new,
                                       from_version="1.0.0.1", to_version="1.0.0.2",
                                       out=self.output, puffin="puffin", metadata=None)

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
                zipped.writestr("data.bin", shared_payload)
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
