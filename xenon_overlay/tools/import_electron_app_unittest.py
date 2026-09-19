#!/usr/bin/env python3
# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Exact release import, preflight and replacement rollback contracts."""

import contextlib
import io
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

import import_electron_app as importer


class ImportApplicationTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='electron-import-test-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.source = self.root / 'release'
        self.target = self.root / 'applications' / 'sample'
        self.source.mkdir()
        self.manifest = self.root / 'manifests' / 'sample.json'

    def write(self, root, relative, contents=b'file contents'):
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(contents)
        return path

    def release(self):
        self.write(self.source, 'resources/app/package.json',
                   b'{"name":"sample","version":"1.2.3","main":"out.asar/main.js"}')
        self.write(self.source, 'resources/app/out.asar',
                   b'opaque encrypted archive\x00\xff\xfe')
        self.write(self.source, 'resources/app/out.asar.unpacked/native/addon.node',
                   b'opaque native module\x00\xff')
        self.write(self.source, 'native/addon.node', b'another native module')
        self.write(self.source, 'arbitrary-host-name.exe', b'executable')
        (self.source / 'empty-directory').mkdir()

    def old_release(self):
        return self.write(self.target, 'keep-old-release', b'old runtime')

    def assert_old_retained(self):
        self.assertEqual((self.target / 'keep-old-release').read_bytes(), b'old runtime')
        self.assertEqual(list(self.target.iterdir()), [self.target / 'keep-old-release'])
        self.assertEqual(list(self.target.parent.glob('.sample.stage-*')), [])
        self.assertEqual(list(self.target.parent.glob('.sample.backup-*')), [])

    def test_preserves_archives_unpacked_native_paths_and_empty_directories(self):
        self.release()
        before = importer.inventory(self.source)
        result = importer.import_application(self.source, self.target, self.manifest)
        self.assertEqual(importer.inventory(self.source), before)
        self.assertEqual(importer.inventory(self.target), before)
        self.assertEqual(result['layout']['kind'], 'electron-release')
        self.assertEqual(json.loads(self.manifest.read_text(encoding='utf-8')), result)
        self.assertTrue(importer.check_application(self.source, self.target)['equal'])
        self.assertFalse((self.target / 'build').exists())
        self.assertFalse((self.target / 'manifest.json').exists())

    def test_archive_layout_does_not_require_executable_or_open_archive(self):
        self.write(self.source, 'resources/app.asar', b'not a parseable archive')
        self.write(self.source, 'resources/app.asar.unpacked/addon.node')
        result = importer.import_application(self.source, self.target)
        self.assertEqual(result['layout']['candidates'], [
            {'path': 'resources/app.asar', 'kind': 'archive'}])
        self.assertEqual((self.target / 'resources/app.asar').read_bytes(),
                         b'not a parseable archive')

    def test_application_directory_package_and_multiple_candidates_are_recorded(self):
        self.write(self.source, 'package.json', b'{"main":"index.js"}')
        self.assertEqual(importer.detect_layout(self.source)['kind'],
                         'application-directory')
        self.write(self.source, 'resources/app.asar')
        layout = importer.detect_layout(self.source)
        self.assertEqual(layout['kind'], 'electron-release')
        self.assertEqual(len(layout['candidates']), 2)

    def test_mac_bundle_layout_is_preserved_without_relocating_resources(self):
        self.write(self.source, 'Contents/Resources/app.asar', b'opaque archive')
        self.write(self.source, 'Contents/MacOS/application', b'executable')
        result = importer.import_application(self.source, self.target)
        self.assertEqual(result['layout']['kind'], 'electron-release')
        self.assertEqual(result['layout']['candidates'], [
            {'path': 'Contents/Resources/app.asar', 'kind': 'archive'}])
        self.assertTrue(importer.check_application(self.source, self.target)['equal'])

    def test_repeat_import_removes_obsolete_files_and_adds_new_files(self):
        self.release()
        importer.import_application(self.source, self.target)
        (self.source / 'native/addon.node').unlink()
        self.write(self.source, 'new/location.js', b'new source')
        self.write(self.source, 'resources/app/out.asar', b'changed archive')
        importer.import_application(self.source, self.target)
        self.assertTrue(importer.check_application(self.source, self.target)['equal'])
        self.assertFalse((self.target / 'native/addon.node').exists())
        self.assertEqual(list(self.target.parent.glob('.sample.backup-*')), [])

    def test_check_detects_missing_extra_changed_files_and_empty_directories(self):
        self.release()
        importer.import_application(self.source, self.target)
        (self.target / 'native/addon.node').unlink()
        self.write(self.target, 'extra.js')
        self.write(self.target, 'resources/app/out.asar', b'changed bytes')
        (self.target / 'empty-directory').rmdir()
        before = importer.inventory(self.target)
        result = importer.check_application(self.source, self.target)
        self.assertFalse(result['equal'])
        self.assertEqual(result['missing_files'], ['native/addon.node'])
        self.assertEqual(result['extra_files'], ['extra.js'])
        self.assertEqual(result['changed_files'], ['resources/app/out.asar'])
        self.assertEqual(result['missing_directories'], ['empty-directory'])
        self.assertEqual(importer.inventory(self.target), before)

    def test_overlap_and_manifest_inside_payload_are_rejected_before_changes(self):
        self.release()
        old = self.old_release()
        for source, target, manifest in (
                (self.source, self.source, None),
                (self.source, self.source / 'nested', None),
                (self.source, self.root, None),
                (self.source, self.target, self.source / 'manifest.json'),
                (self.source, self.target, self.target / 'manifest.json')):
            with self.subTest(source=source, target=target, manifest=manifest):
                with self.assertRaises(importer.ImportFailure):
                    importer.import_application(source, target, manifest)
        self.assertEqual(old.read_bytes(), b'old runtime')

    def test_invalid_layout_and_metadata_keep_previous_release(self):
        self.old_release()
        with self.assertRaisesRegex(importer.ImportFailure, 'expected resources'):
            importer.import_application(self.source, self.target)
        self.write(self.source, 'package.json', b'not json')
        with self.assertRaisesRegex(importer.ImportFailure, 'invalid package'):
            importer.import_application(self.source, self.target)
        self.assert_old_retained()

    def test_copy_failure_keeps_previous_release_and_manifest(self):
        self.release()
        self.old_release()
        self.write(self.manifest.parent, self.manifest.name, b'old manifest')
        with mock.patch.object(importer.shutil, 'copy2', side_effect=OSError('copy failed')):
            with self.assertRaises(OSError):
                importer.import_application(self.source, self.target, self.manifest)
        self.assert_old_retained()
        self.assertEqual(self.manifest.read_bytes(), b'old manifest')

    def test_source_changes_during_copy_are_rejected_before_replacement(self):
        self.release()
        self.old_release()
        original_copy = importer.shutil.copy2

        def changing_copy(source, target, **kwargs):
            result = original_copy(source, target, **kwargs)
            if source.name == 'out.asar':
                source.write_bytes(b'changed during import')
            return result

        with mock.patch.object(importer.shutil, 'copy2', side_effect=changing_copy):
            with self.assertRaisesRegex(importer.ImportFailure, 'changed during import'):
                importer.import_application(self.source, self.target)
        self.assert_old_retained()

    def test_install_rename_failure_rolls_back_previous_release(self):
        self.release()
        self.old_release()
        original_replace = importer.os.replace

        def fail_install(source, target):
            if '.stage-' in source.name and target == self.target:
                raise OSError('locked installation')
            return original_replace(source, target)

        with mock.patch.object(importer.os, 'replace', side_effect=fail_install):
            with self.assertRaisesRegex(importer.ImportFailure, 'previous directory restored'):
                importer.import_application(self.source, self.target)
        self.assert_old_retained()

    def test_manifest_replace_failure_rolls_back_previous_release(self):
        self.release()
        self.old_release()
        self.write(self.manifest.parent, self.manifest.name, b'old manifest')
        original_replace = importer.os.replace

        def fail_manifest(source, target):
            if target == self.manifest:
                raise OSError('locked manifest')
            return original_replace(source, target)

        with mock.patch.object(importer.os, 'replace', side_effect=fail_manifest):
            with self.assertRaisesRegex(importer.ImportFailure, 'previous directory restored'):
                importer.import_application(self.source, self.target, self.manifest)
        self.assert_old_retained()
        self.assertEqual(self.manifest.read_bytes(), b'old manifest')

    def test_failed_rollback_retains_original_directory_at_reported_backup(self):
        self.release()
        self.old_release()
        original_replace = importer.os.replace

        def locked_destination(source, target):
            if target == self.target:
                raise OSError('destination remains locked')
            return original_replace(source, target)

        with mock.patch.object(importer.os, 'replace', side_effect=locked_destination):
            with self.assertRaisesRegex(importer.ImportFailure, 'original directory retained'):
                importer.import_application(self.source, self.target)
        backups = list(self.target.parent.glob('.sample.backup-*'))
        self.assertEqual(len(backups), 1)
        self.assertEqual((backups[0] / 'keep-old-release').read_bytes(), b'old runtime')
        self.assertFalse(self.target.exists())

    def test_manifest_failure_on_first_import_restores_absent_target(self):
        self.release()
        original_replace = importer.os.replace

        def fail_manifest(source, target):
            if target == self.manifest:
                raise OSError('manifest cannot be installed')
            return original_replace(source, target)

        with mock.patch.object(importer.os, 'replace', side_effect=fail_manifest):
            with self.assertRaises(importer.ImportFailure):
                importer.import_application(self.source, self.target, self.manifest)
        self.assertFalse(self.target.exists())
        self.assertFalse(self.manifest.exists())

    def make_link(self, target, link):
        try:
            os.symlink(target, link, target_is_directory=target.is_dir())
        except OSError as error:
            self.skipTest(f'creating symlinks is unavailable: {error}')

    def test_source_symlink_is_rejected_without_following_external_file(self):
        self.release()
        self.old_release()
        external = self.write(self.root, 'outside.txt', b'outside')
        self.make_link(external, self.source / 'linked.txt')
        with self.assertRaisesRegex(importer.ImportFailure, 'links and reparse'):
            importer.import_application(self.source, self.target)
        self.assert_old_retained()
        self.assertEqual(external.read_bytes(), b'outside')

    def test_destination_ancestor_symlink_is_rejected(self):
        self.release()
        external = self.root / 'outside'
        external.mkdir()
        alias = self.root / 'alias'
        self.make_link(external, alias)
        with self.assertRaisesRegex(importer.ImportFailure, 'links and reparse'):
            importer.import_application(self.source, alias / 'application')
        self.assertEqual(list(external.iterdir()), [])

    def test_existing_destination_link_is_rejected_and_external_directory_untouched(self):
        self.release()
        self.old_release()
        external = self.root / 'outside'
        external.mkdir()
        self.make_link(external, self.target / 'linked-directory')
        with self.assertRaisesRegex(importer.ImportFailure, 'links and reparse'):
            importer.import_application(self.source, self.target)
        self.assertEqual((self.target / 'keep-old-release').read_bytes(), b'old runtime')
        self.assertTrue((self.target / 'linked-directory').is_symlink())
        self.assertEqual(list(external.iterdir()), [])

    @unittest.skipUnless(os.name == 'nt', 'Windows junction contract')
    def test_windows_junction_is_rejected_without_traversing_external_directory(self):
        self.release()
        self.old_release()
        external = self.root / 'outside'
        sentinel = self.write(external, 'sentinel', b'outside')
        junction = self.source / 'redirected-directory'
        subprocess.run(
            ['cmd.exe', '/d', '/c', 'mklink', '/J', str(junction), str(external)],
            check=True, capture_output=True,
            creationflags=subprocess.CREATE_NO_WINDOW)
        with self.assertRaisesRegex(importer.ImportFailure, 'links and reparse'):
            importer.import_application(self.source, self.target)
        self.assert_old_retained()
        self.assertEqual(sentinel.read_bytes(), b'outside')

    def test_check_cli_is_read_only_and_returns_nonzero_for_difference(self):
        self.release()
        importer.import_application(self.source, self.target)
        args = ['--src', str(self.source), '--out', str(self.target), '--check']
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.assertEqual(importer.main(args), 0)
        self.assertTrue(json.loads(output.getvalue())['equal'])
        self.write(self.target, 'extra')
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(importer.main(args), 1)


if __name__ == '__main__':
    unittest.main()
