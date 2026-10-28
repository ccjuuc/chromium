#!/usr/bin/env python3
# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Verify the GN stage with small opaque files and relocatable framework links."""

import hashlib
import io
import json
import lzma
import os
from pathlib import Path
import plistlib
import tempfile
import tarfile
import unittest
import zipfile
from unittest import mock

import archive_thunder_macos as archiving
import configure_thunder_macos_resources as configuration
import stage_thunder_macos_resources as staging


def inventory(root):
    result = {}
    for directory, directories, files in os.walk(root, followlinks=False):
        for name in directories + files:
            path = Path(directory) / name
            key = str(path.relative_to(root))
            result[key] = (('link', os.readlink(path)) if path.is_symlink() else
                           ('directory',) if path.is_dir() else
                           ('file', path.read_bytes()))
    return result


class StageThunderMacResourcesTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='thunder-stage-test-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.source = self.root / 'source'
        self.destination = self.root / 'gen/thunder_macos'
        self.bundle = self.source / staging.BUNDLE
        (self.bundle / 'Contents/Resources/app').mkdir(parents=True)
        (self.bundle / 'Contents/MacOS').mkdir()
        (self.bundle / 'Contents/Info.plist').write_bytes(plistlib.dumps({
            'CFBundlePackageType': 'BNDL', 'CFBundleIdentifier': 'fixture.thunder'}))
        (self.bundle / 'Contents/Resources/app/out.asar').write_bytes(b'archive\x00\xff')
        (self.bundle / 'Contents/Resources/xenon-native-player.json').write_text(
            json.dumps(staging.ADAPTER))
        self.descriptor = {'application': './Thunder.app', 'main': 'out.asar/main.js',
                           'name': 'Thunder', 'privilegedSchemes': [{'scheme': 'fixture'}]}
        (self.source / staging.DESCRIPTOR).write_text(json.dumps(self.descriptor))
        (self.source / 'build.log').write_text('must not enter the browser')
        (self.source / 'build-report.json').write_text('{"large":"report"}')
        self.archive = self.root / 'thunder-2025-mac.tar.xz'
        self.checksum = self.root / 'thunder-2025-mac.sha256'

    def stage(self):
        return staging.stage_resources(self.source, self.destination)

    def write_checksum(self):
        digest = hashlib.sha256(self.archive.read_bytes()).hexdigest()
        self.checksum.write_text(f'{digest}  {self.archive.name}\n')

    def make_archive(self, additional=()):
        with tarfile.open(self.archive, 'w:xz', format=tarfile.PAX_FORMAT) as package:
            package.add(self.bundle, arcname=staging.BUNDLE)
            package.add(self.source / staging.DESCRIPTOR, arcname=staging.DESCRIPTOR)
            for member, contents in additional:
                package.addfile(member, io.BytesIO(contents) if contents is not None else None)
        self.write_checksum()

    def stage_archive(self):
        return staging.stage_archive(self.archive, self.checksum, self.destination)

    def test_copies_only_payload_and_preserves_descriptor_without_changing_source(self):
        before = inventory(self.source)
        self.stage()
        self.assertEqual(set(path.name for path in self.destination.iterdir()),
                         {staging.BUNDLE, staging.DESCRIPTOR})
        self.assertEqual(inventory(self.destination / staging.BUNDLE), inventory(self.bundle))
        descriptor = json.loads((self.destination / staging.DESCRIPTOR).read_text())
        self.assertEqual(descriptor,
                         {**self.descriptor, 'application': staging.HOST_APPLICATION})
        self.assertNotIn('executable', descriptor)
        self.assertTrue((self.destination / staging.BUNDLE / 'Contents/MacOS').is_dir())
        self.assertEqual(inventory(self.source), before)

    def test_new_stage_removes_stale_files_and_changes_directory_input_timestamp(self):
        old = self.bundle / 'Contents/Resources/app/old/deep/old.bin'
        old.parent.mkdir(parents=True)
        old.write_bytes(b'obsolete')
        self.stage()
        staged_bundle = self.destination / staging.BUNDLE
        os.utime(staged_bundle, ns=(1, 1))
        old.unlink()
        old.parent.rmdir()
        old.parent.parent.rmdir()
        new = self.bundle / 'Contents/Resources/app/new.bin'
        new.write_bytes(b'new')
        self.stage()
        self.assertEqual(inventory(staged_bundle), inventory(self.bundle))
        self.assertFalse((staged_bundle / 'Contents/Resources/app/old').exists())
        self.assertGreater(staged_bundle.stat().st_mtime_ns, 1)

    def test_depfile_tracks_all_bundle_files_and_directories_but_not_reports(self):
        (self.bundle / 'Contents/Resources/app/empty/nested').mkdir(parents=True)
        stamp, depfile = self.root / 'gen/staged.stamp', self.root / 'gen/staged.d'
        args = ['--source', str(self.source), '--destination', str(self.destination),
                '--stamp', str(stamp), '--depfile', str(depfile)]
        self.assertEqual(staging.main(args), 0)
        recorded = depfile.read_text()
        for path in [self.bundle, self.source / staging.DESCRIPTOR,
                     self.bundle / 'Contents/Resources/app/out.asar',
                     self.bundle / 'Contents/Resources/app/empty',
                     self.bundle / 'Contents/Resources/app/empty/nested']:
            self.assertIn(os.path.relpath(path), recorded)
        self.assertNotIn('build.log', recorded)
        self.assertNotIn('build-report.json', recorded)
        self.assertTrue(stamp.is_file())
        added = self.bundle / 'Contents/Resources/app/empty/nested/new.bin'
        added.write_bytes(b'added')
        self.assertEqual(staging.main(args), 0)
        self.assertIn(os.path.relpath(added), depfile.read_text())

    def test_preserves_framework_relative_aliases_inside_staged_payload(self):
        framework = self.bundle / 'Contents/Frameworks/Fixture.framework'
        (framework / 'Versions/A').mkdir(parents=True)
        (framework / 'Versions/A/Fixture').write_bytes(b'framework')
        (framework / 'Versions/Current').symlink_to('A')
        (framework / 'Fixture').symlink_to('Versions/Current/Fixture')
        inputs = self.stage()
        self.assertIn(framework / 'Versions/Current', inputs)
        installed = self.destination / staging.BUNDLE / framework.relative_to(self.bundle)
        self.assertEqual(os.readlink(installed / 'Fixture'), 'Versions/Current/Fixture')
        self.assertTrue((installed / 'Fixture').resolve().is_relative_to(self.destination))
        self.assertEqual((installed / 'Fixture').read_bytes(), b'framework')

    def test_rejects_non_relocatable_links_before_replacing_previous_stage(self):
        self.stage()
        before = inventory(self.destination)
        outside = self.root / 'outside'
        outside.write_bytes(b'outside')
        link = self.bundle / 'Contents/Resources/alias'
        for target in (outside, self.bundle / 'Contents/Resources/app/out.asar',
                       '../../../../outside'):
            with self.subTest(target=target):
                link.symlink_to(target)
                try:
                    with self.assertRaises((ValueError, FileNotFoundError)):
                        self.stage()
                    self.assertEqual(inventory(self.destination), before)
                finally:
                    link.unlink()

    def test_rejects_invalid_resource_identity_or_descriptor_without_deleting_output(self):
        self.stage()
        before = inventory(self.destination)
        info = self.bundle / 'Contents/Info.plist'
        original = info.read_bytes()
        for identity in ([], {'CFBundlePackageType': 'APPL'},
                         {'CFBundlePackageType': 'BNDL', 'CFBundleExecutable': 'Thunder'}):
            with self.subTest(identity=identity):
                info.write_bytes(plistlib.dumps(identity))
                with self.assertRaisesRegex(ValueError, 'resource BNDL'):
                    self.stage()
                self.assertEqual(inventory(self.destination), before)
        info.write_bytes(original)
        for config in ({**self.descriptor, 'application': '../outside'},
                       {**self.descriptor, 'executable': './Thunder'},
                       {**self.descriptor, 'main': '../outside.js'}):
            with self.subTest(config=config):
                (self.source / staging.DESCRIPTOR).write_text(json.dumps(config))
                with self.assertRaisesRegex(ValueError, 'exported resource descriptor'):
                    self.stage()
                self.assertEqual(inventory(self.destination), before)

    def test_missing_adapter_and_electron_runtime_are_rejected(self):
        declaration = self.bundle / 'Contents/Resources/xenon-native-player.json'
        declaration.unlink()
        with self.assertRaisesRegex(ValueError, 'regular file'):
            self.stage()
        self.assertFalse(self.destination.exists())
        declaration.write_text(json.dumps(staging.ADAPTER))
        (self.bundle / 'Contents/Frameworks/Electron Framework.framework').mkdir(parents=True)
        with self.assertRaisesRegex(ValueError, 'Electron runtime'):
            self.stage()
        self.assertFalse(self.destination.exists())

    def test_rejects_overlapping_and_linked_output_roots(self):
        before = inventory(self.source)
        for destination in (self.source, self.source / 'nested', self.root):
            with self.subTest(destination=destination):
                with self.assertRaisesRegex(ValueError, 'must not overlap'):
                    staging.stage_resources(self.source, destination)
                self.assertEqual(inventory(self.source), before)
        outside = self.root / 'outside'
        outside.mkdir()
        self.destination.parent.mkdir()
        self.destination.symlink_to(outside, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, 'Linked staging destination'):
            self.stage()
        self.assertEqual(list(outside.iterdir()), [])

    def test_copy_failure_keeps_previous_stage(self):
        self.stage()
        before = inventory(self.destination)
        with mock.patch.object(staging, '_copy_bundle', side_effect=OSError('copy failed')):
            with self.assertRaisesRegex(OSError, 'copy failed'):
                self.stage()
        self.assertEqual(inventory(self.destination), before)
        self.assertFalse(list(self.destination.parent.glob('.thunder-resources-*')))

    def test_install_failure_restores_previous_stage(self):
        self.stage()
        before = inventory(self.destination)
        rename = Path.rename

        def fail_install(path, target):
            if path.name == 'payload':
                raise OSError('install failed')
            return rename(path, target)

        with mock.patch.object(Path, 'rename', fail_install):
            with self.assertRaisesRegex(OSError, 'install failed'):
                self.stage()
        self.assertEqual(inventory(self.destination), before)
        self.assertFalse(list(self.destination.parent.glob('.thunder-resources-*')))

    def test_disabled_configuration_removes_only_reserved_host_entries(self):
        host = self.root / 'Browser.app'
        payload = host / 'Contents/Resources/xenon-apps/Thunder.app'
        payload.mkdir(parents=True)
        (payload / 'stale').write_bytes(b'previous app')
        descriptor = payload.parent / staging.DESCRIPTOR
        descriptor.write_text('{}')
        unrelated = host / 'Contents/MacOS/Browser'
        unrelated.parent.mkdir()
        unrelated.write_bytes(b'host executable')
        sibling = payload.with_name('Other.app')
        sibling.mkdir()
        (sibling / 'keep').write_bytes(b'other application')
        before = inventory(host)
        configuration.configure_bundle(host, True)
        self.assertEqual(inventory(host), before)
        configuration.configure_bundle(host, False)
        self.assertFalse(payload.exists())
        self.assertFalse(descriptor.exists())
        self.assertEqual(unrelated.read_bytes(), b'host executable')
        self.assertEqual((sibling / 'keep').read_bytes(), b'other application')
        configuration.configure_bundle(host, False)

    def test_enabled_configuration_removes_only_legacy_executable_directory_descriptor(self):
        host = self.root / 'Browser.app'
        legacy = host / 'Contents/MacOS' / staging.DESCRIPTOR
        legacy.parent.mkdir(parents=True)
        legacy.write_text('legacy')
        descriptor = host / 'Contents/Resources/xenon-apps' / staging.DESCRIPTOR
        descriptor.parent.mkdir(parents=True)
        descriptor.write_text('current')
        payload = descriptor.parent / 'Thunder.app'
        payload.mkdir()
        (payload / 'keep').write_bytes(b'current payload')
        configuration.configure_bundle(host, True)
        self.assertFalse(legacy.exists())
        self.assertEqual(descriptor.read_text(), 'current')
        self.assertEqual((payload / 'keep').read_bytes(), b'current payload')
        legacy.write_text('legacy')
        configuration.configure_bundle(host, False)
        self.assertFalse(legacy.exists())
        self.assertFalse(descriptor.exists())
        self.assertFalse(payload.exists())

    def test_disabled_configuration_does_not_create_missing_bundle(self):
        host = self.root / 'Missing.app'
        configuration.configure_bundle(host, False)
        self.assertFalse(host.exists())

    def test_disabled_configuration_rejects_linked_ancestors_before_removal(self):
        host = self.root / 'Browser.app'
        (host / 'Contents/MacOS').mkdir(parents=True)
        descriptor = host / 'Contents/MacOS' / staging.DESCRIPTOR
        descriptor.write_text('keep')
        outside = self.root / 'outside'
        payload = outside / 'xenon-apps/Thunder.app'
        payload.mkdir(parents=True)
        (payload / 'keep').write_bytes(b'outside')
        (host / 'Contents/Resources').symlink_to(outside, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, 'Linked bundle ancestor'):
            configuration.configure_bundle(host, False)
        self.assertEqual(descriptor.read_text(), 'keep')
        self.assertEqual((payload / 'keep').read_bytes(), b'outside')

    def test_archive_build_does_not_depend_on_the_unpacked_source(self):
        framework = self.bundle / 'Contents/Frameworks/Fixture.framework'
        (framework / 'Versions/A').mkdir(parents=True)
        binary = framework / 'Versions/A/Fixture'
        binary.write_bytes(b'signed framework bytes')
        binary.chmod(0o755)
        (framework / 'Versions/Current').symlink_to('A')
        (framework / 'Fixture').symlink_to('Versions/Current/Fixture')
        # Exercise PAX long names and Unicode using the production writer.
        long_name = self.bundle / 'Contents/Resources/app' / ('compatibility-' * 8 + '字幕')
        long_name.mkdir()
        (long_name / 'fixture.txt').write_bytes(b'portable PAX contents')
        expected = inventory(self.bundle)
        archiving.create_archive(self.source, self.archive, self.checksum)
        self.source.rename(self.root / 'unpacked-not-an-input')
        self.assertEqual(self.stage_archive(), [self.archive, self.checksum])
        installed = self.destination / staging.BUNDLE
        self.assertEqual(inventory(installed), expected)
        self.assertEqual((installed / binary.relative_to(self.bundle)).stat().st_mode & 0o777,
                         0o755)
        self.assertEqual(os.readlink(installed / framework.relative_to(self.bundle) / 'Fixture'),
                         'Versions/Current/Fixture')

    def test_archive_depfile_contains_only_archive_and_checksum(self):
        self.make_archive()
        stamp, depfile = self.root / 'gen/archive.stamp', self.root / 'gen/archive.d'
        self.assertEqual(staging.main([
            '--archive', str(self.archive), '--checksum', str(self.checksum),
            '--destination', str(self.destination), '--stamp', str(stamp),
            '--depfile', str(depfile)]), 0)
        text = depfile.read_text()
        self.assertIn(os.path.relpath(self.archive), text)
        self.assertIn(os.path.relpath(self.checksum), text)
        self.assertNotIn(os.path.relpath(self.source), text)
        self.assertNotIn('thunder-archive-', text)

    def test_archive_rejects_checksum_mismatch_and_filename_before_replacing_stage(self):
        self.stage()
        before = inventory(self.destination)
        self.make_archive()
        digest = hashlib.sha256(self.archive.read_bytes()).hexdigest()
        for checksum in ('0' * 64 + '  ' + self.archive.name + '\n',
                         digest + '  another.tar.xz\n', digest + ' ' + self.archive.name + '\n'):
            with self.subTest(checksum=checksum):
                self.checksum.write_text(checksum)
                with self.assertRaisesRegex(ValueError, 'checksum|SHA256'):
                    self.stage_archive()
                self.assertEqual(inventory(self.destination), before)

    def test_archive_rejects_absolute_parent_paths_unexpected_roots_and_duplicates(self):
        self.stage()
        before = inventory(self.destination)
        for name in ('/outside', '../outside', 'Thunder.app/../../outside',
                     'Thunder.app/Contents/../outside', 'build.log',
                     'Thunder.app/Contents/Resources/app/out.asar',
                     'Thunder.app/contents/extra', 'Thunder.app/Contents\\escape'):
            with self.subTest(name=name):
                member = tarfile.TarInfo(name)
                member.size = 1
                self.make_archive([(member, b'x')])
                with self.assertRaises(ValueError):
                    self.stage_archive()
                self.assertEqual(inventory(self.destination), before)
                self.assertFalse((self.root / 'outside').exists())

    def test_archive_rejects_hardlinks_devices_fifos_and_special_permissions(self):
        for kind in (tarfile.LNKTYPE, tarfile.CHRTYPE, tarfile.BLKTYPE, tarfile.FIFOTYPE,
                     tarfile.GNUTYPE_SPARSE):
            with self.subTest(kind=kind):
                member = tarfile.TarInfo('Thunder.app/Contents/unsafe')
                member.type = kind
                member.linkname = 'Thunder.app/Contents/Info.plist'
                self.make_archive([(member, None)])
                with self.assertRaises(ValueError):
                    self.stage_archive()
        member = tarfile.TarInfo('Thunder.app/Contents/setuid')
        member.mode = 0o4755
        self.make_archive([(member, b'')])
        with self.assertRaisesRegex(ValueError, 'Special archive permissions'):
            self.stage_archive()
        self.assertFalse(self.destination.exists())

    def test_archive_rejects_symlink_parent_in_either_member_order(self):
        link = tarfile.TarInfo('Thunder.app/Contents/Resources/alias')
        link.type = tarfile.SYMTYPE
        link.linkname = 'app'
        child = tarfile.TarInfo('Thunder.app/Contents/Resources/alias/new.bin')
        child.size = 1
        for members in ([(link, None), (child, b'x')], [(child, b'x'), (link, None)]):
            with self.subTest(first=members[0][0].name):
                self.make_archive(members)
                with self.assertRaisesRegex(ValueError, 'symlink'):
                    self.stage_archive()
        self.assertFalse(self.destination.exists())

    def test_archive_rejects_escaping_absolute_and_cyclic_symlinks(self):
        for target in ('/tmp/outside', '../../../outside', 'missing', 'alias'):
            with self.subTest(target=target):
                member = tarfile.TarInfo('Thunder.app/Contents/Resources/alias')
                member.type = tarfile.SYMTYPE
                member.linkname = target
                self.make_archive([(member, None)])
                with self.assertRaises(ValueError):
                    self.stage_archive()
        self.assertFalse(self.destination.exists())

    def test_archive_limits_compressed_size_member_count_and_expanded_data(self):
        self.make_archive()
        for constant, maximum in (('MAX_ARCHIVE_BYTES', 1), ('MAX_FILE_BYTES', 1),
                                  ('MAX_MEMBERS', 1), ('MAX_EXPANDED_BYTES', 100)):
            with self.subTest(limit=constant):
                with mock.patch.object(staging, constant, maximum):
                    with self.assertRaisesRegex(ValueError, 'limit'):
                        self.stage_archive()
        self.assertFalse(self.destination.exists())

    def test_archive_limits_pax_metadata_before_reading_its_payload(self):
        for kind in (tarfile.XHDTYPE, tarfile.SOLARIS_XHDTYPE):
            with self.subTest(kind=kind):
                member = tarfile.TarInfo('metadata')
                member.type = kind
                member.size = staging.MAX_METADATA_BYTES + 1
                # No metadata body follows: its header must be rejected before
                # tarfile allocates or attempts to read the declared payload.
                self.archive.write_bytes(lzma.compress(
                    member.tobuf(format=tarfile.USTAR_FORMAT)))
                self.write_checksum()
                with self.assertRaisesRegex(ValueError, 'size limit'):
                    self.stage_archive()
        self.assertFalse(self.destination.exists())

    def test_archive_rejects_pax_sparse_formats_before_parsing_their_maps(self):
        self.stage()
        before = inventory(self.destination)
        variants = (
            {'GNU.sparse.size': '1', 'GNU.sparse.offset': 'invalid-offset',
             'GNU.sparse.numbytes': '1'},
            {'GNU.sparse.map': '0,invalid-size'},
            {'GNU.sparse.major': '1', 'GNU.sparse.minor': '0'},
        )
        for headers in variants:
            with self.subTest(headers=headers):
                member = tarfile.TarInfo('Thunder.app/Contents/Resources/sparse')
                member.pax_headers = headers
                # Version 1.0 reads its map from the file body. All versions
                # contain malformed integers to prove rejection occurs before
                # either metadata or body is interpreted as a sparse map.
                body = b'invalid-field-count\n'.ljust(tarfile.BLOCKSIZE, b'\0')
                member.size = len(body)
                self.make_archive([(member, body)])
                with self.assertRaisesRegex(ValueError, 'Sparse archive entries are unsupported'):
                    self.stage_archive()
                self.assertEqual(inventory(self.destination), before)

    def test_zip_disguised_as_tar_xz_is_rejected(self):
        self.stage()
        before = inventory(self.destination)
        with zipfile.ZipFile(self.archive, 'w') as package:
            package.writestr('../escape', b'unsafe')
        self.write_checksum()
        with self.assertRaises((tarfile.TarError, lzma.LZMAError)):
            self.stage_archive()
        self.assertEqual(inventory(self.destination), before)

    def test_archive_replacement_removes_stale_files(self):
        old = self.bundle / 'Contents/Resources/app/old.bin'
        old.write_bytes(b'old')
        self.make_archive()
        self.stage_archive()
        old.unlink()
        new = self.bundle / 'Contents/Resources/app/new.bin'
        new.write_bytes(b'new')
        self.make_archive()
        self.stage_archive()
        self.assertFalse((self.destination / staging.BUNDLE / old.relative_to(self.bundle)).exists())
        self.assertEqual((self.destination / staging.BUNDLE / new.relative_to(self.bundle)).read_bytes(),
                         b'new')


if __name__ == '__main__':
    unittest.main()
