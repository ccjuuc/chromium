# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Ensure obsolete dynamic GM libraries are not packaged by the single library build."""

from pathlib import Path
import tempfile
import unittest

import installer


class GmPackagingTest(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix="gm-linux-package-")
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.output = self.root / "output"
        runtime = self.output / "gm_backend"
        runtime.mkdir(parents=True)
        for name in ("libgm_backend.so.stripped", "LICENSE.txt", "NOTICE",
                     "gm_backend_smoke", "test-root.der", "private-key.pem",
                     "libcrypto.so.3", "backend-audit.json"):
            (runtime / name).write_bytes(name.encode("ascii"))

    def stage(self):
        config = installer.InstallerConfig(
            output_dir=self.output,
            staging_dir=self.root / "stage",
            channel="stable",
            branding="chromium",
            arch="amd64",
            target_os="linux",
            is_official_build=True,
            shlib_perms=installer.StandardPermissions.EXECUTABLE,
            info_vars={"PROGNAME": "chromium"})
        artifacts = config.get_binary_artifacts() + config.get_resource_artifacts()
        gm_artifacts = [entry for entry in artifacts
                        if str(entry.dst).startswith("gm_backend/")]
        destination = config.staging_dir / "opt/browser"
        installer.Installer(config)._process_artifacts(gm_artifacts, destination)
        return destination / "gm_backend"

    def test_ignores_stale_backend_and_test_files(self):
        self.assertFalse(self.stage().exists())


if __name__ == "__main__":
    unittest.main()
