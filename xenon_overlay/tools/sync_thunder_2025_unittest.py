#!/usr/bin/env python3
# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""The legacy Thunder command imports an already complete release unchanged."""

import contextlib
import io
import json
from pathlib import Path
import tempfile
import unittest

from import_electron_app import inventory
import sync_thunder_2025 as sync


class ThunderImportEntryTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='thunder-import-entry-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.source = self.root / 'release'
        self.app = self.source / 'resources/app'
        self.app.mkdir(parents=True)
        (self.app / 'package.json').write_text(
            '{"name":"fixture","main":"out.asar/main.js"}', encoding='utf-8')
        (self.app / 'out.asar').write_bytes(b'opaque encrypted archive\x00\xff')
        (self.source / 'single.node').write_bytes(b'native')
        self.output = self.root / 'resources'
        self.target = self.output / 'thunder_2025'
        self.args = ['--src', str(self.source), '--out', str(self.output)]

    def test_complete_release_import_preserves_layout_and_external_manifest(self):
        manifest = self.root / 'manifest.json'
        before = inventory(self.source)
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(sync.main(self.args + ['--manifest', str(manifest)]), 0)
        self.assertEqual(inventory(self.target), before)
        self.assertEqual(inventory(self.source), before)
        self.assertFalse((self.target / 'resources/app/renderer.asar').exists())
        self.assertFalse((self.target / 'resources/app/out').exists())
        self.assertEqual(json.loads(manifest.read_text(encoding='utf-8'))['files'],
                         before['files'])

    def test_check_reports_changed_file_without_resynchronizing(self):
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(sync.main(self.args), 0)
            self.assertEqual(sync.main(self.args + ['--check']), 0)
            (self.target / 'single.node').write_bytes(b'changed')
            self.assertEqual(sync.main(self.args + ['--check']), 1)
        self.assertEqual((self.target / 'single.node').read_bytes(), b'changed')

    def test_retired_assembly_options_are_explicitly_rejected(self):
        for argument in ('--player-sdk', '--plugins-dir', '--asar-format',
                         '--asar-public-key', '--include-maps', '--skip-asar'):
            with self.subTest(argument=argument):
                errors = io.StringIO()
                with contextlib.redirect_stderr(errors), self.assertRaises(SystemExit) as error:
                    sync.main(self.args + [argument])
                self.assertEqual(error.exception.code, 2)
                self.assertIn('provide one complete release', errors.getvalue())
        self.assertFalse(self.output.exists())

    def test_unassembled_build_rejected_without_removing_old_runtime(self):
        build = self.root / 'dist'
        build.mkdir()
        (build / 'main.js').write_bytes(b'compiled main')
        self.target.mkdir(parents=True)
        (self.target / 'keep').write_bytes(b'old runtime')
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(sync.main(['--src', str(build), '--out', str(self.output)]), 1)
        self.assertEqual((self.target / 'keep').read_bytes(), b'old runtime')


if __name__ == '__main__':
    unittest.main()
