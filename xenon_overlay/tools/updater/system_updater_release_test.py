#!/usr/bin/env python3
"""Read-only acceptance of signed release inputs; never execute their installer.

Set XENON_REQUIRE_RELEASE_FIXTURES=1 in release CI to make missing inputs fail.
These checks validate release artifacts, not system service installation.
"""

import hashlib
import os
from pathlib import Path
import re
import stat
import sys
import tempfile
import unittest
import zipfile

import make_updater_crx as crx
from updater_packaging_common import (app_metadata, parse_sha256_pin, run, sha256,
                                      verify_app)


FIXTURE_VARIABLES = (
    "XENON_RELEASE_OLD_APP", "XENON_RELEASE_NEW_APP", "XENON_RELEASE_PKG",
    "XENON_RELEASE_CRX", "XENON_RELEASE_TEAM_ID", "XENON_RELEASE_KEY_SHA256",
)


def release_configuration(environment):
    if not any(name in environment for name in FIXTURE_VARIABLES) and environment.get(
            "XENON_REQUIRE_RELEASE_FIXTURES") != "1":
        return None
    missing = [name for name in (*FIXTURE_VARIABLES, "XENON_VERIFY_TOOL")
               if not environment.get(name)]
    if missing:
        raise ValueError("Missing release fixtures: " + ", ".join(missing))
    if not re.fullmatch(r"[A-Z0-9]{10}", environment["XENON_RELEASE_TEAM_ID"]):
        raise ValueError("Release Team ID must contain 10 uppercase letters or digits")
    result = {name: environment[name] for name in (*FIXTURE_VARIABLES, "XENON_VERIFY_TOOL")}
    result["pin"] = parse_sha256_pin(environment["XENON_RELEASE_KEY_SHA256"])
    result["wait_seconds"] = int(environment.get("XENON_RELEASE_WAIT_SECONDS", "180"))
    if not 1 <= result["wait_seconds"] <= 600:
        raise ValueError("Release wait seconds must be between 1 and 600")
    for name in ("XENON_RELEASE_OLD_APP", "XENON_RELEASE_NEW_APP", "XENON_RELEASE_PKG",
                 "XENON_RELEASE_CRX", "XENON_VERIFY_TOOL"):
        path = Path(result[name])
        if not path.is_absolute() or path.is_symlink():
            raise ValueError(f"{name} must be an absolute path, not a symbolic link")
        if name.endswith("_APP"):
            if not path.is_dir() or path.name != "xl153.app":
                raise ValueError(f"{name} must name a real xl153.app directory")
        elif not path.is_file():
            raise ValueError(f"{name} must name a regular file")
        result[name] = path
    if not os.access(result["XENON_VERIFY_TOOL"], os.X_OK):
        raise ValueError("XENON_VERIFY_TOOL must be executable")
    return result


class ReleaseConfigurationTest(unittest.TestCase):
    def test_offline_suite_skips_only_when_no_release_inputs_are_configured(self):
        self.assertIsNone(release_configuration({}))
        self.assertIsNone(release_configuration({"XENON_VERIFY_TOOL": "/native/tool"}))

    def test_release_gate_refuses_missing_fixtures(self):
        with self.assertRaisesRegex(ValueError, "Missing release fixtures"):
            release_configuration({"XENON_REQUIRE_RELEASE_FIXTURES": "1"})

    def test_partial_or_empty_fixture_configuration_is_an_error(self):
        for name in FIXTURE_VARIABLES:
            for value in ("", "/fixture"):
                with self.subTest(name=name, value=value), self.assertRaisesRegex(
                        ValueError, "Missing release fixtures"):
                    release_configuration({name: value})


class SignedReleaseTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.configuration = release_configuration(os.environ)
        if cls.configuration is None:
            raise unittest.SkipTest("Developer ID release artifacts not configured; system upgrade is unverified")
        if sys.platform != "darwin":
            raise ValueError("Configured release fixtures require native macOS verification")
        cls.old = cls.configuration["XENON_RELEASE_OLD_APP"]
        cls.new = cls.configuration["XENON_RELEASE_NEW_APP"]
        cls.package = cls.configuration["XENON_RELEASE_PKG"]
        cls.crx = cls.configuration["XENON_RELEASE_CRX"]
        cls.team = cls.configuration["XENON_RELEASE_TEAM_ID"]

    def test_release_apps_have_expected_signer_identity_and_increasing_versions(self):
        old_info, new_info = app_metadata(self.old), app_metadata(self.new)
        for bundle, info in ((self.old, old_info), (self.new, new_info)):
            self.assertEqual(info["CFBundleIdentifier"], "org.chromium.Chromium")
            self.assertEqual(info["CFBundleExecutable"], "xl153")
            verify_app(bundle, development=False, team_id=self.team)
        self.assertGreater(tuple(map(int, new_info["CFBundleShortVersionString"].split("."))),
                           tuple(map(int, old_info["CFBundleShortVersionString"].split("."))))

    def test_signed_notarized_package_matches_new_application(self):
        signature = run(["/usr/sbin/pkgutil", "--check-signature", self.package]).decode()
        self.assertRegex(signature, r"Developer ID Installer: .+\(" + re.escape(self.team) + r"\)")
        run(["/usr/bin/xcrun", "stapler", "validate", self.package])
        with tempfile.TemporaryDirectory(prefix="xenon-release-payload-") as temporary:
            crx.verify_package_payload(self.package, self.new, Path(temporary))

    def test_native_crx_verification_binds_package_and_installer(self):
        run([self.configuration["XENON_VERIFY_TOOL"], f"--crx={self.crx}",
             f"--key-sha256={self.configuration['pin'].hex()}",
             f"--file-sha256={sha256(self.crx)}"])
        with zipfile.ZipFile(self.crx) as archive:
            self.assertEqual(sorted(archive.namelist()), [".install", "application.pkg"])
            script = archive.getinfo(".install")
            self.assertEqual(script.create_system, 3)
            self.assertEqual(script.external_attr >> 16, stat.S_IFREG | 0o755)
            self.assertEqual(archive.read(script).decode(), crx.installer_script(
                app_metadata(self.new), sha256(self.package),
                wait_seconds=self.configuration["wait_seconds"]))
            digest = hashlib.sha256()
            with archive.open("application.pkg") as source:
                for block in iter(lambda: source.read(1024 * 1024), b""):
                    digest.update(block)
            self.assertEqual(digest.hexdigest(), sha256(self.package))


if __name__ == "__main__":
    unittest.main(verbosity=2)
