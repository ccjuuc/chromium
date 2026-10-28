#!/usr/bin/env python3
# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Exercise payload deployment with small files, without running/signing apps."""

import json
import os
from pathlib import Path
import plistlib
import tempfile
import unittest
from unittest import mock

import deploy_thunder_macos as deployment


ADAPTER = {
    'schemaVersion': 1,
    'adapter': {'id': 'xunlei.mac-player', 'version': 1},
}


def inventory(directory):
    """Record file contents and link identity without following directory links."""
    result = {}
    for root, dirs, files in os.walk(directory, followlinks=False):
        for name in dirs + files:
            path = Path(root) / name
            relative = str(path.relative_to(directory))
            if path.is_symlink():
                result[relative] = ('link', os.readlink(path))
            elif path.is_dir():
                result[relative] = ('directory',)
            else:
                result[relative] = ('file', path.read_bytes())
    return result


class DeployThunderMacTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='thunder-deploy-test-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.host = self.make_bundle(self.root / 'xl153.app', payload=False)
        self.thunder = self.make_bundle(self.root / 'Thunder.app')
        self.backups = self.root / 'backups'
        self.destination = self.host / 'Contents/Resources/xenon-apps/Thunder.app'
        self.config = self.destination.parent / 'thunder_2025.xenon.json'
        self.legacy_config = self.host / 'Contents/MacOS/thunder_2025.xenon.json'
        self.legacy = self.destination.with_name('Thunder.previous.app')

    def make_bundle(self, path, *, payload=True, executable=None):
        (path / 'Contents/MacOS').mkdir(parents=True)
        (path / 'Contents/Resources').mkdir()
        info = {'CFBundleIdentifier': 'fixture.thunder' if payload else 'fixture.host',
                'CFBundlePackageType': 'BNDL' if payload else 'APPL',
                'CFBundleShortVersionString': '25.1.13'}
        if not payload:
            executable = 'xl153'
            (path / 'Contents/Resources/xenon-apps').mkdir()
        if executable:
            info['CFBundleExecutable'] = executable
            info['CFBundlePackageType'] = 'APPL'
            (path / 'Contents/MacOS' / executable).write_bytes(b'fixture executable')
        (path / 'Contents/Info.plist').write_bytes(plistlib.dumps(info))
        if payload:
            resources = path / 'Contents/Resources'
            (resources / 'app').mkdir()
            (resources / 'app/out.asar').write_bytes(b'opaque ASAR\x00\xff')
            (resources / 'xenon-native-player.json').write_text(json.dumps(ADAPTER))
            (path / 'Contents/MacOS/runtime-data.dat').write_bytes(b'working directory data')
        return path

    def deploy(self, source=None, host=None, backups=None):
        return deployment.deploy(host or self.host, source or self.thunder,
                                 backups or self.backups)

    def seed_existing_deployment(self):
        self.destination.mkdir()
        (self.destination / 'old-resource').write_bytes(b'previous payload')
        self.config.write_bytes(b'{"old":"descriptor"}\n')
        self.legacy_config.write_bytes(b'{"legacy":"descriptor"}\n')
        self.legacy.mkdir()
        (self.legacy / 'older-resource').write_bytes(b'legacy payload')

    def test_resource_bundle_uses_host_executable_and_keeps_mac_os_working_directory(self):
        source_before = inventory(self.thunder)
        host_executable = self.host / 'Contents/MacOS/xl153'
        executable_before = host_executable.read_bytes()

        self.assertEqual(self.deploy(), self.config)

        descriptor = json.loads(self.config.read_text())
        self.assertNotIn('executable', descriptor)
        self.assertEqual(descriptor['application'], './Thunder.app')
        self.assertEqual(descriptor['main'], 'out.asar/main.js')
        self.assertEqual((self.config.parent / descriptor['application']).resolve(),
                         self.destination)
        self.assertEqual(inventory(self.destination), source_before)
        self.assertEqual(inventory(self.thunder), source_before)
        self.assertTrue((self.destination / 'Contents/MacOS').is_dir())
        self.assertEqual((self.destination / 'Contents/MacOS/runtime-data.dat').read_bytes(),
                         b'working directory data')
        self.assertEqual(host_executable.read_bytes(), executable_before)
        self.assertFalse(self.backups.exists())

    def test_complete_electron_bundle_remains_supported_without_forcing_its_executable(self):
        source = self.make_bundle(self.root / 'ElectronThunder.app', executable='Thunder')
        self.deploy(source=source)
        self.assertNotIn('executable', json.loads(self.config.read_text()))
        self.assertEqual((self.destination / 'Contents/MacOS/Thunder').read_bytes(),
                         b'fixture executable')

    def test_requires_exact_native_adapter_declaration_before_changing_host(self):
        self.seed_existing_deployment()
        before = inventory(self.host)
        declaration = self.thunder / 'Contents/Resources/xenon-native-player.json'
        for value in (None, {}, {'schemaVersion': 2, 'adapter': ADAPTER['adapter']},
                      {'schemaVersion': 1, 'adapter': {'id': 'other', 'version': 1}},
                      {**ADAPTER, 'unexpected': True}, 'malformed json'):
            with self.subTest(value=value):
                if value is None:
                    declaration.unlink(missing_ok=True)
                else:
                    declaration.write_text(value if isinstance(value, str)
                                           else json.dumps(value))
                with self.assertRaises(ValueError):
                    self.deploy()
                self.assertEqual(inventory(self.host), before)
                self.assertFalse(self.backups.exists())

    def test_rejects_fake_resource_bundle_and_invalid_executable_identity(self):
        before = inventory(self.host)
        plist = self.thunder / 'Contents/Info.plist'
        original = plistlib.loads(plist.read_bytes())
        for properties in ({'CFBundlePackageType': 'APPL'},
                           {'CFBundlePackageType': 'BNDL', 'CFBundleExecutable': 'missing'},
                           {'CFBundleExecutable': '../../outside'},
                           {'CFBundleExecutable': '/outside'}):
            with self.subTest(properties=properties):
                plist.write_bytes(plistlib.dumps({**original, **properties}))
                with self.assertRaises(ValueError):
                    self.deploy()
                self.assertEqual(inventory(self.host), before)

    def test_requires_archive_and_runtime_working_directory(self):
        before = inventory(self.host)
        for relative in ('Contents/Resources/app/out.asar', 'Contents/MacOS'):
            with self.subTest(relative=relative):
                path = self.thunder / relative
                saved = path.with_name(path.name + '.saved')
                path.rename(saved)
                try:
                    with self.assertRaises(ValueError):
                        self.deploy()
                    self.assertEqual(inventory(self.host), before)
                finally:
                    saved.rename(path)

    def test_rejects_source_file_and_directory_links_outside_bundle(self):
        before = inventory(self.host)
        outside = self.root / 'outside'
        outside.mkdir()
        (outside / 'data').write_bytes(b'must not deploy')
        for target in (outside, outside / 'data'):
            with self.subTest(target=target):
                link = self.thunder / 'Contents/Resources/escape'
                link.symlink_to(target)
                try:
                    with self.assertRaises(ValueError):
                        self.deploy()
                    self.assertEqual(inventory(self.host), before)
                    self.assertFalse(self.backups.exists())
                finally:
                    link.unlink()

    def test_absolute_internal_link_cannot_escape_after_relocation(self):
        before = inventory(self.host)
        link = self.thunder / 'Contents/Resources/absolute-alias'
        link.symlink_to(self.thunder / 'Contents/Resources/app/out.asar')
        with self.assertRaises(ValueError):
            self.deploy()
        self.assertEqual(inventory(self.host), before)
        self.assertFalse(self.backups.exists())

    def test_relative_internal_framework_links_survive_relocation(self):
        framework = self.thunder / 'Contents/Frameworks/Fixture.framework'
        (framework / 'Versions/A').mkdir(parents=True)
        (framework / 'Versions/A/Fixture').write_bytes(b'framework binary fixture')
        (framework / 'Versions/Current').symlink_to('A')
        (framework / 'Fixture').symlink_to('Versions/Current/Fixture')
        self.deploy()
        installed = self.destination / framework.relative_to(self.thunder)
        self.assertTrue((installed / 'Fixture').is_symlink())
        self.assertEqual(os.readlink(installed / 'Fixture'), 'Versions/Current/Fixture')
        self.assertTrue((installed / 'Fixture').resolve().is_relative_to(self.destination))
        self.assertEqual((installed / 'Fixture').read_bytes(), b'framework binary fixture')

    def test_rejects_source_host_overlap(self):
        nested_source = self.make_bundle(self.host / 'Nested.app')
        nested_host = self.make_bundle(self.thunder / 'NestedHost.app', payload=False)
        before_host, before_source = inventory(self.host), inventory(self.thunder)
        for host, source in ((self.host, self.host), (self.host, nested_source),
                             (nested_host, self.thunder)):
            with self.subTest(host=host, source=source):
                with self.assertRaises(ValueError):
                    self.deploy(host=host, source=source)
                self.assertEqual(inventory(self.host), before_host)
                self.assertEqual(inventory(self.thunder), before_source)

    def test_rejects_backup_directory_inside_either_bundle(self):
        before_host, before_source = inventory(self.host), inventory(self.thunder)
        for backups in (self.host, self.host / 'backup', self.thunder,
                        self.thunder / 'backup'):
            with self.subTest(backups=backups):
                with self.assertRaises(ValueError):
                    self.deploy(backups=backups)
                self.assertEqual(inventory(self.host), before_host)
                self.assertEqual(inventory(self.thunder), before_source)

    def test_rejects_host_destination_ancestors_that_escape_bundle(self):
        for relative in ('Contents', 'Contents/Resources', 'Contents/MacOS',
                         'Contents/Resources/xenon-apps'):
            with self.subTest(relative=relative):
                host = self.make_bundle(self.root / (relative.replace('/', '-') + '.app'),
                                        payload=False)
                ancestor = host / relative
                outside = self.root / (relative.replace('/', '-') + '-outside')
                ancestor.rename(outside)
                ancestor.symlink_to(outside, target_is_directory=True)
                host_before, outside_before = inventory(host), inventory(outside)
                with self.assertRaises(ValueError):
                    self.deploy(host=host)
                self.assertEqual(inventory(host), host_before)
                self.assertEqual(inventory(outside), outside_before)

    def test_success_replaces_previous_payload_and_keeps_external_backup(self):
        self.seed_existing_deployment()
        old_payload, old_legacy = inventory(self.destination), inventory(self.legacy)
        old_config = self.config.read_bytes()
        self.deploy()
        backups = list(self.backups.iterdir())
        self.assertEqual(len(backups), 1)
        self.assertEqual(inventory(backups[0] / 'Thunder.app'), old_payload)
        self.assertEqual(inventory(backups[0] / 'Thunder.previous.app'), old_legacy)
        self.assertEqual((backups[0] / self.config.name).read_bytes(), old_config)
        self.assertEqual((backups[0] / 'legacy-macos-descriptor.json').read_bytes(),
                         b'{"legacy":"descriptor"}\n')
        self.assertFalse(self.legacy_config.exists())
        self.assertFalse(self.legacy.exists())
        self.assertFalse(list(self.root.glob('.thunder-stage-*')))

    def test_descriptor_install_failure_restores_all_previous_host_entries(self):
        self.seed_existing_deployment()
        before = inventory(self.host)
        replace = Path.replace

        def fail_descriptor(path, target):
            if path.name == 'descriptor.json':
                raise OSError('fixture descriptor installation failure')
            return replace(path, target)

        with mock.patch.object(Path, 'replace', fail_descriptor):
            with self.assertRaisesRegex(OSError, 'fixture descriptor'):
                self.deploy()
        self.assertEqual(inventory(self.host), before)
        self.assertFalse(list(self.root.glob('.thunder-stage-*')))
        self.assertEqual(len(list(self.backups.iterdir())), 1)

    def test_staging_copy_failure_does_not_modify_previous_host(self):
        self.seed_existing_deployment()
        before = inventory(self.host)
        copy = deployment._copy

        def fail_source(source, destination):
            if source == self.thunder:
                raise OSError('fixture staging copy failure')
            return copy(source, destination)

        with mock.patch.object(deployment, '_copy', side_effect=fail_source):
            with self.assertRaisesRegex(OSError, 'fixture staging'):
                self.deploy()
        self.assertEqual(inventory(self.host), before)
        self.assertFalse(list(self.root.glob('.thunder-stage-*')))


if __name__ == '__main__':
    unittest.main()
