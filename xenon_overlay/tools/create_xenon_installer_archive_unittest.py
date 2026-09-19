#!/usr/bin/env python3
# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Exercise the actual Chromium staging pipeline with opaque release fixtures."""

import contextlib
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest import mock

import create_xenon_installer_archive as wrapper
from import_electron_app import inventory


class InstallerPayloadTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='installer-payload-test-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.build = self.root / 'build'
        self.build.mkdir()
        self.staging = self.root / 'staging'
        self.release = self.root / 'chrome.release'
        self.release.write_text('[GENERAL]\nhost.exe: %(ChromeDir)s/\n',
                                encoding='utf-8')
        (self.build / 'host.exe').write_bytes(b'chrome host fixture')
        self.first = self.build / 'application-a'
        self.second = self.build / 'application-b'
        self.write(self.first, 'resources/app/package.json',
                   b'{"main":"out.asar/main.js"}')
        self.write(self.first, 'resources/app/out.asar', b'encrypted\x00\xfe\xff')
        self.write(self.first, 'resources/app/out.asar.unpacked/addon.node', b'native')
        self.write(self.first, '.hidden-file', b'hidden')
        self.write(self.first, '.cache/opaque.bin', b'preserve relative path')
        self.write(self.first, 'logs/packaged.log', b'no product filename filter')
        self.write(self.first, 'XDASKernel.dll', b'opaque shipped library')
        (self.first / 'empty-directory').mkdir()
        self.write(self.second, 'resources/app.asar', b'second opaque archive')
        self.write(self.second, 'single.node', b'one native copy')
        self.write(self.build, 'application-a.xenon.json', b'{"application":"application-a"}')
        self.write(self.build, 'application-a.asar-public-key.pem', b'public fixture')
        self.base_args = ['--build_dir', str(self.build), '--staging_dir', str(self.staging),
                          '--input_file', str(self.release), '--custom_version=1.2.3.4']
        self.payload_args = [
            '--xenon-payload-directory=application-a',
            '--xenon-payload-directory=application-b',
            '--xenon-payload-file=application-a.xenon.json',
            '--xenon-payload-file=application-a.asar-public-key.pem']

    def write(self, root, relative, data=b'fixture'):
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return path

    def run_staging(self, extra=(), payloads=True):
        output = io.StringIO()
        args = self.base_args + (self.payload_args if payloads else [])
        with contextlib.redirect_stdout(output):
            result = wrapper.main(args + ['--xenon-staging-only', *extra])
        report = json.loads(output.getvalue()) if result == 0 else None
        return result, report

    def staged_root(self):
        return self.staging / 'temp_installer_archive/Chrome-bin'

    def test_staging_only_preserves_complete_trees_and_external_files(self):
        before_a, before_b = inventory(self.first), inventory(self.second)
        result, report = self.run_staging(['--build_time=1700000000'])
        self.assertEqual(result, 0)
        staged = self.staged_root()
        self.assertEqual(inventory(staged / self.first.name), before_a)
        self.assertEqual(inventory(staged / self.second.name), before_b)
        self.assertEqual(inventory(self.first), before_a)
        self.assertEqual(inventory(self.second), before_b)
        for name in ('host.exe', 'application-a.xenon.json',
                     'application-a.asar-public-key.pem'):
            self.assertEqual((staged / name).read_bytes(), (self.build / name).read_bytes())
        self.assertEqual(int((staged / 'application-a').stat().st_mtime),
                         1700000000)
        self.assertTrue(all(app['verified'] for app in report['applications']))
        self.assertFalse((self.build / 'chrome.7z').exists())
        self.assertFalse((self.build / 'setup.ex_').exists())

    def test_no_payload_arguments_leave_unrelated_build_directories_out(self):
        result, report = self.run_staging(payloads=False)
        self.assertEqual(result, 0)
        self.assertEqual(report['applications'], [])
        self.assertEqual(report['external_files'], [])
        self.assertEqual([path.name for path in self.staged_root().iterdir()], ['host.exe'])

    def test_optional_globs_only_add_matching_external_regular_files(self):
        self.write(self.build, 'msvcp140.dll', b'vc runtime')
        self.write(self.build, 'msvcp140_1.dll', b'additional runtime')
        result, report = self.run_staging([
            '--xenon-payload-glob=msvcp140*.dll',
            '--xenon-payload-glob=not-present*.dll'])
        self.assertEqual(result, 0)
        self.assertIn('msvcp140.dll', report['external_files'])
        self.assertIn('msvcp140_1.dll', report['external_files'])

    def test_canonical_source_rejects_build_cache_or_mixed_version_before_staging(self):
        canonical = self.root / 'canonical' / self.first.name
        shutil.copytree(self.first, canonical)
        extra = ['--xenon-payload-source=application-a=' + str(canonical)]
        self.assertEqual(self.run_staging(extra)[0], 0)
        sentinel = self.write(self.staged_root(), 'keep-previous-staging', b'keep')
        self.write(self.first, 'new-runtime-cache', b'not a release file')
        errors = io.StringIO()
        with contextlib.redirect_stderr(errors):
            self.assertEqual(self.run_staging(extra)[0], 1)
        self.assertIn('resynchronize the complete release', errors.getvalue())
        self.assertEqual(sentinel.read_bytes(), b'keep')
        (self.first / 'new-runtime-cache').unlink()
        self.write(self.first, 'resources/app/out.asar', b'another release')
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(self.run_staging(extra)[0], 1)
        self.assertEqual(sentinel.read_bytes(), b'keep')

    def test_glob_parent_is_dependency_even_before_new_library_exists(self):
        archive = wrapper.load_archive_module()
        with mock.patch.object(wrapper, 'load_archive_module', return_value=archive):
            result, _ = self.run_staging(['--xenon-payload-glob=msvcp140*.dll'])
        self.assertEqual(result, 0)
        self.assertIn('.', archive.g_archive_inputs)
        self.write(self.build, 'msvcp140_new.dll', b'new runtime')
        result, report = self.run_staging(['--xenon-payload-glob=msvcp140*.dll'])
        self.assertEqual(result, 0)
        self.assertIn('msvcp140_new.dll', report['external_files'])

    def test_missing_required_file_keeps_existing_staging_untouched(self):
        sentinel = self.write(self.staged_root(), 'keep', b'old staging')
        with contextlib.redirect_stderr(io.StringIO()):
            result, _ = self.run_staging(['--xenon-required-file=missing.asar'])
        self.assertEqual(result, 1)
        self.assertEqual(sentinel.read_bytes(), b'old staging')

    def test_invalid_relative_payload_and_overlaps_are_rejected_before_staging(self):
        for extra in (['--xenon-payload-file=../outside'],
                      ['--xenon-payload-directory=application-a/resources'],
                      ['--xenon-payload-file=application-a/resources/app/out.asar']):
            with self.subTest(extra=extra), contextlib.redirect_stderr(io.StringIO()):
                result, _ = self.run_staging(extra)
                self.assertEqual(result, 1)
        self.assertFalse(self.staging.exists())

    def test_staging_cannot_be_placed_inside_application_tree(self):
        before = inventory(self.first)
        args = self.base_args + ['--staging_dir', str(self.first / 'stage')]
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(wrapper.main(args + self.payload_args +
                                          ['--xenon-staging-only']), 1)
        self.assertEqual(inventory(self.first), before)

    def test_real_archive_pipeline_receives_verified_payload_and_depfile_inputs(self):
        archive = wrapper.load_archive_module()
        captured = []

        def archive_stub(options, staged, *arguments):
            staged = Path(staged) / 'Chrome-bin'
            self.assertEqual(inventory(staged / self.first.name), inventory(self.first))
            captured.extend(archive.g_archive_inputs)
            return 'chrome.packed.7z', 'chrome.7z'

        archive.CreateArchiveFiles = mock.Mock(side_effect=archive_stub)
        archive.PrepareSetupExec = mock.Mock(return_value='setup.ex_')
        archive.CreateResourceInputFile = mock.Mock()
        with mock.patch.object(wrapper, 'load_archive_module', return_value=archive), \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(wrapper.main(self.base_args + self.payload_args), 0)
        normalized = {name.replace('\\', '/') for name in captured}
        self.assertIn('application-a/resources/app/out.asar', normalized)
        self.assertIn('application-a/.hidden-file', normalized)
        self.assertIn('application-a/empty-directory', normalized)
        self.assertIn('application-a.xenon.json', normalized)
        archive.CreateResourceInputFile.assert_called_once()

    def test_post_staging_change_is_detected_before_archive_creation(self):
        original_stage = wrapper.stage_payloads

        def damaged_stage(archive, options, staged, directories, files):
            original_stage(archive, options, staged, directories, files)
            (Path(staged) / 'Chrome-bin/application-a/resources/app/out.asar').write_bytes(
                b'corrupt staging')

        with mock.patch.object(wrapper, 'stage_payloads', side_effect=damaged_stage), \
                contextlib.redirect_stderr(io.StringIO()):
            result, _ = self.run_staging()
        self.assertEqual(result, 1)
        self.assertFalse((self.build / 'chrome.7z').exists())

    def test_real_7z_roundtrip_preserves_application_layout_and_bytes(self):
        executable = ('7za.exe' if os.name == 'nt' else '7za')
        tool = (Path(__file__).resolve().parents[2] /
                'third_party/lzma_sdk/bin/host_platform' / executable)
        if not tool.is_file():
            self.skipTest('bundled 7za is unavailable')
        result, _ = self.run_staging()
        self.assertEqual(result, 0)
        archive_path = self.root / 'roundtrip.7z'
        extracted = self.root / 'extracted'
        flags = subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0
        subprocess.run([str(tool), 'a', '-t7z', str(archive_path),
                        str(self.staged_root()), '-mx0'], check=True,
                       capture_output=True, creationflags=flags)
        subprocess.run([str(tool), 'x', str(archive_path), '-o' + str(extracted), '-y'],
                       check=True, capture_output=True, creationflags=flags)
        self.assertEqual(inventory(extracted / 'Chrome-bin/application-a'),
                         inventory(self.first))
        self.assertEqual(inventory(extracted / 'Chrome-bin/application-b'),
                         inventory(self.second))
        self.assertEqual(
            (extracted / 'Chrome-bin/application-a.xenon.json').read_bytes(),
            (self.build / 'application-a.xenon.json').read_bytes())


if __name__ == '__main__':
    unittest.main()
