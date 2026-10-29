# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Offline tests for build isolation, archive validation and target planning."""

import hashlib
from pathlib import Path
import shlex
import stat
import tempfile
import unittest
import zipfile

import build_dependency as build


class DependencyBuildTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.archive = self.root / "source.zip"
        self.output = self.root / "build"
        self.source_name = "Tongsuo-test"

    def make_archive(self, entries=None):
        with zipfile.ZipFile(self.archive, "w") as bundle:
            for name, value in (entries or [
                    (self.source_name + "/include/openssl/ssl.h", "new header")]):
                if isinstance(name, str):
                    entry = zipfile.ZipInfo(name)
                    # ZipInfo normalizes backslashes on Windows. Preserve the
                    # hostile raw name to test the archive reader's boundary.
                    entry.filename = name
                else:
                    entry = name
                bundle.writestr(entry, value)
        return {"archive_root": self.source_name,
                "sha256": hashlib.sha256(self.archive.read_bytes()).hexdigest()}

    def test_supported_targets_and_static_pic(self):
        expected = {("win", "x64"): "chromium-VC-WIN64A",
                    ("linux", "x64"): "linux-x86_64-clang",
                    ("linux", "arm64"): "linux-aarch64",
                    ("mac", "x64"): "darwin64-x86_64",
                    ("mac", "arm64"): "darwin64-arm64"}
        for (os_name, cpu), target in expected.items():
            with self.subTest(os=os_name, cpu=cpu):
                args = build.configure_arguments({
                    "target_os": os_name, "target_cpu": cpu, "debug": False,
                    "sysroot": "/sdk path", "deployment_target": "13.0"})
                self.assertEqual(args[0], target)
                self.assertIn("no-shared", args)
                self.assertIn("enable-async", args)
                self.assertIn("enable-ntls", args)
                self.assertNotIn("no-asm", args)
                if os_name != "win":
                    self.assertIn("-fPIC", args)
                    self.assertIn("-fvisibility=hidden", args)
                if os_name == "linux":
                    self.assertIn("--sysroot=/sdk path", shlex.split(" ".join(args)))
                if os_name == "mac":
                    self.assertIn("-isysroot/sdk path", shlex.split(" ".join(args)))
                    self.assertNotIn("/sdk path", args)

    def test_debug_configuration_is_explicit(self):
        args = build.configure_arguments({"target_os": "win", "target_cpu": "x64",
                                          "debug": True})
        self.assertIn("--debug", args)
        self.assertNotIn("--release", args)

    def test_windows_tool_paths_are_not_double_quoted(self):
        tools = {"compiler": "C:/LLVM path/clang-cl.exe",
                 "archiver": "C:/Program Files/VS/lib.exe"}
        env = build.compiler_environment("win", tools)
        self.assertEqual(env["CC"], tools["compiler"])
        self.assertEqual(env["AR"], tools["archiver"])
        # The generated Windows make template adds the one required pair.
        self.assertEqual('"' + env["AR"] + '"', '"C:/Program Files/VS/lib.exe"')

    def test_posix_tool_paths_preserve_spaces(self):
        tools = {"compiler": "/LLVM path/clang", "archiver": "/LLVM path/llvm-ar"}
        env = build.compiler_environment("linux", tools)
        self.assertEqual(shlex.split(env["CC"]), [tools["compiler"]])
        self.assertEqual(shlex.split(env["RANLIB"]), [tools["archiver"], "s"])

    def test_unsupported_architectures_fail(self):
        for os_name, cpu in [("win", "x86"), ("win", "arm64"),
                             ("linux", "x86"), ("android", "arm64")]:
            with self.subTest(os=os_name, cpu=cpu), self.assertRaises(ValueError):
                build.configure_arguments({"target_os": os_name, "target_cpu": cpu,
                                           "debug": False})

    def test_posix_requires_sdk_and_mac_deployment_target(self):
        for config in [
                {"target_os": "linux", "target_cpu": "x64", "debug": False},
                {"target_os": "mac", "target_cpu": "arm64", "debug": False,
                 "sysroot": "/sdk"}]:
            with self.assertRaises(ValueError):
                build.configure_arguments(config)

    def test_wrong_archive_hash_preserves_existing_output(self):
        spec = self.make_archive()
        self.output.mkdir()
        sentinel = self.output / "previous-result"
        sentinel.write_text("keep")
        spec["sha256"] = "0" * 64
        with self.assertRaisesRegex(ValueError, "SHA-256"):
            build.prepare_source(self.archive, self.output, spec)
        self.assertEqual(sentinel.read_text(), "keep")

    def test_unsafe_paths_rejected_before_existing_source_is_modified(self):
        for suffix in ["../escape", "dir/../../escape", "file:stream",
                       "dir\\file", "trailing. ", "dir/../file"]:
            with self.subTest(suffix=suffix):
                spec = self.make_archive([(self.source_name + "/" + suffix, "bad")])
                previous = self.output / self.source_name
                previous.mkdir(parents=True, exist_ok=True)
                sentinel = previous / "sentinel"
                sentinel.write_text("keep")
                (self.output / ".tongsuo-managed").write_text(self.source_name)
                with self.assertRaises(ValueError):
                    build.prepare_source(self.archive, self.output, spec)
                self.assertEqual(sentinel.read_text(), "keep")

    def test_wrong_archive_root_rejected(self):
        spec = self.make_archive([("other-root/file", "bad")])
        with self.assertRaises(ValueError):
            build.prepare_source(self.archive, self.output, spec)
        self.assertFalse(self.output.exists())

    def test_archive_symlink_rejected(self):
        entry = zipfile.ZipInfo(self.source_name + "/symlink")
        entry.create_system = 3
        entry.external_attr = (stat.S_IFLNK | 0o777) << 16
        spec = self.make_archive([(entry, "../../outside")])
        with self.assertRaisesRegex(ValueError, "non-regular"):
            build.prepare_source(self.archive, self.output, spec)

    def test_case_aliases_rejected(self):
        spec = self.make_archive([(self.source_name + "/FILE", "a"),
                                  (self.source_name + "/file", "b")])
        with self.assertRaisesRegex(ValueError, "duplicate"):
            build.prepare_source(self.archive, self.output, spec)

    def test_unmanaged_directory_is_not_deleted(self):
        spec = self.make_archive()
        previous = self.output / self.source_name
        previous.mkdir(parents=True)
        sentinel = previous / "sentinel"
        sentinel.write_text("keep")
        with self.assertRaisesRegex(ValueError, "unmanaged"):
            build.prepare_source(self.archive, self.output, spec)
        self.assertEqual(sentinel.read_text(), "keep")

    def test_ninja_precreated_output_directories_are_accepted(self):
        spec = self.make_archive()
        source = self.output / self.source_name
        (source / "include/openssl").mkdir(parents=True)
        build.prepare_source(self.archive, self.output, spec)
        self.assertEqual((source / "include/openssl/ssl.h").read_text(), "new header")
        self.assertTrue((self.output / ".tongsuo-managed").is_file())

    def test_empty_file_is_not_mistaken_for_output_scaffolding(self):
        spec = self.make_archive()
        source = self.output / self.source_name
        (source / "include/openssl").mkdir(parents=True)
        sentinel = source / "include/openssl/empty.h"
        sentinel.touch()
        with self.assertRaisesRegex(ValueError, "unmanaged"):
            build.prepare_source(self.archive, self.output, spec)
        self.assertTrue(sentinel.is_file())

    def test_action_rerun_removes_stale_objects_and_generated_headers(self):
        spec = self.make_archive()
        source = build.prepare_source(self.archive, self.output, spec)
        (source / "stale.obj").write_text("previous compiler/configuration")
        (source / "include/openssl/ssl.h").write_text("old generated configuration")
        source = build.prepare_source(self.archive, self.output, spec)
        self.assertFalse((source / "stale.obj").exists())
        self.assertEqual((source / "include/openssl/ssl.h").read_text(), "new header")

    def test_depfile_has_make_continuations_and_escaped_paths(self):
        path = self.root / "output.d"
        build.write_depfile(path, self.root / "lib.a",
                            {self.root / "with space.h", self.root / "hash#dollar$.h"})
        result = path.read_text()
        self.assertIn(" \\\n  ", result)
        self.assertNotIn("\n+", result)
        self.assertIn("with\\ space.h", result)
        self.assertIn("hash\\#dollar$$.h", result)


if __name__ == "__main__":
    unittest.main()
