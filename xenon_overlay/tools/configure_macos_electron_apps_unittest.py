#!/usr/bin/env python3
# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Test incremental bundle cleanup for the tracked Mac Electron applications."""

from pathlib import Path
import tempfile
import unittest

import configure_thunder_macos_resources as configuration


APPLICATIONS = (
    ('draw.io.app', 'drawio.xenon.json'),
    ('MarkText.app', 'marktext.xenon.json'),
    ('Motrix.app', 'motrix.xenon.json'),
)


class ConfigureMacElectronAppsTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.bundle = self.root / 'Browser.app'
        self.apps = self.bundle / 'Contents/Resources/xenon-apps'
        self.executables = self.bundle / 'Contents/MacOS'
        self.apps.mkdir(parents=True)
        self.executables.mkdir()
        for application, descriptor in APPLICATIONS:
            (self.apps / application).mkdir()
            (self.apps / application / 'keep').write_bytes(b'application')
            (self.apps / descriptor).write_text('current descriptor')
            (self.executables / descriptor).write_text('legacy descriptor')
            (self.apps / descriptor[:-len('.xenon.json')]).symlink_to(application)
        (self.apps / 'Thunder.app').mkdir()
        (self.apps / 'Thunder.app/keep').write_bytes(b'thunder')
        (self.apps / 'thunder_2025.xenon.json').write_text('thunder descriptor')
        (self.executables / 'Browser').write_bytes(b'host executable')

    def configure(self, enabled):
        configuration.configure_bundle(self.bundle, enabled, 'mac-electron-apps')

    def assert_unrelated_preserved(self):
        self.assertEqual((self.apps / 'Thunder.app/keep').read_bytes(), b'thunder')
        self.assertEqual((self.apps / 'thunder_2025.xenon.json').read_text(),
                         'thunder descriptor')
        self.assertEqual((self.executables / 'Browser').read_bytes(),
                         b'host executable')

    def test_enabled_preserves_payloads_and_removes_only_legacy_entries(self):
        self.configure(True)
        for application, descriptor in APPLICATIONS:
            self.assertEqual((self.apps / application / 'keep').read_bytes(),
                             b'application')
            self.assertEqual((self.apps / descriptor).read_text(), 'current descriptor')
            self.assertFalse((self.executables / descriptor).exists())
            self.assertFalse((self.apps / descriptor[:-len('.xenon.json')]).is_symlink())
        self.assert_unrelated_preserved()

    def test_disabled_removes_reserved_payloads_and_descriptors(self):
        self.configure(False)
        self.configure(False)
        for application, descriptor in APPLICATIONS:
            self.assertFalse((self.apps / application).exists())
            self.assertFalse((self.apps / descriptor).exists())
            self.assertFalse((self.executables / descriptor).exists())
            self.assertFalse((self.apps / descriptor[:-len('.xenon.json')]).is_symlink())
        self.assert_unrelated_preserved()

    def test_alias_cleanup_preserves_other_targets_and_real_directories(self):
        (self.apps / 'drawio').unlink()
        (self.apps / 'drawio').symlink_to('Thunder.app')
        (self.apps / 'marktext').unlink()
        (self.apps / 'marktext').mkdir()
        (self.apps / 'marktext/keep').write_bytes(b'user files')
        (self.apps / 'motrix').unlink()
        (self.apps / 'motrix').symlink_to('./Motrix.app')
        for enabled in (True, False):
            self.configure(enabled)
            self.assertEqual((self.apps / 'drawio').readlink(), Path('Thunder.app'))
            self.assertEqual((self.apps / 'marktext/keep').read_bytes(), b'user files')
            self.assertTrue((self.apps / 'motrix').is_symlink())
        self.assert_unrelated_preserved()

    def test_linked_resource_ancestor_is_rejected_before_any_removal(self):
        moved = self.root / 'outside'
        resources = self.bundle / 'Contents/Resources'
        resources.rename(moved)
        resources.symlink_to(moved, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, 'Linked bundle ancestor'):
            self.configure(False)
        self.assertEqual((self.executables / 'drawio.xenon.json').read_text(),
                         'legacy descriptor')
        self.assertTrue((moved / 'xenon-apps/drawio').is_symlink())
        self.assertEqual((moved / 'xenon-apps/draw.io.app/keep').read_bytes(),
                         b'application')


if __name__ == '__main__':
    unittest.main()
