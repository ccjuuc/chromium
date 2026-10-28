#!/usr/bin/env python3
# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import contextlib
import difflib
import hashlib
import io
import json
import os
from pathlib import Path
import plistlib
import shutil
import sys
import tempfile
import unittest
from unittest import mock

import build_thunder_macos as builder


class ThunderMacBuildTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='thunder-build-test-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.source = self.root / 'source'
        self.source.mkdir()
        self.workspace = self.root / 'workspace'
        self.output = self.root / 'published'

    def write(self, root, relative, contents=b'original\n'):
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(contents)
        return path

    def symlink(self, path, target, *, directory=False):
        path.parent.mkdir(parents=True, exist_ok=True)
        try:
            path.symlink_to(target, target_is_directory=directory)
        except (OSError, NotImplementedError) as error:
            self.skipTest(f'symlinks unavailable: {error}')

    def make_manifest(self, name='native', repository='.', files=None):
        files = files or {'src/a.cc': (b'before a\n', b'after a\n'),
                          'src/b.cc': (b'before b\n', b'after b\n')}
        directory = self.root / 'support' / name
        directory.mkdir(parents=True)
        patch_file = directory / (name + '.patch')
        patch_text = ''
        file_entries = []
        for relative, (before, after) in files.items():
            self.write(self.source / repository, relative, before)
            patch_text += ''.join(difflib.unified_diff(
                before.decode().splitlines(keepends=True),
                after.decode().splitlines(keepends=True),
                fromfile='a/' + relative, tofile='b/' + relative))
            file_entries.append({
                'path': relative,
                'before_sha256': hashlib.sha256(before).hexdigest(),
                'after_sha256': hashlib.sha256(after).hexdigest(),
            })
        patch_file.write_text(patch_text, encoding='utf-8')
        manifest = directory / 'compatibility.json'
        builder.write_json(manifest, {
            'schema_version': 1,
            'patches': [{
                'path': patch_file.name,
                'repository': repository,
                'base_revision': 'a' * 40,
                'sha256': builder.sha256(patch_file),
                'files': file_entries,
            }],
        })
        return manifest

    def owned_output(self):
        artifact = self.write(self.output, 'Thunder.app/Contents/keep', b'old app')
        builder.write_json(self.output / 'build-report.json', {
            'builder': 'xenon-thunder-macos-v1', 'status': 'success',
        })
        return artifact

    def test_logged_subprocess_filters_split_environment_lines_and_keeps_failures(self):
        script = r'''
import os, sys
os.write(1, b'compile started\n')
for chunk in (b'gyp sill find Python execFile: op', b'ts = {"env":',
              b'{"SIGNING_TEST":"fixture-private-value"},"shell":false}\n'):
    os.write(2, chunk)
os.write(2, b'gyp ERR! compiler: header missing\n')
os.write(1, b'gyp sill find Python execFile: opts = {"shell":false}\n')
os.write(2, b'gyp sill find Python execFile: opts = {"env":{"LAST":"fixture-final-value"}}')
sys.exit(int(sys.argv[1]))
'''
        expected = ('compile started\n'
                    '[node-gyp diagnostic environment omitted]\n'
                    'gyp ERR! compiler: header missing\n'
                    'gyp sill find Python execFile: opts = {"shell":false}\n'
                    '[node-gyp diagnostic environment omitted]\n')
        for status in (0, 7):
            with self.subTest(status=status):
                filename = self.root / f'subprocess-{status}.log'
                with filename.open('w') as log:
                    if status:
                        with self.assertRaises(builder.BuildError) as failure:
                            builder.run([sys.executable, '-c', script, str(status)], log=log)
                        self.assertIn('（7）', str(failure.exception))
                        self.assertIn('build.log', str(failure.exception))
                        self.assertNotIn('fixture-private-value', str(failure.exception))
                    else:
                        self.assertEqual(builder.run(
                            [sys.executable, '-c', script, str(status)], log=log), '')
                self.assertEqual(filename.read_text(), expected)

    def test_upstream_cleanup_preserves_other_bytes_and_only_visits_known_logs(self):
        original = (b'compiler diagnostic\r\n'
                    b'gyp sill find Python execFile: opts = {"env":{"KEY":"fixture"}}\n'
                    b'non-UTF8 native diagnostic: \xff\n')
        log = self.write(self.workspace, 'dist-mac/logs/run/build.log', original)
        log.chmod(0o600)
        unknown = self.write(self.workspace, 'dist-mac/logs/run/other.log', original)
        source = self.write(self.source, 'app/input.log', original)
        builder.sanitize_upstream_build_logs(self.workspace)
        self.assertEqual(log.read_bytes(),
                         b'compiler diagnostic\r\n'
                         b'[node-gyp diagnostic environment omitted]\n'
                         b'non-UTF8 native diagnostic: \xff\n')
        self.assertEqual(log.stat().st_mode & 0o777, 0o600)
        self.assertEqual(unknown.read_bytes(), original)
        self.assertEqual(source.read_bytes(), original)
        modified = log.stat().st_mtime_ns
        builder.sanitize_upstream_build_logs(self.workspace)
        self.assertEqual(log.stat().st_mtime_ns, modified)

    def test_upstream_cleanup_failure_keeps_original_build_error_and_log(self):
        original = b'gyp sill find Python execFile: opts = {"env":{"KEY":"fixture"}}\n'
        log = self.write(self.workspace, 'dist-mac/logs/run/build.log', original)
        errors = io.StringIO()
        failure = builder.BuildError('original compilation failure')
        with mock.patch.object(Path, 'replace', side_effect=OSError('private diagnostic')):
            with contextlib.redirect_stderr(errors):
                with self.assertRaises(builder.BuildError) as caught:
                    try:
                        raise failure
                    finally:
                        builder.sanitize_upstream_build_logs(self.workspace)
        self.assertIs(caught.exception, failure)
        self.assertEqual(log.read_bytes(), original)
        self.assertEqual(list(log.parent.iterdir()), [log])
        self.assertIn(str(log), errors.getvalue())
        self.assertNotIn('private diagnostic', errors.getvalue())

    def test_upstream_cleanup_does_not_follow_log_symlinks(self):
        original = b'gyp sill find Python execFile: opts = {"env":{"KEY":"fixture"}}\n'
        outside = self.write(self.source, 'original.log', original)
        self.symlink(self.workspace / 'dist-mac/logs/run/build.log', outside)
        with contextlib.redirect_stderr(io.StringIO()) as errors:
            builder.sanitize_upstream_build_logs(self.workspace)
        self.assertEqual(outside.read_bytes(), original)
        self.assertIn('清理失败', errors.getvalue())

    def test_snapshot_has_independent_files_and_keeps_dependency_build_files(self):
        original = self.write(self.source, 'app/source.bin', b'\x00\xffopaque')
        executable = self.write(self.source, 'app/run.sh', b'#!/bin/sh\n')
        executable.chmod(0o755)
        self.write(self.source, 'app/node_modules/dependency/build/module.node')
        self.write(self.source, 'cppsrc/build/stale.node')
        self.write(self.source, 'app/dist/stale.js')
        self.write(self.source, '.git/config')
        (self.source / 'app/empty').mkdir()
        before = builder.fingerprint_source(self.source)

        builder.snapshot_source(self.source, self.workspace)

        copied = self.workspace / 'app/source.bin'
        self.assertEqual(copied.read_bytes(), original.read_bytes())
        self.assertNotEqual(copied.stat().st_ino, original.stat().st_ino)
        self.assertEqual((self.workspace / 'app/run.sh').stat().st_mode & 0o777,
                         0o755)
        self.assertTrue((self.workspace / 'app/empty').is_dir())
        self.assertTrue((self.workspace /
                         'app/node_modules/dependency/build/module.node').is_file())
        for excluded in ('cppsrc/build', 'app/dist', '.git'):
            self.assertFalse((self.workspace / excluded).exists())
        copied.write_bytes(b'compiler rewrote the snapshot')
        self.assertEqual(original.read_bytes(), b'\x00\xffopaque')
        self.assertEqual(builder.fingerprint_source(self.source), before)

    def test_snapshot_rebases_absolute_internal_links_before_build_writes(self):
        original = self.write(self.source, 'dependencies/pkg/file', b'upstream')
        self.symlink(self.source / 'app/node_modules/pkg', original.parent,
                     directory=True)
        self.symlink(self.source / 'absolute-file', original)
        self.symlink(self.source / 'relative-file', 'dependencies/pkg/file')
        before = builder.fingerprint_source(self.source)

        builder.snapshot_source(self.source, self.workspace)

        copied = self.workspace / 'dependencies/pkg/file'
        for relative in ('app/node_modules/pkg/file', 'absolute-file',
                         'relative-file'):
            self.assertEqual((self.workspace / relative).resolve(), copied)
        self.assertFalse(os.path.isabs(os.readlink(self.workspace / 'absolute-file')))
        (self.workspace / 'app/node_modules/pkg/file').write_bytes(b'built')
        self.assertEqual(original.read_bytes(), b'upstream')
        self.assertEqual(builder.fingerprint_source(self.source), before)

    def test_external_links_are_rejected_before_any_snapshot_files_are_written(self):
        outside = self.write(self.root, 'outside/file', b'outside')
        self.write(self.source, 'a-safe-file')
        link = self.source / 'z-escaping-link'
        for target in (outside, outside.parent):
            with self.subTest(target=target):
                self.symlink(link, target, directory=target.is_dir())
                try:
                    with self.assertRaises(builder.BuildError):
                        builder.snapshot_source(self.source, self.workspace)
                    self.assertFalse(self.workspace.exists())
                    self.assertEqual(outside.read_bytes(), b'outside')
                finally:
                    link.unlink()

    def test_fingerprint_detects_equal_size_changes_with_unchanged_timestamp(self):
        source_file = self.write(self.source, 'app/file', b'before')
        metadata = source_file.stat()
        before = builder.fingerprint_source(self.source)
        source_file.write_bytes(b'after!')
        os.utime(source_file, ns=(metadata.st_atime_ns, metadata.st_mtime_ns))
        self.assertNotEqual(builder.fingerprint_source(self.source), before)

    def test_location_validation_rejects_overlap_and_symlink_aliases(self):
        alias = self.root / 'source-alias'
        self.symlink(alias, self.source, directory=True)
        for output in (self.source, self.source / 'build', self.root,
                       alias / 'build'):
            with self.subTest(output=output), self.assertRaises(builder.BuildError):
                builder.validate_locations(self.source, output)
        for work in (self.source, self.source / 'scratch', self.output,
                     self.output / 'scratch'):
            with self.subTest(work=work), self.assertRaises(builder.BuildError):
                builder.validate_locations(self.source, self.output, work)
        builder.validate_locations(self.source, self.output, self.workspace)
        self.assertFalse(self.output.exists())
        self.assertFalse(self.workspace.exists())

    def test_manifest_rejects_modified_patch_and_escaping_patch_path(self):
        manifest = self.make_manifest()
        spec = builder.load_patch_specs([manifest])[0]
        spec['patch_file'].write_bytes(b'tampered patch')
        with self.assertRaises(builder.BuildError):
            builder.load_patch_specs([manifest])

        outside = self.write(self.root, 'outside.patch', b'outside patch')
        document = json.loads(manifest.read_text())
        document['patches'][0]['sha256'] = builder.sha256(outside)
        for relative in (str(outside), '../../outside.patch', 'linked.patch'):
            with self.subTest(relative=relative):
                document['patches'][0]['path'] = relative
                builder.write_json(manifest, document)
                if relative == 'linked.patch':
                    self.symlink(manifest.parent / relative, outside)
                with self.assertRaises(builder.BuildError):
                    builder.load_patch_specs([manifest])
        self.assertEqual(outside.read_bytes(), b'outside patch')

    def test_classification_rejects_wrong_version_missing_and_mixed_files(self):
        spec = builder.load_patch_specs([self.make_manifest()])[0]
        self.assertEqual(builder.classify_patch(self.source, spec), 'apply')
        second = self.source / 'src/b.cc'
        second.write_bytes(b'after b\n')
        with self.assertRaises(builder.BuildError):
            builder.classify_patch(self.source, spec)
        second.write_bytes(b'upstream changed\n')
        with self.assertRaises(builder.BuildError):
            builder.classify_patch(self.source, spec)
        second.unlink()
        with self.assertRaises(builder.BuildError):
            builder.classify_patch(self.source, spec)
        second.write_bytes(b'after b\n')
        (self.source / 'src/a.cc').write_bytes(b'after a\n')
        self.assertEqual(builder.classify_patch(self.source, spec), 'already-applied')

    def test_patch_set_preflights_every_repository_before_writing(self):
        first = self.make_manifest('main')
        second = self.make_manifest('player', 'cppsrc/player')
        specs = builder.load_patch_specs([first, second])
        builder.snapshot_source(self.source, self.workspace)
        (self.workspace / 'cppsrc/player/src/a.cc').write_bytes(b'unknown version\n')
        before = builder.fingerprint_source(self.workspace)
        with mock.patch.object(builder, 'run') as run:
            with self.assertRaises(builder.BuildError):
                builder.apply_patch_set(self.workspace, specs)
            run.assert_not_called()
        self.assertEqual(builder.fingerprint_source(self.workspace), before)

    @unittest.skipUnless(shutil.which('git'), 'git is required to apply patches')
    def test_patch_application_changes_only_snapshot_and_is_idempotent(self):
        manifests = [self.make_manifest('main'),
                     self.make_manifest('player', 'cppsrc/player')]
        specs = builder.load_patch_specs(manifests)
        source_before = builder.fingerprint_source(self.source)
        builder.snapshot_source(self.source, self.workspace)

        actions = builder.apply_patch_set(self.workspace, specs)

        self.assertEqual([item['action'] for item in actions], ['apply', 'apply'])
        self.assertEqual((self.workspace / 'src/a.cc').read_bytes(), b'after a\n')
        self.assertEqual((self.workspace / 'cppsrc/player/src/b.cc').read_bytes(),
                         b'after b\n')
        self.assertEqual(builder.fingerprint_source(self.source), source_before)
        with mock.patch.object(builder, 'run') as run:
            actions = builder.apply_patch_set(self.workspace, specs)
            run.assert_not_called()
        self.assertEqual([item['action'] for item in actions],
                         ['already-applied', 'already-applied'])

    def test_publish_rejects_unowned_directory_and_symbolic_links(self):
        staged = self.root / 'staged'
        self.write(staged, 'Thunder.app/new', b'new app')
        marker = self.write(self.output, 'keep', b'user data')
        with self.assertRaises(builder.BuildError):
            builder.publish(staged, self.output)
        self.assertEqual(marker.read_bytes(), b'user data')
        shutil.rmtree(self.output)

        outside = self.root / 'outside-owned'
        outside.mkdir()
        builder.write_json(outside / 'build-report.json', {
            'builder': 'xenon-thunder-macos-v1', 'status': 'success',
        })
        self.write(outside, 'keep', b'other output')
        self.symlink(self.output, outside, directory=True)
        with self.assertRaises(builder.BuildError):
            builder.publish(staged, self.output)
        self.assertTrue(self.output.is_symlink())
        self.assertEqual((outside / 'keep').read_bytes(), b'other output')
        self.assertEqual((staged / 'Thunder.app/new').read_bytes(), b'new app')
        self.output.unlink()

        artifact = self.owned_output()
        staged_link = self.root / 'linked-staging'
        self.symlink(staged_link, staged, directory=True)
        with self.assertRaises(builder.BuildError):
            builder.publish(staged_link, self.output)
        self.assertEqual(artifact.read_bytes(), b'old app')
        self.assertTrue(staged_link.is_symlink())
        self.assertEqual((staged / 'Thunder.app/new').read_bytes(), b'new app')

    def test_publish_replaces_only_owned_output_after_staging_is_complete(self):
        self.owned_output()
        staged = self.root / 'staged'
        self.write(staged, 'Thunder.app/Contents/new', b'new app')
        builder.write_json(staged / 'build-report.json', {
            'builder': 'xenon-thunder-macos-v1', 'status': 'success',
        })
        builder.publish(staged, self.output)
        self.assertEqual((self.output / 'Thunder.app/Contents/new').read_bytes(),
                         b'new app')
        self.assertFalse((self.output / 'Thunder.app/Contents/keep').exists())
        self.assertFalse(staged.exists())
        self.assertEqual(list(self.root.glob('published.previous-*')), [])

    def test_failed_publication_restores_previous_artifact(self):
        artifact = self.owned_output()
        report_before = (self.output / 'build-report.json').read_bytes()
        staged = self.root / 'staged'
        self.write(staged, 'Thunder.app/Contents/new', b'new app')
        rename = Path.rename

        def fail_staged_rename(path, target):
            if path == staged:
                raise OSError('simulated publication failure')
            return rename(path, target)

        with mock.patch.object(Path, 'rename', fail_staged_rename):
            with self.assertRaisesRegex(OSError, 'simulated publication failure'):
                builder.publish(staged, self.output)
        self.assertEqual(artifact.read_bytes(), b'old app')
        self.assertEqual((self.output / 'build-report.json').read_bytes(), report_before)
        self.assertEqual((staged / 'Thunder.app/Contents/new').read_bytes(), b'new app')
        self.assertEqual(list(self.root.glob('published.previous-*')), [])

    def test_archive_publication_failure_restores_directory_and_both_files(self):
        artifact = self.owned_output()
        archive, checksum = builder.archive_tool.archive_paths(self.output)
        archive.write_bytes(b'old archive')
        checksum.write_text(f'{builder.sha256(archive)}  {archive.name}\n')
        original = archive.read_bytes(), checksum.read_bytes()
        staged = self.root / 'work/artifacts'
        self.write(staged, 'Thunder.app/Contents/new', b'new app')
        staged_archive, staged_checksum = (staged.parent / archive.name,
                                           staged.parent / checksum.name)
        staged_archive.write_bytes(b'new archive')
        staged_checksum.write_text(f'{builder.sha256(staged_archive)}  {archive.name}\n')
        rename = Path.rename

        def fail_last_install(path, target):
            if path == staged_checksum:
                raise OSError('simulated checksum publication failure')
            return rename(path, target)

        with mock.patch.object(Path, 'rename', fail_last_install):
            with self.assertRaisesRegex(OSError, 'checksum publication failure'):
                builder.publish(staged, self.output, staged_archive, staged_checksum)
        self.assertEqual(artifact.read_bytes(), b'old app')
        self.assertEqual((archive.read_bytes(), checksum.read_bytes()), original)
        self.assertEqual((staged / 'Thunder.app/Contents/new').read_bytes(), b'new app')
        self.assertEqual(staged_archive.read_bytes(), b'new archive')
        self.assertTrue(staged_checksum.is_file())
        self.assertEqual(list(self.root.glob('*.previous-*')), [])

    def test_publish_rejects_failed_report_and_invalid_archive_before_renaming(self):
        artifact = self.owned_output()
        staged = self.root / 'work/artifacts'
        self.write(staged, 'Thunder.app/Contents/new', b'new app')
        builder.write_json(self.output / 'build-report.json', {
            'builder': 'xenon-thunder-macos-v1', 'status': 'failed',
        })
        with self.assertRaises(builder.BuildError):
            builder.publish(staged, self.output)
        self.owned_output()
        archive, checksum = builder.archive_tool.archive_paths(self.output)
        archive.write_bytes(b'incomplete old archive')
        staged_archive, staged_checksum = (staged.parent / archive.name,
                                           staged.parent / checksum.name)
        staged_archive.write_bytes(b'new archive')
        staged_checksum.write_text(f'{builder.sha256(staged_archive)}  {archive.name}\n')
        with self.assertRaisesRegex(ValueError, 'both be present'):
            builder.publish(staged, self.output, staged_archive, staged_checksum)
        self.assertEqual(artifact.read_bytes(), b'old app')
        self.assertEqual(archive.read_bytes(), b'incomplete old archive')
        self.assertTrue(staged_archive.is_file())

    def test_backup_cleanup_failure_keeps_successful_publication_and_warns(self):
        self.owned_output()
        staged = self.root / 'staged'
        self.write(staged, 'Thunder.app/Contents/new', b'new app')
        builder.write_json(staged / 'build-report.json', {
            'builder': 'xenon-thunder-macos-v1', 'status': 'success',
        })
        errors = io.StringIO()

        with mock.patch.object(builder.shutil, 'rmtree',
                               side_effect=OSError('backup is temporarily busy')):
            with contextlib.redirect_stderr(errors):
                builder.publish(staged, self.output)

        self.assertEqual((self.output / 'Thunder.app/Contents/new').read_bytes(),
                         b'new app')
        self.assertEqual(json.loads((self.output / 'build-report.json').read_text())[
            'status'], 'success')
        self.assertFalse(staged.exists())
        backups = list(self.root.glob('published.previous-*'))
        self.assertEqual(len(backups), 1)
        self.assertEqual((backups[0] / 'Thunder.app/Contents/keep').read_bytes(),
                         b'old app')
        self.assertIn(str(backups[0]), errors.getvalue())

    def test_work_cleanup_failure_returns_success_with_published_artifact(self):
        self.write(self.source, 'app/scripts/mac/workflow.js', b'// fixture\n')
        self.write(self.source, 'app/node_modules/.modules.yaml', b'fixture: true\n')
        support = self.root / 'support'
        self.write(support, 'README.md', b'Compatibility fixture\n')
        source_before = builder.fingerprint_source(self.source)
        errors, output = io.StringIO(), io.StringIO()

        def run_fixture_tool(command, **kwargs):
            arguments = [str(argument) for argument in command]
            if len(arguments) > 1 and Path(arguments[1]).name == 'workflow.js':
                artifacts = Path(arguments[arguments.index('--output') + 1])
                (artifacts / 'Thunder.app/Contents/MacOS').mkdir(parents=True)
                self.write(artifacts, 'Thunder.app/Contents/Resources/app/out.asar',
                           b'fixture app')
                self.write(artifacts, 'Thunder.app/Contents/Info.plist', plistlib.dumps({
                    'CFBundlePackageType': 'BNDL',
                    'CFBundleShortVersionString': '25.1.13',
                }))
            elif len(arguments) > 1 and Path(arguments[1]).name in (
                    'package_adapter.cjs', 'strip_electron_runtime.py'):
                report = Path(arguments[arguments.index('--report') + 1])
                builder.write_json(report, {'fixture': True})
                if Path(arguments[1]).name == 'package_adapter.cjs':
                    bundle = Path(arguments[arguments.index('--bundle') + 1])
                    builder.write_json(bundle / 'Contents/Resources/xenon-native-player.json', {
                        'schemaVersion': 1,
                        'adapter': {'id': 'xunlei.mac-player', 'version': 1},
                    })
            elif arguments[0] != 'codesign':
                self.fail(f'unexpected tool invocation: {arguments}')
            return ''

        # 外部编译和签名使用小型替身，真实执行快照、发布和 main() 状态处理。
        with contextlib.ExitStack() as stack:
            stack.enter_context(mock.patch.object(builder.sys, 'platform', 'darwin'))
            stack.enter_context(mock.patch.object(builder, 'SUPPORT', support))
            stack.enter_context(mock.patch.object(builder, 'load_patch_specs', return_value=[]))
            stack.enter_context(mock.patch.object(builder, 'source_metadata', return_value={}))
            stack.enter_context(mock.patch.object(builder, 'resolve_tools', return_value=(
                self.root / 'node', self.root / 'pnpm.cjs', self.root / 'cmake')))
            stack.enter_context(mock.patch.object(builder, 'verify_native_imports',
                                                 return_value={'fixture': True}))
            stack.enter_context(mock.patch.object(builder, '_copy_file',
                                                 side_effect=shutil.copy2))
            stack.enter_context(mock.patch.object(builder, 'run', side_effect=run_fixture_tool))
            cleanup = stack.enter_context(mock.patch.object(
                builder.shutil, 'rmtree', side_effect=OSError('work directory is busy')))
            stack.enter_context(contextlib.redirect_stdout(output))
            stack.enter_context(contextlib.redirect_stderr(errors))
            status = builder.main(['--source', str(self.source),
                                   '--output', str(self.output), '--arch', 'arm64'])

        self.assertEqual(status, 0)
        report = json.loads((self.output / 'build-report.json').read_text())
        self.assertEqual(report['status'], 'success')
        self.assertTrue(report['source_unchanged'])
        archive, checksum = builder.archive_tool.archive_paths(self.output)
        self.assertTrue(archive.is_file())
        self.assertEqual(checksum.read_text(),
                         f'{builder.sha256(archive)}  {archive.name}\n')
        self.assertEqual(report['archive']['sha256'], builder.sha256(archive))
        self.assertEqual((self.output /
                          'Thunder.app/Contents/Resources/app/out.asar').read_bytes(),
                         b'fixture app')
        work_directories = list(self.root.glob('.thunder-build-*'))
        self.assertEqual(len(work_directories), 1)
        cleanup.assert_called_once_with(work_directories[0])
        self.assertTrue((work_directories[0] / 'source').is_dir())
        self.assertIn(str(work_directories[0]), errors.getvalue())
        self.assertNotIn('构建未发布', errors.getvalue())
        self.assertNotIn('错误:', errors.getvalue())
        self.assertEqual(builder.fingerprint_source(self.source), source_before)


if __name__ == '__main__':
    unittest.main()
