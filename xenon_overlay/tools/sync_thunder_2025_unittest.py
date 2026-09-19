#!/usr/bin/env python3
# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Plugin release preflight and ASAR conversion contracts.

Set THUNDER_APP_ROOT to the Thunder source app directory to also exercise its
real standard/security ASAR build tools, using only temporary public fixtures.
"""

import contextlib
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

import sync_thunder_2025 as sync
from asar_sync_support_unittest import PUBLIC_KEY, standard_asar


class PluginReleaseTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='thunder-plugin-test-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.plugins = self.root / 'plugins'
        self.plugins.mkdir()

    def config(self, value):
        (self.plugins / 'config.json').write_text(json.dumps(value), encoding='utf-8')

    def fixture(self, relative, data=b'fixture'):
        target = self.root / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        return target

    def test_windows_loader_entries_use_config_keys(self):
        self.config({'directory': {}, 'nested/release': {'version': 'unused'},
                     'explicit.asar': {}, 'unpacked.asar': {}})
        self.fixture('plugins/directory/index.js')
        self.fixture('plugins/nested/release.asar')
        self.fixture('plugins/explicit.asar')
        self.fixture('plugins/unpacked.asar/index.js')
        sync.validate_plugins_dir(self.plugins)

    def test_missing_archive_and_entry_reports_declared_plugin(self):
        self.config({'nested/release': {}})
        self.fixture('plugins/nested/another-version.asar')
        with self.assertRaisesRegex(ValueError, 'nested/release: expected'):
            sync.validate_plugins_dir(self.plugins)

    def test_explicit_source_wins_without_fallback(self):
        sdk = self.root / 'sdk'
        fallback = sdk / 'resources/app/plugins'
        fallback.mkdir(parents=True)
        (fallback / 'config.json').write_text('{}', encoding='utf-8')
        selected = sync.select_plugins_dir(self.root / 'app/dist', sdk, self.plugins)
        self.assertEqual(selected, self.plugins)
        with self.assertRaisesRegex(ValueError, 'cannot read plugin config'):
            sync.validate_plugins_dir(selected)

    def test_invalid_config_and_external_entries_fail(self):
        for config in ([], {'../outside': {}}, {'C:/outside': {}}, {'': {}}):
            with self.subTest(config=config):
                self.config(config)
                with self.assertRaises(ValueError):
                    sync.validate_plugins_dir(self.plugins)

    def test_missing_plugins_preserve_existing_runtime_and_legacy_outputs(self):
        self.config({'nested/release': {}})
        self.fixture('app/dist/main.js')
        (self.root / 'app/dist/main-renderer').mkdir()
        sentinels = [self.fixture('out/' + name) for name in (
            'thunder_2025/keep', 'thunder_2025_main/keep',
            'thunder_2025_frontend/keep', 'thunder_2025_resources.asar', 'Thunder.exe')]
        argv = ['sync', '--src', str(self.root / 'app/dist'),
                '--out', str(self.root / 'out'), '--plugins-dir', str(self.plugins)]
        errors = io.StringIO()
        with mock.patch.object(sys, 'argv', argv), contextlib.redirect_stderr(errors):
            self.assertEqual(sync.main(), 1)
        self.assertIn('plugin release is incomplete', errors.getvalue())
        for sentinel in sentinels:
            self.assertEqual(sentinel.read_bytes(), b'fixture')

    def test_source_inside_replaced_runtime_is_rejected(self):
        self.fixture('app/dist/main.js')
        (self.root / 'app/dist/main-renderer').mkdir()
        plugins = self.root / 'out/thunder_2025/resources/app/plugins'
        plugins.mkdir(parents=True)
        config = plugins / 'config.json'
        config.write_text('{}', encoding='utf-8')
        argv = ['sync', '--src', str(self.root / 'app/dist'),
                '--out', str(self.root / 'out'), '--plugins-dir', str(plugins)]
        errors = io.StringIO()
        with mock.patch.object(sys, 'argv', argv), contextlib.redirect_stderr(errors):
            self.assertEqual(sync.main(), 1)
        self.assertIn('source would be removed', errors.getvalue())
        self.assertEqual(config.read_text(encoding='utf-8'), '{}')

    def sync_arguments(self):
        self.fixture('app/dist/main.js')
        (self.root / 'app/dist/main-renderer').mkdir(exist_ok=True)
        return ['sync', '--src', str(self.root / 'app/dist'),
                '--out', str(self.root / 'out'), '--plugins-dir', str(self.plugins),
                '--skip-asar']

    @unittest.skipUnless(shutil.which('node'), 'Node.js public key validation')
    def test_default_preserves_original_archives_and_publishes_public_key(self):
        self.config({'original': {}})
        original = b'opaque encrypted archive payload'
        self.fixture('plugins/original.asar', original)
        self.fixture('app/asar/security-asar/lib/rsa-pub.pem', PUBLIC_KEY)
        with mock.patch.object(sys, 'argv', self.sync_arguments()), \
                mock.patch.object(sync, 'normalize_plugin_asars') as normalize, \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(sync.main(), 0)
        normalize.assert_not_called()
        app = self.root / 'out/thunder_2025/resources/app'
        self.assertEqual((app / 'plugins/original.asar').read_bytes(), original)
        self.assertEqual((app / 'asar-public-key.pem').read_bytes(), PUBLIC_KEY)

    def test_standard_only_release_does_not_require_key_or_conversion(self):
        self.config({'plain': {}})
        self.fixture('plugins/plain.asar', standard_asar())
        with mock.patch.object(sys, 'argv', self.sync_arguments()), \
                mock.patch.object(sync, 'normalize_plugin_asars') as normalize, \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(sync.main(), 0)
        normalize.assert_not_called()
        self.assertFalse((self.root /
                          'out/thunder_2025/resources/app/asar-public-key.pem').exists())

    def test_encrypted_release_without_key_preserves_existing_runtime(self):
        self.config({'encrypted': {}})
        self.fixture('plugins/encrypted.asar', b'encrypted fixture')
        sentinel = self.fixture('out/thunder_2025/keep')
        errors = io.StringIO()
        with mock.patch.object(sys, 'argv', self.sync_arguments()), \
                contextlib.redirect_stderr(errors):
            self.assertEqual(sync.main(), 1)
        self.assertIn('requires --asar-public-key', errors.getvalue())
        self.assertEqual(sentinel.read_bytes(), b'fixture')

    def test_private_key_is_rejected_before_deleting_runtime(self):
        self.config({})
        self.fixture('app/asar/security-asar/lib/rsa-pub.pem',
                     b'-----BEGIN PRIVATE KEY-----\nsecret\n-----END PRIVATE KEY-----')
        sentinel = self.fixture('out/thunder_2025/keep')
        errors = io.StringIO()
        with mock.patch.object(sys, 'argv', self.sync_arguments()), \
                contextlib.redirect_stderr(errors):
            self.assertEqual(sync.main(), 1)
        self.assertNotIn('secret', errors.getvalue())
        self.assertEqual(sentinel.read_bytes(), b'fixture')

    def test_standard_conversion_requires_explicit_option(self):
        self.config({'plain': {}})
        self.fixture('plugins/plain.asar', standard_asar())
        args = self.sync_arguments() + ['--asar-format', 'standard']
        with mock.patch.object(sys, 'argv', args), \
                mock.patch.object(sync, 'normalize_plugin_asars', return_value=1) as normalize, \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(sync.main(), 0)
        normalize.assert_called_once()

    @unittest.skipUnless(os.environ.get('THUNDER_APP_ROOT') and shutil.which('node'),
                         'set THUNDER_APP_ROOT and provide node for real ASAR tools')
    def test_vendor_conversion_preserves_unpacked_and_external_companions(self):
        app = Path(os.environ['THUNDER_APP_ROOT']).resolve()
        resolver = app / 'asar/resolve-asar.js'
        node = shutil.which('node')
        script = r"""
const fs = require('node:fs');
const path = require('node:path');
const {loadAsar} = require(process.argv[1]);
const root = process.argv[2];
(async () => {
  const source = path.join(root, 'source');
  fs.mkdirSync(path.join(source, 'native'), {recursive: true});
  fs.mkdirSync(path.join(source, 'payload'), {recursive: true});
  fs.writeFileSync(path.join(source, 'index.js'), 'module.exports = 42;');
  fs.writeFileSync(path.join(source, 'native', '[helper].exe'), 'external executable');
  fs.writeFileSync(path.join(source, 'packed.exe'), 'intentionally packed');
  fs.writeFileSync(path.join(source, 'payload', 'data.bin'), 'external directory');
  const vendor = path.join(root, 'plugins', 'vendor.asar');
  const originalLog = console.log;
  let indexText;
  console.log = () => {};
  try {
    await loadAsar('security').createPackageWithOptions(source, vendor, {
      unpack: '**/[[]helper[]].exe', unpackDir: 'payload',
    });
    indexText = loadAsar('security').extractFile(vendor, 'index.js').toString();
  } finally {
    console.log = originalLog;
  }
  fs.writeFileSync(path.join(vendor + '.unpacked', 'companion.exe'), 'unlisted companion');
  await loadAsar('standard').createPackage(source, path.join(root, 'plugins', 'standard.asar'));
  console.log(JSON.stringify({indexText}));
})().catch(error => {console.error(error); process.exitCode = 1;});
"""
        fixture = subprocess.run([node, '-e', script, str(resolver), str(self.root)],
                                 check=True, capture_output=True, text=True)
        original_index = json.loads(fixture.stdout)['indexText']
        standard = self.plugins / 'standard.asar'
        standard_before = standard.read_bytes()
        # Resolver and loadAsar diagnostics must also stay out of sync output.
        quiet_app = self.root / 'quiet-app'
        self.fixture('quiet-app/asar/resolve-asar.js', (
            'console.warn("vendor resolver diagnostic");\n'
            f'const real = require({json.dumps(str(resolver))});\n'
            'exports.loadAsar = format => {\n'
            '  console.error("vendor loader diagnostic");\n'
            '  return real.loadAsar(format);\n'
            '};\n').encode())
        run = subprocess.run
        outputs = []

        def capture(*args, **kwargs):
            result = run(*args, **kwargs, capture_output=True, text=True)
            outputs.append(result)
            return result

        with mock.patch.object(sync.subprocess, 'run', side_effect=capture):
            self.assertEqual(sync.normalize_plugin_asars(self.plugins, quiet_app), 2)
        self.assertEqual(outputs[0].stderr, '')
        self.assertEqual(outputs[0].stdout.strip(),
                         f'plugin ASAR normalized: {self.plugins / "vendor.asar"}')
        self.assertEqual(standard.read_bytes(), standard_before)
        script = r"""
const path = require('node:path');
const standard = require(process.argv[1]).loadAsar('standard');
const archive = process.argv[2];
const result = {};
for (const file of ['index.js', 'packed.exe', 'native/[helper].exe', 'payload', 'payload/data.bin']) {
  const relative = file.split('/').join(path.sep);
  const info = standard.statFile(archive, relative, false);
  result[file] = {unpacked: Boolean(info.unpacked)};
  if (!info.files) result[file].text = standard.extractFile(archive, relative).toString();
}
console.log(JSON.stringify(result));
"""
        result = subprocess.run([node, '-e', script, str(resolver),
                                 str(self.plugins / 'vendor.asar')],
                                check=True, capture_output=True, text=True)
        entries = json.loads(result.stdout)
        self.assertEqual(entries['index.js'], {'unpacked': False, 'text': original_index})
        self.assertEqual(entries['packed.exe'], {'unpacked': False, 'text': 'intentionally packed'})
        self.assertEqual(entries['native/[helper].exe'],
                         {'unpacked': True, 'text': 'external executable'})
        self.assertEqual(entries['payload'], {'unpacked': True})
        self.assertEqual(entries['payload/data.bin'],
                         {'unpacked': True, 'text': 'external directory'})
        companion = self.plugins / 'vendor.asar.unpacked/companion.exe'
        self.assertEqual(companion.read_bytes(), b'unlisted companion')


if __name__ == '__main__':
    unittest.main()
