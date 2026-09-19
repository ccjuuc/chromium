#!/usr/bin/env python3
# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import copy_runtime_tree as copy_runtime


class CopyRuntimeTreeTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='copy-runtime-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        previous_cwd = Path.cwd()
        os.chdir(self.root)
        self.addCleanup(os.chdir, previous_cwd)
        self.source = self.root / 'source'
        self.target = self.root / 'target'
        self.source.mkdir()

    def write(self, root, relative, contents=b'contents'):
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(contents)
        return path

    def symlink(self, path, target, *, directory=False):
        try:
            path.symlink_to(target, target_is_directory=directory)
        except (OSError, NotImplementedError) as error:
            self.skipTest(f'symlinks unavailable: {error}')
        self.addCleanup(path.unlink)

    def test_preserves_original_names_opaque_bytes_and_empty_directories(self):
        files = {
            'XDASKernel.dll': b'original root library',
            'main/XDASKernel.dll': b'original nested library',
            'resources/app.asar': bytes(range(256)),
            'resources/app.asar.unpacked/native.node': b'native payload',
        }
        for name, contents in files.items():
            self.write(self.source, name, contents)
        (self.source / 'resources/empty/nested').mkdir(parents=True)
        copied, skipped, _ = copy_runtime.sync_tree(self.source, self.target)
        self.assertEqual((copied, skipped), (len(files), 0))
        for name, contents in files.items():
            self.assertEqual((self.target / name).read_bytes(), contents)
        self.assertTrue((self.target / 'resources/empty/nested').is_dir())

    def test_incremental_copy_removes_stale_files_and_directories(self):
        self.write(self.source, 'unchanged.dll')
        changed = self.write(self.source, 'changed.bin', b'before')
        stale = self.write(self.source, 'old/sub/stale.node')
        copy_runtime.sync_tree(self.source, self.target)
        unchanged_time = (self.target / 'unchanged.dll').stat().st_mtime_ns
        stale.unlink()
        stale.parent.rmdir()
        stale.parent.parent.rmdir()
        changed.write_bytes(b'after')
        self.write(self.source, 'new/sub/new.node')
        with mock.patch.object(copy_runtime.shutil, 'copy2',
                               wraps=copy_runtime.shutil.copy2) as copied_file:
            copied, skipped, _ = copy_runtime.sync_tree(self.source, self.target)
        self.assertEqual((copied, skipped), (2, 1))
        self.assertEqual(copied_file.call_count, 2)
        self.assertEqual((self.target / 'changed.bin').read_bytes(), b'after')
        self.assertFalse((self.target / 'old').exists())
        self.assertEqual((self.target / 'unchanged.dll').stat().st_mtime_ns,
                         unchanged_time)

    def test_equal_size_and_timestamp_do_not_hide_different_bytes(self):
        source = self.write(self.source, 'app.asar', b'original')
        copy_runtime.sync_tree(self.source, self.target)
        metadata = source.stat()
        source.write_bytes(b'replaced')
        os.utime(source, ns=(metadata.st_atime_ns, metadata.st_mtime_ns))
        self.assertEqual(copy_runtime.sync_tree(self.source, self.target)[:2],
                         (1, 0))
        self.assertEqual((self.target / 'app.asar').read_bytes(), b'replaced')

    def test_file_directory_type_changes_are_mirrored(self):
        self.write(self.source, 'new-directory/child')
        self.write(self.source, 'new-file', b'file')
        self.write(self.target, 'new-directory', b'old file')
        self.write(self.target, 'new-file/old-child', b'old child')
        copy_runtime.sync_tree(self.source, self.target)
        self.assertTrue((self.target / 'new-directory/child').is_file())
        self.assertEqual((self.target / 'new-file').read_bytes(), b'file')

    def test_original_file_and_directory_casing_is_preserved(self):
        self.write(self.source, 'Library/Native.dll')
        self.write(self.target, 'library/native.dll')
        copy_runtime.sync_tree(self.source, self.target)
        self.assertEqual([path.name for path in self.target.iterdir()], ['Library'])
        self.assertEqual([path.name for path in (self.target / 'Library').iterdir()],
                         ['Native.dll'])

    def test_depfile_tracks_source_root_and_all_new_nested_directories(self):
        (self.source / 'empty/nested').mkdir(parents=True)
        self.write(self.source, 'existing/file')
        _, _, first = copy_runtime.sync_tree(self.source, self.target)
        self.assertEqual(set(first), {
            'source', 'source/empty', 'source/empty/nested',
            'source/existing', 'source/existing/file'})
        self.write(self.source, 'empty/nested/added/deep/file')
        _, _, second = copy_runtime.sync_tree(self.source, self.target)
        self.assertTrue({'source/empty/nested/added',
                         'source/empty/nested/added/deep',
                         'source/empty/nested/added/deep/file'} <= set(second))
        with mock.patch('sys.argv', [str(Path(copy_runtime.__file__)),
                                    '--src', str(self.source), '--dest', str(self.target),
                                    '--stamp', 'copied.stamp', '--depfile', 'copied.d']):
            self.assertEqual(copy_runtime.main(), 0)
        depfile = (self.root / 'copied.d').read_text(encoding='utf-8')
        for relative in second:
            self.assertIn(relative, depfile)

    def test_overlapping_roots_are_rejected_before_deletion(self):
        original = self.write(self.source, 'keep', b'keep')
        for target in (self.source, self.source / 'nested', self.root):
            with self.subTest(target=target):
                with self.assertRaisesRegex(ValueError, 'must not overlap'):
                    copy_runtime.sync_tree(self.source, target)
                self.assertEqual(original.read_bytes(), b'keep')

    def test_source_link_is_rejected_without_touching_destination(self):
        outside = self.write(self.root, 'outside/file', b'outside')
        self.symlink(self.source / 'linked', outside.parent, directory=True)
        marker = self.write(self.target, 'keep', b'keep')
        with self.assertRaisesRegex(ValueError, 'links and reparse points'):
            copy_runtime.sync_tree(self.source, self.target)
        self.assertEqual(marker.read_bytes(), b'keep')
        self.assertEqual(outside.read_bytes(), b'outside')

    def test_destination_link_is_never_followed_or_removed(self):
        outside = self.write(self.root, 'outside/file', b'outside')
        self.target.mkdir()
        self.symlink(self.target / 'linked', outside.parent, directory=True)
        with self.assertRaisesRegex(ValueError, 'links and reparse points'):
            copy_runtime.sync_tree(self.source, self.target)
        self.assertEqual(outside.read_bytes(), b'outside')
        self.assertTrue((self.target / 'linked').is_symlink())

    def test_linked_destination_root_and_ancestor_are_rejected(self):
        outside = self.root / 'outside'
        outside.mkdir()
        self.symlink(self.target, outside, directory=True)
        for target in (self.target, self.target / 'nested'):
            with self.subTest(target=target):
                with self.assertRaisesRegex(ValueError, 'links and reparse points'):
                    copy_runtime.sync_tree(self.source, target)
        self.assertEqual(list(outside.iterdir()), [])

    def test_destination_hardlink_cannot_modify_external_file(self):
        outside = self.write(self.root, 'outside.bin', b'outside')
        self.target.mkdir()
        try:
            os.link(outside, self.target / 'file')
        except OSError as error:
            self.skipTest(f'hardlinks unavailable: {error}')
        self.write(self.source, 'file', b'new content')
        with self.assertRaisesRegex(ValueError, 'links and reparse points'):
            copy_runtime.sync_tree(self.source, self.target)
        self.assertEqual(outside.read_bytes(), b'outside')

    def test_destructive_path_guard_rejects_sibling(self):
        self.target.mkdir()
        outside = self.write(self.root, 'outside/file', b'outside')
        with self.assertRaisesRegex(ValueError, 'outside destination'):
            copy_runtime._checked_destination(self.target, outside)
        self.assertEqual(outside.read_bytes(), b'outside')


if __name__ == '__main__':
    unittest.main()
