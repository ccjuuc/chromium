# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Ensure the single-library build excludes obsolete dynamic GM payloads."""

import importlib.util
from pathlib import Path
import tempfile
import types
import unittest

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location(
    "gm_installer_archive", ROOT / "chrome/tools/build/win/create_installer_archive.py")
archive = importlib.util.module_from_spec(spec)
spec.loader.exec_module(archive)


class PackagingTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="gm-installer-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.build = self.root / "build"
        self.build.mkdir()
        (self.build / "chrome.exe").write_bytes(b"browser")
        (self.build / "chrome.dll").write_bytes(b"browser-library")
        self.runtime = self.build / "gm_backend"
        self.runtime.mkdir()
        self.required = ("gm_backend.dll", "LICENSE.txt", "NOTICE", "vcruntime140.dll",
                         "vcruntime140_1.dll")
        for name in self.required + ("gm_backend_smoke.exe", "libcrypto-3-x64.dll",
                                     "backend-audit.json", "gm_sm2_simulator.dll"):
            (self.runtime / name).write_bytes(name.encode())
        (self.build / "gm_boringssl_test_backend.dll").write_bytes(b"test shim")
        self.peer = self.build / "gm_interop_peer"
        self.peer.mkdir()
        (self.peer / "libcrypto-3-x64.dll").write_bytes(b"test peer only")
        self.config = archive.Readconfig(
            str(ROOT / "chrome/installer/mini_installer/chrome.release"), "153.0.8004.0")
        archive.options = types.SimpleNamespace(build_time=None, use_static_angle="False")
        archive.g_archive_inputs.clear()

    def stage(self):
        stage = self.root / "stage"
        archive.CopyAllFilesToStagingDir(self.config, "", str(stage), str(self.build),
                                         "0", "0", "0", "0", "0", False)
        return stage

    def test_stages_browser_without_stale_gm_outputs(self):
        stage = self.stage() / "Chrome-bin"
        self.assertEqual((stage / "chrome.exe").read_bytes(), b"browser")
        self.assertEqual((stage / "153.0.8004.0/chrome.dll").read_bytes(),
                         b"browser-library")
        self.assertFalse((stage / "153.0.8004.0/gm_backend").exists())
        self.assertFalse(self.config.has_section("GM_BACKEND"))
        self.assertFalse((stage / "153.0.8004.0/gm_interop_peer").exists())
        self.assertFalse(list(stage.rglob("gm_boringssl_test_backend.dll")))

    def test_missing_old_runtime_does_not_block_staging(self):
        for name in self.required:
            (self.runtime / name).unlink()
        self.assertTrue((self.stage() / "Chrome-bin/chrome.exe").is_file())


if __name__ == "__main__":
    unittest.main()
