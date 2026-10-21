#!/usr/bin/env python3
"""Exercise the packaging CLI with real local artifacts; never install them."""

import hashlib
import json
import os
from pathlib import Path
import plistlib
import shutil
import sys
import tempfile
import unittest
import zipfile

import make_updater_crx as crx
import package_macos as packaging
from updater_packaging_common import run, sha256


class PackagingArgumentsTest(unittest.TestCase):
    def arguments(self, *extra):
        return packaging.parser().parse_args([
            "--app", "/unused/xl153.app", "--key", "/unused/key.pk8",
            "--expected-key-sha256", "a" * 64, "--out-dir", "/unused/output", *extra])

    def reject(self, args, message):
        with self.assertRaisesRegex(ValueError, message):
            packaging.build_packages(args, command=lambda *args, **kwargs: self.fail(
                "Invalid arguments must be rejected before running external commands"))

    @unittest.skipUnless(sys.platform == "darwin", "macOS packaging")
    def test_release_credentials_are_required_by_default(self):
        self.reject(self.arguments(), "Release mode requires")

    @unittest.skipUnless(sys.platform == "darwin", "macOS packaging")
    def test_development_cannot_mix_release_credentials(self):
        for option in ("--team-id", "--installer-identity", "--notary-profile"):
            with self.subTest(option=option):
                self.reject(self.arguments("--development", option, "value"),
                            "cannot use release signing")

    @unittest.skipUnless(sys.platform == "darwin", "macOS packaging")
    def test_existing_directory_and_dangling_symlink_are_preserved(self):
        with tempfile.TemporaryDirectory() as temporary:
            existing = Path(temporary) / "existing"
            existing.mkdir()
            sentinel = existing / "original.crx3"
            sentinel.write_bytes(b"published baseline")
            alias = Path(temporary) / "alias"
            alias.symlink_to("missing")
            for output in (existing, alias):
                self.reject(self.arguments("--development", "--out-dir", str(output)),
                            "must not already exist")
            self.assertEqual(sentinel.read_bytes(), b"published baseline")
            self.assertTrue(alias.is_symlink())

    @unittest.skipUnless(sys.platform == "darwin", "macOS packaging")
    def test_invalid_wait_and_test_updater_are_rejected_early(self):
        self.reject(self.arguments("--development", "--wait-seconds", "0"), "wait-seconds")
        self.reject(self.arguments("--development", "--updater-app", "/unused/XenonUpdater_test.app"),
                    "must name XenonUpdater.app")


@unittest.skipUnless(sys.platform == "darwin" and os.environ.get("XENON_PACKAGE_TOOLS_DIR"),
                     "Set XENON_PACKAGE_TOOLS_DIR for real macOS packaging")
class NativePackagingTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="xenon package cli ")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.root = Path(cls.temporary.name)
        cls.key = cls.root / "private.pk8"
        private = run(["/usr/bin/openssl", "genrsa", "2048"])
        cls.key.write_bytes(run(["/usr/bin/openssl", "pkcs8", "-topk8", "-nocrypt",
                                 "-outform", "DER"], input=private))
        cls.key.chmod(0o600)
        public = run(["/usr/bin/openssl", "pkey", "-inform", "DER", "-in", cls.key,
                      "-pubout", "-outform", "DER"])
        cls.pin = hashlib.sha256(public).hexdigest()
        cls.old = cls.bundle("old", "xl153", "org.chromium.Chromium", "1.0.0.1")
        cls.new = cls.bundle("new", "xl153", "org.chromium.Chromium", "1.0.0.2")
        cls.updater = cls.bundle("updater", "XenonUpdater", "com.xenon.XenonUpdater", "153.0.8004.0")
        cls.arch = run(["/usr/bin/lipo", "-archs",
                        cls.new / "Contents/MacOS/xl153"]).decode().split()[0]

    @classmethod
    def bundle(cls, directory, name, app_id, version):
        app = cls.root / directory / (name + ".app")
        (app / "Contents/MacOS").mkdir(parents=True)
        shutil.copyfile("/usr/bin/true", app / "Contents/MacOS" / name)
        (app / "Contents/MacOS" / name).chmod(0o755)
        (app / "Contents/Info.plist").write_bytes(plistlib.dumps({
            "CFBundleIdentifier": app_id, "CFBundleExecutable": name,
            "CFBundleName": name, "CFBundlePackageType": "APPL",
            "CFBundleVersion": version, "CFBundleShortVersionString": version}))
        (app / "Contents/Resources").mkdir()
        (app / "Contents/Resources/resource.txt").write_text("shared resource\n" * 8192 + version)
        run(["/usr/bin/codesign", "--force", "--sign", "-", app])
        return app

    def arguments(self, app, output, *extra):
        return packaging.parser().parse_args([
            "--app", str(app), "--key", str(self.key), "--expected-key-sha256", self.pin,
            "--out-dir", str(output), "--development", "--arch", self.arch,
            "--tools-dir", os.environ["XENON_PACKAGE_TOOLS_DIR"], *extra])

    def test_failure_after_pkg_does_not_publish_partial_release(self):
        output = self.root / "failed"
        def fail_verifier(argv, **kwargs):
            if Path(argv[0]).name == "xenon_updater_verify":
                raise ValueError("injected native verification failure")
            return run(argv, **kwargs)
        with self.assertRaisesRegex(ValueError, "injected native verification"):
            packaging.build_packages(self.arguments(self.new, output), command=fail_verifier)
        self.assertFalse(output.exists())
        self.assertEqual(list(self.root.glob(".xenon-package-*")), [])

    def test_wrong_key_pin_leaves_inputs_and_output_untouched(self):
        output = self.root / "wrong-pin"
        args = self.arguments(self.new, output)
        args.expected_key_sha256 = "0" * 64
        original = crx.inventory(self.new)
        with self.assertRaisesRegex(ValueError, "does not match"):
            packaging.build_packages(args)
        self.assertEqual(crx.inventory(self.new), original)
        self.assertFalse(output.exists())

    def test_real_cli_pkgs_crxs_manifest_and_delta(self):
        before = {app: crx.inventory(app) for app in (self.old, self.new, self.updater)}
        outputs = []
        for number, app in enumerate((self.old, self.new), start=1):
            output = self.root / f"release-{number}"
            extra = ["--updater-app", str(self.updater)] if number == 2 else []
            argv = [sys.executable, packaging.SCRIPT_DIR / "package_macos.py",
                    "--app", app, "--key", self.key, "--expected-key-sha256", self.pin,
                    "--out-dir", output, "--development", "--arch", self.arch,
                    "--tools-dir", os.environ["XENON_PACKAGE_TOOLS_DIR"], *extra]
            result = json.loads(run(argv))
            self.assertEqual(result["output"], str(output))
            manifest = json.loads((output / "release.json").read_text())
            update = output / manifest["full"]["file"]
            self.assertEqual(sha256(update), manifest["full"]["sha256"])
            with zipfile.ZipFile(update) as archive:
                self.assertEqual(sorted(archive.namelist()), [".install", "application.pkg"])
                self.assertEqual(hashlib.sha256(archive.read("application.pkg")).hexdigest(),
                                 sha256(output / f"application-1.0.0.{number}.pkg"))
            script = (f"import {{loadCatalog}} from '{(packaging.SCRIPT_DIR / 'omaha_update_server.js').as_uri()}';"
                      "const catalog = await loadCatalog(process.argv[1]); await catalog.close();")
            run(["node", "--input-type=module", "-e", script, output / "release.json"])
            outputs.append(update)
        output = outputs[1].parent
        self.assertEqual(sorted(entry.name for entry in output.iterdir()), [
            "application-1.0.0.2.pkg", "artifacts.json", "release.json",
            "update-1.0.0.2.crx3", "xl153-with-updater-1.0.0.2.pkg"])
        metadata = json.loads((output / "artifacts.json").read_text())
        self.assertTrue(metadata["development"])
        self.assertEqual(metadata["crx"]["public_key_sha256"], self.pin)
        self.assertTrue(metadata["system_installer"]["includes_app"])
        expanded = self.root / "expanded-system"
        run(["/usr/sbin/pkgutil", "--expand-full",
             output / "xl153-with-updater-1.0.0.2.pkg", expanded])
        scripts = list(expanded.rglob("preinstall"))
        self.assertEqual(len(scripts), 1)
        self.assertIn('Development artifact: root installation is disabled.', scripts[0].read_text())
        self.assertIn("exit 1", scripts[0].read_text())
        for app, original in before.items():
            self.assertEqual(crx.inventory(app), original)
        puffin = os.environ.get("XENON_PUFFIN_TOOL")
        if not puffin:
            self.skipTest("Packages verified; set XENON_PUFFIN_TOOL for real CLI delta generation")
        patch = self.root / "1-to-2.puff"
        sidecar = self.root / "1-to-2.json"
        result = json.loads(run([
            sys.executable, packaging.SCRIPT_DIR / "make_updater_delta.py",
            "--old-crx", outputs[0], "--new-crx", outputs[1],
            "--from-version", "1.0.0.1", "--to-version", "1.0.0.2",
            "--puffin", puffin, "--out", patch, "--metadata", sidecar]))
        self.assertTrue(result["roundtrip_verified"])
        self.assertEqual(result["compression"]["source_gzip_streams"], 1)
        self.assertEqual(result["compression"]["target_gzip_streams"], 1)
        self.assertEqual(result["previous_sha256"], sha256(outputs[0]))
        self.assertEqual(result["full_sha256"], sha256(outputs[1]))
        self.assertEqual(json.loads(sidecar.read_text()), result)


if __name__ == "__main__":
    unittest.main(verbosity=2)
