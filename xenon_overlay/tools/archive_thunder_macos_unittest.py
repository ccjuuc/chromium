#!/usr/bin/env python3
# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import contextlib
import io
import json
import os
from pathlib import Path
import plistlib
import stat
import tarfile
import tempfile
import unittest
from unittest import mock

import archive_thunder_macos as archiver


class ThunderMacArchiveTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='thunder-archive-test-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.source = self.root / 'source'
        self.bundle = self.source / 'Thunder.app'
        (self.bundle / 'Contents/MacOS').mkdir(parents=True)
        (self.bundle / 'Contents/Resources/app').mkdir(parents=True)
        (self.bundle / 'Contents/Info.plist').write_bytes(plistlib.dumps({
            'CFBundlePackageType': 'BNDL', 'CFBundleIdentifier': 'test.thunder',
        }))
        (self.bundle / 'Contents/Resources/app/out.asar').write_bytes(b'opaque\x00asar')
        (self.bundle / 'Contents/Resources/xenon-native-player.json').write_text(
            json.dumps({'schemaVersion': 1,
                        'adapter': {'id': 'xunlei.mac-player', 'version': 1}}))
        (self.source / archiver.DESCRIPTOR).write_text(json.dumps({
            'application': './Thunder.app', 'main': 'out.asar/main.js',
        }))
        self.archive, self.checksum = archiver.archive_paths(self.root / 'thunder')

    def write(self, relative, contents=b'fixture'):
        path = self.source / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(contents)
        return path

    def test_archive_contains_only_runtime_roots_and_preserves_signed_framework(self):
        binary = self.write('Thunder.app/Contents/Frameworks/Player.framework/'
                            'Versions/A/Player', b'\xcf\xfa\xed\xfesignature fixture')
        binary.chmod(0o755)
        resources = self.write('Thunder.app/Contents/_CodeSignature/CodeResources',
                               b'opaque signed resource bytes')
        framework = binary.parent.parent.parent
        (framework / 'Versions/Current').symlink_to('A')
        (framework / 'Player').symlink_to('Versions/Current/Player')
        for extra in ('build.log', 'build-report.json', 'maps/main.js.map'):
            self.write(extra, b'not runtime input')

        result = archiver.archive_and_publish(self.source, self.archive, self.checksum)

        with tarfile.open(self.archive, 'r:xz') as archive:
            members = {entry.name: entry for entry in archive.getmembers()}
            self.assertEqual({name.split('/')[0] for name in members},
                             {'Thunder.app', archiver.DESCRIPTOR})
            path = binary.relative_to(self.source).as_posix()
            self.assertEqual(members[path].mode, 0o755)
            self.assertEqual(archive.extractfile(members[path]).read(), binary.read_bytes())
            path = resources.relative_to(self.source).as_posix()
            self.assertEqual(archive.extractfile(members[path]).read(), resources.read_bytes())
            link = members['Thunder.app/Contents/Frameworks/Player.framework/Player']
            self.assertTrue(link.issym())
            self.assertEqual(link.linkname, 'Versions/Current/Player')
            for member in members.values():
                self.assertEqual((member.uid, member.gid, member.uname,
                                  member.gname, member.mtime), (0, 0, '', '', 0))
        self.assertEqual(self.checksum.read_text(),
                         f'{archiver.sha256(self.archive)}  {self.archive.name}\n')
        self.assertEqual(result['sha256'], archiver.sha256(self.archive))
        self.assertEqual(result['entries'], len(members))

    def test_archive_is_reproducible_across_source_timestamps(self):
        archiver.archive_and_publish(self.source, self.archive, self.checksum)
        original = self.archive.read_bytes(), self.checksum.read_bytes()
        for path in self.source.rglob('*'):
            os.utime(path, (12345, 12345))
        archiver.archive_and_publish(self.source, self.archive, self.checksum)
        self.assertEqual((self.archive.read_bytes(), self.checksum.read_bytes()), original)
        self.assertEqual(list(self.root.glob('*.previous-*')), [])

    def test_archive_rejects_unsafe_links_and_special_files_before_writing(self):
        entry = self.bundle / 'Contents/Resources/unsafe'
        outside = self.root / 'outside'
        outside.write_bytes(b'user bytes')
        for kind in ('absolute-internal', 'escaping', 'dangling', 'hardlink', 'fifo', 'special-mode'):
            with self.subTest(kind=kind):
                if kind == 'absolute-internal':
                    entry.symlink_to(self.bundle / 'Contents/Info.plist')
                elif kind == 'escaping':
                    entry.symlink_to(os.path.relpath(outside, entry.parent))
                elif kind == 'dangling':
                    entry.symlink_to('missing')
                elif kind == 'hardlink':
                    os.link(outside, entry)
                elif kind == 'fifo':
                    os.mkfifo(entry)
                else:
                    entry.write_bytes(b'unsafe permissions')
                    entry.chmod(stat.S_ISVTX | 0o755)
                try:
                    with self.assertRaises((ValueError, OSError)):
                        archiver.create_archive(self.source, self.archive, self.checksum)
                    self.assertFalse(self.archive.exists())
                    self.assertFalse(self.checksum.exists())
                finally:
                    entry.unlink()
        self.assertEqual(outside.read_bytes(), b'user bytes')

    def test_archive_rejects_executable_bundle_and_invalid_descriptor(self):
        info = self.bundle / 'Contents/Info.plist'
        info.write_bytes(plistlib.dumps({'CFBundlePackageType': 'APPL',
                                        'CFBundleExecutable': 'Thunder'}))
        with self.assertRaisesRegex(ValueError, 'resource bundle'):
            archiver.create_archive(self.source, self.archive, self.checksum)
        info.write_bytes(plistlib.dumps({'CFBundlePackageType': 'BNDL'}))
        (self.source / archiver.DESCRIPTOR).write_text(json.dumps({
            'application': '/absolute/Thunder.app', 'main': 'out.asar/main.js'}))
        with self.assertRaisesRegex(ValueError, 'relative resource descriptor'):
            archiver.create_archive(self.source, self.archive, self.checksum)

    def test_linked_source_output_and_incomplete_old_pair_are_rejected(self):
        alias = self.root / 'alias'
        alias.symlink_to(self.source, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, 'real directory'):
            archiver.archive_and_publish(alias, self.archive, self.checksum)
        self.archive.symlink_to(self.source / archiver.DESCRIPTOR)
        with self.assertRaisesRegex(ValueError, 'Linked publication path'):
            archiver.archive_and_publish(self.source, self.archive, self.checksum)
        self.archive.unlink()
        self.archive.write_bytes(b'existing data')
        with self.assertRaisesRegex(ValueError, 'both be present'):
            archiver.archive_and_publish(self.source, self.archive, self.checksum)
        self.checksum.write_text('incorrect checksum')
        with self.assertRaisesRegex(ValueError, 'checksum does not match'):
            archiver.archive_and_publish(self.source, self.archive, self.checksum)
        self.assertEqual(self.archive.read_bytes(), b'existing data')
        self.assertEqual(self.checksum.read_text(), 'incorrect checksum')

    def test_payload_change_rejects_archive_and_preserves_old_pair(self):
        archiver.archive_and_publish(self.source, self.archive, self.checksum)
        original = self.archive.read_bytes(), self.checksum.read_bytes()
        verify = archiver._verify_archive

        def verify_then_change(path, entries):
            verify(path, entries)
            self.write('Thunder.app/Contents/Resources/app/out.asar', b'changed while packing')

        with mock.patch.object(archiver, '_verify_archive', verify_then_change):
            with self.assertRaisesRegex(ValueError, 'Payload changed'):
                archiver.archive_and_publish(self.source, self.archive, self.checksum)
        self.assertEqual((self.archive.read_bytes(), self.checksum.read_bytes()), original)
        self.assertEqual(list(self.root.glob('.thunder-archive-*')), [])

    def test_second_install_failure_rolls_back_both_outputs(self):
        archiver.archive_and_publish(self.source, self.archive, self.checksum)
        original = self.archive.read_bytes(), self.checksum.read_bytes()
        self.write('Thunder.app/Contents/Resources/app/out.asar', b'new app')
        rename = Path.rename

        def fail_checksum_install(path, target):
            if path.name == self.checksum.name and path.parent != self.root:
                raise OSError('checksum install failed')
            return rename(path, target)

        with mock.patch.object(Path, 'rename', fail_checksum_install):
            with self.assertRaisesRegex(OSError, 'checksum install failed'):
                archiver.archive_and_publish(self.source, self.archive, self.checksum)
        self.assertEqual((self.archive.read_bytes(), self.checksum.read_bytes()), original)
        self.assertEqual(list(self.root.glob('*.previous-*')), [])

    def test_failed_rollback_preserves_backup_and_staging(self):
        archiver.archive_and_publish(self.source, self.archive, self.checksum)
        previous = self.archive.read_bytes()
        self.write('Thunder.app/Contents/Resources/app/out.asar', b'new app')
        rename = Path.rename

        def fail_install_and_rollback(path, target):
            if (path.name == self.checksum.name and path.parent != self.root or
                    path == self.archive and target.parent != self.root):
                raise OSError('simulated storage failure')
            return rename(path, target)

        with mock.patch.object(Path, 'rename', fail_install_and_rollback):
            with contextlib.redirect_stderr(io.StringIO()) as errors:
                with self.assertRaises(archiver.PublicationRollbackError):
                    archiver.archive_and_publish(self.source, self.archive, self.checksum)
        backups = list(self.root.glob(self.archive.name + '.previous-*'))
        self.assertEqual(len(backups), 1)
        self.assertEqual(backups[0].read_bytes(), previous)
        staging = list(self.root.glob('.thunder-archive-*'))
        self.assertEqual(len(staging), 1)
        self.assertTrue((staging[0] / self.checksum.name).is_file())
        self.assertIn(str(staging[0]), errors.getvalue())

    def test_create_collision_does_not_remove_someone_elses_file(self):
        original_open = Path.open

        def collide(path, mode='r', *args, **kwargs):
            if path == self.archive and mode == 'xb':
                with original_open(path, 'wb') as stream:
                    stream.write(b'created by another publisher')
            return original_open(path, mode, *args, **kwargs)

        with mock.patch.object(Path, 'open', collide):
            with self.assertRaises(FileExistsError):
                archiver.create_archive(self.source, self.archive, self.checksum)
        self.assertEqual(self.archive.read_bytes(), b'created by another publisher')


if __name__ == '__main__':
    unittest.main()
