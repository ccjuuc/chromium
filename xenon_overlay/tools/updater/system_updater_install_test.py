#!/usr/bin/env python3
"""Run the generated installer against ad-hoc signed apps in private fixtures.

The root identity, ownership, process lookup and Apple Installer boundaries are
substituted only in a temporary copy of the script. File operations, plist reads,
package hashing and codesign verification are native. This never installs a PKG
or registers a system service and is not a privileged installation acceptance test.
"""

import json
import os
from pathlib import Path
import plistlib
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest

import make_updater_crx as crx
from updater_packaging_common import app_metadata, run, sha256, verify_app


def fixture_command(operation, state_path, arguments):
    state_path = Path(state_path).resolve()
    state = json.loads(state_path.read_text())
    root = state_path.parent
    target = root / "installed/xl153.app"
    package = root / "payload/application.pkg"
    state["calls"].append(operation)
    if operation == "pgrep":
        statuses = state.get("process_statuses", [1])
        status = statuses.pop(0) if len(statuses) > 1 else statuses[0]
        state["process_statuses"] = statuses
        if "version_after_wait" in state:
            plist = target / "Contents/Info.plist"
            info = plistlib.loads(plist.read_bytes())
            info["CFBundleShortVersionString"] = state.pop("version_after_wait")
            plist.write_bytes(plistlib.dumps(info))
        if "owner_after_wait" in state:
            state["owner"] = state.pop("owner_after_wait")
        if state.pop("link_target_after_wait", False):
            target.rename(root / "previous.app")
            target.symlink_to(root / "new/xl153.app")
        state_path.write_text(json.dumps(state))
        return status
    state_path.write_text(json.dumps(state))
    if operation == "stat":
        if arguments != ["-f", "%u", str(target)]:
            raise ValueError("Unexpected ownership query")
        print(state.get("owner", 0))
        return 0
    if operation == "codesign":
        if arguments[-1] != str(target) or target.is_symlink():
            raise ValueError("Signature checks must stay inside the fixture")
        return subprocess.run(["/usr/bin/codesign", *arguments], check=False).returncode
    if operation != "installer" or arguments != ["-pkg", str(package), "-target", "/"]:
        raise ValueError("Unexpected installer command")
    if target.is_symlink() or not target.is_dir():
        raise ValueError("Fixture installation target must be a real directory")
    behavior = state.get("installer_behavior", "success")
    if behavior == "fail":
        return 1
    with tempfile.TemporaryDirectory(prefix="expanded-", dir=root) as temporary:
        expanded = Path(temporary) / "package"
        run(["/usr/sbin/pkgutil", "--expand-full", package, expanded])
        source = expanded / "Payload/xl153.app"
        if behavior == "partial":
            shutil.copy2(source / "Contents/Info.plist", target / "Contents/Info.plist")
            (target / "Contents/Resources/version").write_text("partial installation")
            return 1
        shutil.rmtree(target)
        run(["/usr/bin/ditto", source, target])
    if behavior == "corrupt":
        (target / "Contents/Resources/version").write_text("corrupted after installation")
    elif behavior in ("wrong_version", "wrong_identifier"):
        plist = target / "Contents/Info.plist"
        info = plistlib.loads(plist.read_bytes())
        key = "CFBundleShortVersionString" if behavior == "wrong_version" else "CFBundleIdentifier"
        info[key] = "1.0.0.1" if behavior == "wrong_version" else "org.example.other"
        plist.write_bytes(plistlib.dumps(info))
        run(["/usr/bin/codesign", "--force", "--sign", "-", target])
    return 0


@unittest.skipUnless(sys.platform == "darwin", "Native macOS developer-signature fixtures")
class SystemInstallerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fixtures = tempfile.TemporaryDirectory(prefix="xenon-install-fixtures-")
        cls.addClassCleanup(cls.fixtures.cleanup)
        cls.fixture_root = Path(cls.fixtures.name).resolve()
        for name, version in (("old", "1.0.0.1"), ("new", "1.0.0.2")):
            bundle = cls.fixture_root / name / "xl153.app"
            (bundle / "Contents/MacOS").mkdir(parents=True)
            (bundle / "Contents/Resources").mkdir()
            executable = bundle / "Contents/MacOS/xl153"
            shutil.copyfile("/bin/sleep", executable)
            executable.chmod(0o755)
            (bundle / "Contents/Resources/version").write_text(version)
            (bundle / "Contents/Info.plist").write_bytes(plistlib.dumps({
                "CFBundleIdentifier": "org.chromium.Chromium",
                "CFBundleExecutable": "xl153", "CFBundleName": "xl153",
                "CFBundlePackageType": "APPL", "CFBundleVersion": version,
                "CFBundleShortVersionString": version}))
            run(["/usr/bin/codesign", "--force", "--sign", "-", bundle])
            run(["/usr/bin/codesign", "--verify", "--deep", "--strict", bundle])
        cls.package = cls.fixture_root / "application.pkg"
        run(["/usr/bin/pkgbuild", "--root", cls.fixture_root / "new",
             "--identifier", "org.xenon.installer-fixture", "--version", "1.0.0.2",
             "--install-location", "/Applications", cls.package])

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="xenon-install-contract-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.target = self.root / "installed/xl153.app"
        self.source = self.root / "new/xl153.app"
        for name, destination in (("old", self.target), ("new", self.source)):
            destination.parent.mkdir()
            run(["/usr/bin/ditto", self.fixture_root / name / "xl153.app", destination])
        self.payload = self.root / "payload"
        self.payload.mkdir()
        self.package = self.payload / "application.pkg"
        shutil.copy2(type(self).package, self.package)
        self.state_path = self.root / "state.json"
        self.state_path.write_text(json.dumps({"calls": []}))
        script = crx.installer_script(app_metadata(self.source), sha256(self.package), wait_seconds=1)
        self.production_script = script
        self.assertEqual(script.count('"$EUID"'), 1)
        script = script.replace('"$EUID"', '"$XENON_TEST_EUID"')
        self.assertEqual(script.count("readonly TARGET=" + crx.TARGET), 1)
        script = script.replace("readonly TARGET=" + crx.TARGET,
                                "readonly TARGET=" + shlex.quote(str(self.target)))
        for command in ("/usr/bin/stat", "/usr/bin/pgrep", "/usr/sbin/installer", "/usr/bin/codesign"):
            self.assertIn(command, script)
            replacement = shlex.join([sys.executable, str(Path(__file__).resolve()),
                                      "--fixture-command", Path(command).name, str(self.state_path)])
            script = script.replace(command, replacement)
        self.script = self.payload / ".install"
        self.script.write_text(script)

    def configure(self, **values):
        state = self.state()
        state.update(values)
        self.state_path.write_text(json.dumps(state))

    def state(self):
        return json.loads(self.state_path.read_text())

    def install(self, **environment):
        return subprocess.run(
            ["/bin/bash", "-p", str(self.script), str(self.payload), str(self.target), "1.0.0.1"],
            env={"PATH": "/usr/bin:/bin", "UPDATE_IS_MACHINE": "1",
                 "KS_TICKET_XC_PATH": str(self.target), "XENON_TEST_EUID": "0", **environment},
            capture_output=True, timeout=15)

    def verify_installed(self):
        self.assertEqual(app_metadata(self.target)["CFBundleShortVersionString"], "1.0.0.2")
        run(["/usr/bin/codesign", "--verify", "--deep", "--strict", self.target])
        self.assertEqual(crx.inventory(self.target), crx.inventory(self.source))

    def assert_not_installed(self, result):
        self.assertNotEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("installer", self.state()["calls"])

    def test_complete_script_installs_and_verifies_developer_signed_app(self):
        result = self.install()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.state()["calls"].count("installer"), 1)
        self.verify_installed()

    def test_developer_signature_does_not_pass_release_signing_policy(self):
        with self.assertRaisesRegex(ValueError, "codesign failed"):
            verify_app(self.source, development=False, team_id="ABCDEFGHIJ")

    def test_scope_and_registered_path_are_required(self):
        for environment in ({"UPDATE_IS_MACHINE": "0"}, {"XENON_TEST_EUID": "501"},
                            {"KS_TICKET_XC_PATH": str(self.source)}):
            with self.subTest(environment=environment):
                self.assert_not_installed(self.install(**environment))

    def test_unmodified_production_script_rejects_nonroot_execution(self):
        if os.geteuid() == 0:
            self.skipTest("Nonroot invocation is required")
        result = subprocess.run(["/bin/bash", "-p"], input=self.production_script.encode(),
                                env={"UPDATE_IS_MACHINE": "1", "KS_TICKET_XC_PATH": crx.TARGET},
                                capture_output=True, timeout=5)
        self.assertNotEqual(result.returncode, 0)

    def test_rejects_nonroot_owned_target(self):
        self.configure(owner=501)
        self.assert_not_installed(self.install())

    def test_rejects_symlink_target_without_modifying_destination(self):
        shutil.rmtree(self.target)
        self.target.symlink_to(self.source)
        before = crx.inventory(self.source)
        self.assert_not_installed(self.install())
        self.assertEqual(before, crx.inventory(self.source))

    def test_rejects_changed_package_before_installation(self):
        self.package.write_bytes(b"xar!damaged package")
        self.assert_not_installed(self.install())

    def test_rejects_package_symlink(self):
        self.package.unlink()
        self.package.symlink_to(type(self).package)
        self.assert_not_installed(self.install())

    def test_process_lookup_error_does_not_install(self):
        self.configure(process_statuses=[2])
        self.assert_not_installed(self.install())

    def test_running_browser_times_out_without_installation(self):
        self.configure(process_statuses=[0])
        result = self.install()
        self.assert_not_installed(result)
        self.assertIn(b"still running", result.stderr)

    def test_rechecks_version_after_browser_wait(self):
        self.configure(version_after_wait="1.0.0.3")
        result = self.install()
        self.assert_not_installed(result)
        self.assertIn(b"Refusing update", result.stderr)

    def test_rechecks_target_ownership_after_browser_wait(self):
        self.configure(owner_after_wait=501)
        self.assert_not_installed(self.install())

    def test_rechecks_target_symlink_after_browser_wait(self):
        self.configure(link_target_after_wait=True)
        before = crx.inventory(self.source)
        self.assert_not_installed(self.install())
        self.assertEqual(before, crx.inventory(self.source))

    def test_installer_failure_keeps_old_signed_application(self):
        before = crx.inventory(self.target)
        self.configure(installer_behavior="fail")
        self.assertNotEqual(self.install().returncode, 0)
        self.assertNotIn("codesign", self.state()["calls"])
        self.assertEqual(before, crx.inventory(self.target))

    def test_partial_installation_is_repaired_by_a_fresh_script_process(self):
        self.configure(installer_behavior="partial")
        self.assertNotEqual(self.install().returncode, 0)
        self.assertEqual(app_metadata(self.target)["CFBundleShortVersionString"], "1.0.0.2")
        with self.assertRaisesRegex(ValueError, "codesign failed"):
            run(["/usr/bin/codesign", "--verify", "--deep", "--strict", self.target])
        self.configure(installer_behavior="success")
        result = self.install()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.verify_installed()

    def test_retry_cannot_downgrade_a_newer_installation(self):
        self.configure(installer_behavior="partial")
        self.assertNotEqual(self.install().returncode, 0)
        self.configure(calls=[], installer_behavior="success", version_after_wait="1.0.0.3")
        self.assert_not_installed(self.install())

    def test_successful_installer_exit_requires_valid_bundle_signature(self):
        self.configure(installer_behavior="corrupt")
        self.assertNotEqual(self.install().returncode, 0)
        self.assertIn("codesign", self.state()["calls"])

    def test_successful_installer_exit_requires_expected_identity_and_version(self):
        for behavior in ("wrong_version", "wrong_identifier"):
            with self.subTest(behavior=behavior):
                self.configure(installer_behavior=behavior, calls=[])
                self.assertNotEqual(self.install().returncode, 0)
                self.assertIn("installer", self.state()["calls"])
                self.assertNotIn("codesign", self.state()["calls"])


if __name__ == "__main__":
    if len(sys.argv) >= 4 and sys.argv[1] == "--fixture-command":
        sys.exit(fixture_command(sys.argv[2], sys.argv[3], sys.argv[4:]))
    unittest.main(verbosity=2)
