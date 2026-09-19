#!/usr/bin/env python3
# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Player compatibility commands preserve releases and reject partial imports."""

import contextlib
import io
from pathlib import Path
import tempfile
import unittest

from import_electron_app import inventory
import sync_xenon_player as sync
import sync_xenon_player_frontend as frontend


class PlayerImportEntryTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='player-import-entry-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.source = self.root / 'release'
        app = self.source / 'resources'
        app.mkdir(parents=True)
        (app / 'app.asar').write_bytes(b'opaque application\x00\xff')
        external = app / 'app.asar.unpacked/native'
        external.mkdir(parents=True)
        (external / 'single.node').write_bytes(b'native')
        self.output = self.root / 'resources'
        self.target = self.output / 'xenon_player'
        self.args = ['--src', str(self.source), '--out', str(self.output)]

    def test_complete_release_has_one_native_and_no_generated_main_or_frontend(self):
        before = inventory(self.source)
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(sync.main(self.args), 0)
        self.assertEqual(inventory(self.target), before)
        self.assertEqual(inventory(self.source), before)
        self.assertFalse((self.target / 'main').exists())
        self.assertFalse((self.target / 'frontend').exists())
        self.assertEqual(len(list(self.target.rglob('*.node'))), 1)

    def test_check_reports_extra_file_without_removing_it(self):
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(sync.main(self.args), 0)
            self.assertEqual(sync.main(self.args + ['--check']), 0)
            extra = self.target / 'extra'
            extra.write_bytes(b'extra')
            self.assertEqual(sync.main(self.args + ['--check']), 1)
        self.assertEqual(extra.read_bytes(), b'extra')

    def test_retired_assembly_options_are_explicitly_rejected(self):
        for argument in ('--player-sdk', '--native-dir', '--app-version',
                         '--source-archive', '--vendor-asar-module',
                         '--asar-public-key', '--include-maps'):
            with self.subTest(argument=argument):
                errors = io.StringIO()
                with contextlib.redirect_stderr(errors), self.assertRaises(SystemExit) as error:
                    sync.main(self.args + [argument])
                self.assertEqual(error.exception.code, 2)
                self.assertIn('provide one complete release', errors.getvalue())
        self.assertFalse(self.output.exists())

    def test_frontend_only_command_always_fails_without_writing(self):
        errors = io.StringIO()
        with contextlib.redirect_stderr(errors):
            self.assertEqual(frontend.main(self.args), 2)
        self.assertIn('frontend-only synchronization is retired', errors.getvalue())
        self.assertIn('import_electron_app.py', errors.getvalue())
        self.assertFalse(self.output.exists())

    def test_unassembled_build_rejected_without_removing_old_runtime(self):
        build = self.root / 'build'
        build.mkdir()
        (build / 'main.js').write_bytes(b'compiled main')
        self.target.mkdir(parents=True)
        (self.target / 'keep').write_bytes(b'old runtime')
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(sync.main(['--src', str(build), '--out', str(self.output)]), 1)
        self.assertEqual((self.target / 'keep').read_bytes(), b'old runtime')


if __name__ == '__main__':
    unittest.main()
