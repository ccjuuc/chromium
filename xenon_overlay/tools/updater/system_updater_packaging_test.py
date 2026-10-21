#!/usr/bin/env python3
# Copyright 2026 The Xenon Overlay Authors. All rights reserved.
"""Offline packaging tests; never install a PKG or register/start a service."""

import argparse
import hashlib
import os
from pathlib import Path
import plistlib
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import time
import unittest
import zipfile

import make_system_updater_pkg as system_pkg
import make_updater_crx as crx
from updater_packaging_common import app_metadata, check_tree, run, verify_app


def field(number, value):
    def varint(value):
        result = bytearray()
        while value > 127:
            result.append((value & 127) | 128)
            value >>= 7
        return bytes(result + bytes([value]))
    return varint(number * 8 + 2) + varint(len(value)) + value


class PackagingTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="xenon-packaging-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def fixture_app(self, name="xl153", app_id="org.chromium.Chromium"):
        app = self.root / (name + ".app")
        (app / "Contents/MacOS").mkdir(parents=True)
        binary = app / "Contents/MacOS" / name
        binary.write_text("#!/bin/sh\nexit 0\n")
        binary.chmod(0o755)
        (app / "Contents/Info.plist").write_bytes(plistlib.dumps({
            "CFBundleIdentifier": app_id, "CFBundleExecutable": name,
            "CFBundleName": name, "CFBundlePackageType": "APPL",
            "CFBundleVersion": "1.0.0.2", "CFBundleShortVersionString": "1.0.0.2"}))
        return app

    def test_payload_allows_framework_symlinks(self):
        app = self.fixture_app()
        framework = app / "Contents/Frameworks/Test.framework"
        (framework / "Versions/1").mkdir(parents=True)
        (framework / "Versions/Current").symlink_to("1")
        (framework / "Test").symlink_to("Versions/Current/Test")
        (framework / "Versions/1/Test").write_text("safe")
        check_tree(app)

    def test_payload_rejects_escaping_symlinks(self):
        app = self.fixture_app()
        (app / "escape").symlink_to("../outside")
        (self.root / "outside").write_text("outside")
        with self.assertRaisesRegex(ValueError, "Escaping"):
            check_tree(app)

    def test_payload_rejects_hardlinks_and_writable_files(self):
        app = self.fixture_app()
        main = app / "Contents/MacOS/xl153"
        os.link(main, app / "hardlink")
        with self.assertRaisesRegex(ValueError, "Hard-linked"):
            check_tree(app)
        (app / "hardlink").unlink()
        main.chmod(0o777)
        with self.assertRaisesRegex(ValueError, "Unsafe"):
            check_tree(app)

    def test_release_requires_apple_team_and_notarization(self):
        commands = []
        verify_app("test.app", development=False, team_id="ABCDEFGHIJ",
                   command=lambda argv: commands.append(argv))
        self.assertIn("certificate leaf[subject.OU] = \"ABCDEFGHIJ\"", commands[0][-2])
        self.assertIn("1.2.840.113635.100.6.1.13", commands[0][-2])
        self.assertEqual(commands[1][1:3], ["stapler", "validate"])
        with self.assertRaisesRegex(ValueError, "team-id"):
            verify_app("test.app", development=False, command=lambda argv: None)

    def test_crx_output_cannot_replace_signing_key(self):
        app = self.fixture_app()
        package = self.root / "application.pkg"
        package.write_bytes(b"xar!fixture")
        key = self.root / "release-key.pk8"
        original = b"private signing key fixture"
        key.write_bytes(original)
        key.chmod(0o600)
        alias = self.root / "key-alias"
        alias.symlink_to(key.name)
        for output in (key, alias):
            with self.subTest(output=output):
                args = argparse.Namespace(app=app, pkg=package, out=output,
                                          key=key, development=True, team_id=None)
                with self.assertRaisesRegex(ValueError, "packaging input"):
                    crx.build_crx(args, command=lambda argv: self.fail(
                        "Output collision must be rejected before invoking tools"))
                self.assertEqual(key.read_bytes(), original)

    def test_postinstall_is_system_only_and_registers_fixed_target(self):
        script = system_pkg.postinstall_script("org.chromium.Chromium", "1.0.0.2",
                                               "org.chromium.Chromium", "XenonSoftwareUpdate")
        self.assertIn('--install --system', script)
        self.assertIn('-r -S -P org.chromium.Chromium', script)
        self.assertIn('readonly TARGET=/Applications/xl153.app', script)
        self.assertIn('"$EUID" -eq 0', script)
        self.assertIn('"${3:-/}" = /', script)
        self.assertNotIn("sudo", script)
        self.assertNotIn("launchctl", script)
        subprocess.run(["/bin/bash", "-n"], input=script.encode(), check=True)

    def test_distribution_allows_only_local_system(self):
        xml = system_pkg.distribution_xml("com.xenon.updater.pkg", "1.0.0.2", {"arm64"})
        self.assertIn('enable_currentUserHome="false"', xml)
        self.assertIn('enable_localSystem="true"', xml)
        self.assertIn('enable_anywhere="false"', xml)
        self.assertIn('hostArchitectures="arm64"', xml)

    def test_bundle_architectures_include_nested_native_helpers(self):
        app = self.fixture_app()
        main = app / "Contents/MacOS/xl153"
        main.write_bytes(b"\xcf\xfa\xed\xfe" + b"\0" * 28)
        helper = app / "Contents/MacOS/helper"
        helper.write_bytes(main.read_bytes())
        architectures = {main: b"arm64 x86_64\n", helper: b"arm64\n"}
        commands = []
        def mock(argv):
            commands.append(argv)
            self.assertEqual(argv[:2], ["/usr/bin/lipo", "-archs"])
            return architectures[Path(argv[-1])]
        self.assertEqual(system_pkg.bundle_architectures(app, command=mock), {"arm64"})
        self.assertEqual(len(commands), 2)
        architectures[main] = b"x86_64\n"
        with self.assertRaisesRegex(ValueError, "common.*architecture"):
            system_pkg.bundle_architectures(app, command=mock)

    def test_bundle_architectures_require_native_main_executable(self):
        app = self.fixture_app()
        with self.assertRaisesRegex(ValueError, "Mach-O"):
            system_pkg.bundle_architectures(app, command=lambda argv: self.fail(
                "Non-native main executable must fail before invoking lipo"))

    def test_system_package_rejects_mismatched_architectures_before_copying(self):
        updater = self.fixture_app("XenonUpdater", "com.xenon.XenonUpdater")
        app = self.fixture_app()
        for bundle in (app, updater):
            (bundle / "Contents/MacOS" / bundle.stem).write_bytes(
                b"\xcf\xfa\xed\xfe" + b"\0" * 28)
        output = self.root / "release.pkg"
        output.write_bytes(b"previous release")
        args = argparse.Namespace(updater_app=updater, app=app, out=output,
                                  app_id="org.chromium.Chromium", updater_id="com.xenon.XenonUpdater",
                                  identifier="com.xenon.updater.pkg", keystone_name="XenonSoftwareUpdate",
                                  development=True, include_app=True, team_id=None,
                                  installer_identity=None, notary_profile=None)
        def mock(argv):
            self.assertEqual(argv[:2], ["/usr/bin/lipo", "-archs"])
            return b"arm64\n" if Path(argv[-1]).is_relative_to(updater) else b"x86_64\n"
        with self.assertRaisesRegex(ValueError, "no common supported architecture"):
            system_pkg.build_package(args, command=mock)
        self.assertEqual(output.read_bytes(), b"previous release")

    def test_architecture_preinstall_rejects_unsupported_hosts(self):
        for allowed, native, apple_silicon, expected in (
                ({"arm64"}, "arm64", "1", 0),
                ({"arm64"}, "x86_64", "0", 1),
                ({"arm64"}, "x86_64", "1", 0),
                ({"x86_64"}, "x86_64", "0", 0),
                ({"x86_64"}, "x86_64", "1", 1),
                ({"arm64", "x86_64"}, "x86_64", "0", 0)):
            with self.subTest(allowed=allowed, native=native, apple_silicon=apple_silicon):
                script = system_pkg.architecture_preinstall(allowed)
                script = script.replace("/usr/bin/uname -m", "printf " + native)
                script = script.replace("/usr/sbin/sysctl -n hw.optional.arm64",
                                        "printf " + apple_silicon)
                result = subprocess.run(["/bin/bash"], input=script.encode(),
                                        capture_output=True)
                self.assertEqual(result.returncode, expected, result.stderr)

    def test_distribution_rejects_empty_or_unknown_architectures(self):
        for architectures in (set(), {"i386"}, {"arm64", "unknown"}):
            with self.subTest(architectures=architectures), self.assertRaises(ValueError):
                system_pkg.distribution_xml("com.xenon.updater.pkg", "1.0.0.2", architectures)

    def test_installer_version_guard_compares_uint32_components(self):
        cases = (("1.0.0.1", "1.0.0.2", 0), ("1.0.0.2", "1.0.0.2", 0),
                 ("1.0.0.3", "1.0.0.2", 1), ("1.10.0.0", "1.9.0.0", 1),
                 ("1.9.0.0", "1.10.0.0", 0),
                 ("4294967295.0.0.0", "4294967294.0.0.0", 1),
                 ("0.0.0.0", "4294967295.0.0.0", 0))
        for installed, target, expected in cases:
            with self.subTest(installed=installed, target=target):
                script = crx.version_guard_script() + '\nversion_allows_install "$1" "$2"\n'
                result = subprocess.run(["/bin/bash", "-c", script, "guard", installed, target],
                                        capture_output=True)
                self.assertEqual(result.returncode, expected, result.stderr)

    def test_installer_version_guard_rejects_malformed_versions(self):
        for invalid in ("", "1.0", "1.0.0.01", "1.0.0.-1", "1.0.0.4294967296",
                        "0.0.0.99999999999999999999", "1.0.0.2\n", "1.0.0.2;exit 0"):
            for installed, target in ((invalid, "1.0.0.2"), ("1.0.0.2", invalid)):
                with self.subTest(installed=installed, target=target):
                    script = crx.version_guard_script() + '\nversion_allows_install "$1" "$2"\n'
                    result = subprocess.run(["/bin/bash", "-c", script, "guard", installed, target],
                                            capture_output=True)
                    self.assertEqual(result.returncode, 2, result.stderr)

    def test_installer_rechecks_version_after_waiting_before_installing(self):
        script = crx.installer_script(app_metadata(self.fixture_app()), "a" * 64)
        guard = script.index('version_allows_install "$INSTALLED_VERSION"')
        self.assertGreater(guard, script.index("# Recheck the installation"))
        self.assertLess(guard, script.index("/usr/sbin/installer -pkg"))

    def test_installer_version_failure_stops_before_installation(self):
        script = crx.installer_script(app_metadata(self.fixture_app()), "a" * 64)
        boundary = script[script.index("INSTALLED_VERSION="):script.index("/usr/sbin/installer -pkg")]
        for installed, expected in (("1.0.0.1", 0), ("1.0.0.2", 0), ("1.0.0.3", 1), ("broken", 1)):
            with self.subTest(installed=installed):
                check = boundary.replace(
                    "/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' \"$TARGET/Contents/Info.plist\"",
                    "printf '%s' \"$1\"")
                command = "set -eu\n" + crx.version_guard_script() + check + "printf reached-installer\n"
                result = subprocess.run(["/bin/bash", "-c", command, "guard", installed],
                                        capture_output=True)
                self.assertEqual(result.returncode, expected, result.stderr)
                self.assertEqual(result.stdout, b"reached-installer" if expected == 0 else b"")

    def test_installer_has_scope_path_hash_and_post_version_guards(self):
        info = app_metadata(self.fixture_app())
        script = crx.installer_script(info, "a" * 64)
        for expected in ('UPDATE_IS_MACHINE', 'KS_TICKET_XC_PATH', 'pgrep -x xl153',
                         'DEADLINE=', 'shasum -a 256', '1.0.0.2', 'codesign --verify'):
            self.assertIn(expected, script)
        self.assertNotIn("/bin/kill", script)
        self.assertNotIn("xattr", script)
        self.assertEqual(script.count("/usr/sbin/installer -pkg"), 1)
        subprocess.run(["/bin/bash", "-n"], input=script.encode(), check=True)

    @unittest.skipUnless(sys.platform == "darwin", "macOS pgrep behavior")
    def test_waiting_times_out_without_killing_process(self):
        executable = self.root / "xenonWaitTest"
        shutil.copyfile("/bin/sleep", executable)
        executable.chmod(0o755)
        run(["/usr/bin/codesign", "--force", "--sign", "-", executable])
        child = subprocess.Popen([str(executable), "30"])
        self.addCleanup(self.stop_child, child)
        info = {"CFBundleExecutable": executable.name, "CFBundleIdentifier": "test.xenon",
                "CFBundleShortVersionString": "1.0.0.2"}
        script = crx.installer_script(info, "a" * 64, wait_seconds=1)
        # Run just the pure waiting section, never the root guard or installer.
        waiting = script[script.index("echo 'Waiting"):script.index("# Recheck the installation")]
        time.sleep(0.05)
        result = subprocess.run(["/bin/bash", "-c", waiting], capture_output=True, timeout=5)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"still running", result.stderr)
        self.assertIsNone(child.poll())
        child.terminate()
        child.wait()

    @unittest.skipUnless(sys.platform == "darwin", "macOS pgrep behavior")
    def test_waiting_continues_after_natural_exit(self):
        executable = self.root / "xenonWaitTest"
        shutil.copyfile("/bin/sleep", executable)
        executable.chmod(0o755)
        run(["/usr/bin/codesign", "--force", "--sign", "-", executable])
        child = subprocess.Popen([str(executable), "0.3"])
        self.addCleanup(self.stop_child, child)
        info = {"CFBundleExecutable": executable.name, "CFBundleIdentifier": "test.xenon",
                "CFBundleShortVersionString": "1.0.0.2"}
        script = crx.installer_script(info, "a" * 64, wait_seconds=5)
        waiting = script[script.index("echo 'Waiting"):script.index("# Recheck the installation")]
        # Reap the child concurrently so a completed zombie is not treated as a
        # running application by pgrep on platforms where it appears in comm.
        import threading
        reaper = threading.Thread(target=child.wait)
        reaper.start()
        result = subprocess.run(["/bin/bash", "-c", waiting], capture_output=True, timeout=8)
        reaper.join()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(child.wait(), 0)

    @staticmethod
    def stop_child(child):
        if child.poll() is None:
            child.terminate()
        child.wait()

    def test_crx_pin_and_signature_and_archive_tampering(self):
        key = self.root / "key.der"
        pem = run(["/usr/bin/openssl", "genrsa", "2048"])
        key.write_bytes(run(["/usr/bin/openssl", "pkcs8", "-topk8", "-nocrypt",
                             "-outform", "DER"], input=pem))
        key.chmod(0o600)
        public = run(["/usr/bin/openssl", "pkey", "-inform", "DER", "-in", key,
                      "-pubout", "-outform", "DER"])
        pin = hashlib.sha256(public).hexdigest()
        self.assertEqual(crx.public_key(key, pin), public)
        with self.assertRaisesRegex(ValueError, "does not match"):
            crx.public_key(key, "0" * 64)
        key.chmod(0o644)
        with self.assertRaisesRegex(ValueError, "private"):
            crx.public_key(key, pin)
        key.chmod(0o600)
        archive = self.root / "payload.zip"
        with zipfile.ZipFile(archive, "w") as zipped:
            zipped.writestr(".install", "fixture; never execute")
        signed_header = field(1, hashlib.sha256(public).digest()[:16])
        signed_data = b"CRX3 SignedData\0" + struct.pack("<I", len(signed_header)) + signed_header + archive.read_bytes()
        # This independent fixture producer uses OpenSSL, not the native creator
        # or the verifier under test.
        signature = run(["/usr/bin/openssl", "dgst", "-sha256", "-sign", key,
                         "-keyform", "DER"], input=signed_data)
        header = field(2, field(1, public) + field(2, signature)) + field(10000, signed_header)
        package = self.root / "test.crx3"
        package.write_bytes(b"Cr24" + struct.pack("<II", 3, len(header)) + header + archive.read_bytes())
        crx.verify_crx(package, archive, public)
        data = bytearray(package.read_bytes())
        signature_offset = data.index(signature)
        data[signature_offset] ^= 1
        package.write_bytes(data)
        with self.assertRaisesRegex(ValueError, "failed"):
            crx.verify_crx(package, archive, public)

    @unittest.skipUnless(sys.platform == "darwin", "Apple packaging tools")
    def test_real_development_pkg_build_and_expand_without_install(self):
        updater = self.fixture_app("XenonUpdater", "com.xenon.XenonUpdater")
        app = self.fixture_app()
        for bundle in (app, updater):
            executable = bundle / "Contents/MacOS" / bundle.stem
            shutil.copyfile("/bin/sleep", executable)
            executable.chmod(0o755)
            # Sign harmless script bundles in this private fixture; never run them.
            run(["/usr/bin/codesign", "--force", "--sign", "-", bundle])
        output = self.root / "XenonUpdater.development.pkg"
        args = argparse.Namespace(updater_app=updater, app=app, out=output,
                                  app_id="org.chromium.Chromium", updater_id="com.xenon.XenonUpdater",
                                  identifier="com.xenon.updater.pkg", keystone_name="XenonSoftwareUpdate",
                                  development=True, include_app=True, team_id=None,
                                  installer_identity=None, notary_profile=None)
        metadata = system_pkg.build_package(args)
        self.assertTrue(metadata["development"])
        expanded = self.root / "expanded"
        run(["/usr/sbin/pkgutil", "--expand-full", output, expanded])
        scripts = list(expanded.rglob("preinstall"))
        self.assertEqual(len(scripts), 1)
        self.assertIn("root installation is disabled", scripts[0].read_text())
        self.assertIn("--install --system", scripts[0].with_name("postinstall").read_text())
        self.assertEqual(len(list(expanded.rglob("xl153.app"))), 1)
        self.assertEqual(len(list(expanded.rglob("XenonUpdater.app"))), 1)
        self.assertIn("must-close", (expanded / "Distribution").read_text())
        architectures = ",".join(metadata["architectures"])
        self.assertIn(f'hostArchitectures="{architectures}"',
                      (expanded / "Distribution").read_text())
        self.assertIn("hw.optional.arm64", scripts[0].read_text())

    @unittest.skipUnless(sys.platform == "darwin" and shutil.which("node"),
                         "Apple packaging tools and Node")
    def test_real_app_pkg_payload_validation(self):
        app = self.fixture_app()
        run(["/usr/bin/codesign", "--force", "--sign", "-", app])
        package = self.root / "application.pkg"
        run([shutil.which("node"), Path(__file__).with_name("make_mac_pkg.js"),
             "--app", app, "--out", package])
        verification = self.root / "verification"
        verification.mkdir()
        crx.verify_package_payload(package, app, verification)
        # Reject an extra script-only component, even if the app payload still
        # matches. It could otherwise execute arbitrary commands as root.
        def inject_script(argv, **kwargs):
            result = run(argv, **kwargs)
            if "--expand-full" in argv:
                extra = Path(argv[-1]) / "extra.pkg"
                (extra / "Scripts").mkdir(parents=True)
                (extra / "PackageInfo").write_text('<pkg-info install-location="/"/>')
            return result
        tampered = self.root / "tampered"
        tampered.mkdir()
        with self.assertRaisesRegex(ValueError, "one component"):
            crx.verify_package_payload(package, app, tampered, command=inject_script)


if __name__ == "__main__":
    unittest.main(verbosity=2)
