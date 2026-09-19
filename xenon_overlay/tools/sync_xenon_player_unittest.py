#!/usr/bin/env python3
# Copyright 2026 The Xenon Overlay Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import contextlib
import io
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest import mock

from asar_sync_support_unittest import PUBLIC_KEY, standard_asar
import sync_xenon_player as sync


class PlayerArchiveSyncTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='player-sync-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.fixture('app/build/main.js', b'module.exports = {};')
        self.fixture('app/build/main-renderer/index.html', b'<html></html>')
        self.fixture('sdk/xmp.exe')
        for name in ('dk_addon.node', 'pc_addon.node', 'player_helper.node',
                     'xmp_helper.node'):
            self.fixture('sdk/' + name)
        self.args = ['sync', '--src', str(self.root / 'app/build'),
                     '--out', str(self.root / 'out'),
                     '--player-sdk', str(self.root / 'sdk')]
        self.runtime = self.root / 'out/xenon_player/main'

    def fixture(self, relative, contents=b'fixture'):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(contents)
        return path

    @unittest.skipUnless(shutil.which('node'), 'Node.js public key validation')
    def test_preserves_copied_archive_and_publishes_public_key(self):
        original = b'opaque encrypted archive payload'
        self.fixture('sdk/resources/app/out.asar', original)
        public_key = self.fixture('public/rsa-pub.pem', PUBLIC_KEY)
        args = self.args + ['--asar-public-key', str(public_key)]
        # Standalone preload normalization remains a separate pipeline.
        self.fixture('app/build/preload/bridge.js', b'encoded preload')
        with mock.patch.object(sys, 'argv', args), \
                mock.patch.object(sync, 'normalize_preloads', return_value=(
                    {'preload/bridge.js': b'/* decoded source */'}, 1)) as preloads, \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(sync.main(), 0)
        preloads.assert_called_once()
        self.assertEqual((self.runtime / 'resources/app/out.asar').read_bytes(), original)
        self.assertEqual((self.runtime / 'resources/app/asar-public-key.pem').read_bytes(),
                         PUBLIC_KEY)
        self.assertEqual((self.runtime / 'preload/bridge.js').read_bytes(),
                         b'/* decoded source */')

    @unittest.skipUnless(shutil.which('node'), 'Node.js public key validation')
    def test_default_key_is_relative_to_explicit_vendor_module(self):
        self.fixture('sdk/resources/app/out.asar', b'encrypted fixture')
        module = self.fixture('custom/lib/asar.js', b'')
        self.fixture('custom/lib/rsa-pub.pem', PUBLIC_KEY)
        args = self.args + ['--vendor-asar-module', str(module)]
        with mock.patch.object(sys, 'argv', args), \
                mock.patch.object(sync, 'normalize_preloads', return_value=({}, 0)), \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(sync.main(), 0)
        self.assertEqual((self.runtime / 'resources/app/asar-public-key.pem').read_bytes(),
                         PUBLIC_KEY)

    def test_standard_only_input_without_key_succeeds(self):
        original = standard_asar()
        self.fixture('sdk/resources/app/plain.asar', original)
        with mock.patch.object(sys, 'argv', self.args), \
                mock.patch.object(sync, 'normalize_preloads', return_value=({}, 0)), \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(sync.main(), 0)
        self.assertEqual((self.runtime / 'resources/app/plain.asar').read_bytes(), original)
        self.assertFalse((self.runtime / 'resources/app/asar-public-key.pem').exists())

    def test_missing_or_private_key_does_not_replace_runtime(self):
        self.fixture('sdk/resources/app/out.asar', b'encrypted fixture')
        main_sentinel = self.fixture('out/xenon_player/main/keep')
        frontend_sentinel = self.fixture('out/xenon_player/frontend/keep')
        for invalid_private in (False, True):
            with self.subTest(private_key=invalid_private):
                if invalid_private:
                    self.fixture('app/asar/security-asar/lib/rsa-pub.pem',
                                 b'-----BEGIN PRIVATE KEY-----\nsecret\n-----END PRIVATE KEY-----')
                errors = io.StringIO()
                with mock.patch.object(sys, 'argv', self.args), \
                        mock.patch.object(sync, 'normalize_preloads') as preloads, \
                        contextlib.redirect_stderr(errors):
                    self.assertEqual(sync.main(), 1)
                preloads.assert_not_called()
                self.assertNotIn('secret', errors.getvalue())
                self.assertEqual(main_sentinel.read_bytes(), b'fixture')
                self.assertEqual(frontend_sentinel.read_bytes(), b'fixture')


if __name__ == '__main__':
    unittest.main()
