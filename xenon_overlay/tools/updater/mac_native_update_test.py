#!/usr/bin/env python3
# Copyright 2026 The Xenon Overlay Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Compile and integration-test the native helper without a Chromium build.

Runs only on macOS with Xcode command line tools. All applications, processes,
and signatures are test fixtures inside TemporaryDirectory; no GUI is opened.
"""

import json
import os
from pathlib import Path
import plistlib
import select
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]
UPDATER = ROOT / 'xenon_overlay/chrome/browser/updater'


@unittest.skipUnless(sys.platform == 'darwin', 'macOS native updater')
class NativeUpdateTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build_directory = tempfile.TemporaryDirectory(
            prefix='xenon-native-update-build-')
        cls.build = Path(cls.build_directory.name)
        cls.helper = cls.build / 'xenon_update_helper'
        subprocess.run([
            'xcrun', 'clang++', '-std=c++20', '-fobjc-arc', '-fblocks',
            '-Wall', '-Wextra', '-Werror', '-I', str(ROOT),
            str(UPDATER / 'xenon_update_mac_helper.mm'),
            str(UPDATER / 'xenon_update_mac_util.mm'),
            str(UPDATER / 'xenon_update_mac_privileged_client.mm'),
            str(UPDATER / 'xenon_update_mac_privileged_util.mm'),
            str(UPDATER / 'xenon_update_mac_privileged_generation.mm'),
            '-framework', 'Foundation', '-framework', 'AppKit',
            '-framework', 'Security', '-framework', 'ServiceManagement',
            '-o', str(cls.helper),
        ], check=True)
        source = cls.build / 'fixture.c'
        source.write_text('int main(void) { return 0; }\n')
        cls.executable = cls.build / 'fixture'
        subprocess.run(['xcrun', 'clang', str(source), '-o', str(cls.executable)],
                       check=True)
        probe_source = cls.build / 'privileged_client_probe.mm'
        probe_source.write_text(r'''
#import <Foundation/Foundation.h>
#include <stdio.h>
#include "xenon_overlay/chrome/browser/updater/xenon_update_mac_privileged_client.h"
int main(int argc, char** argv) {
  @autoreleasepool {
    if (argc < 2) return 2;
    using namespace xenon::updater;
    NSString* mode = @(argv[1]);
    NSDictionary* result = nil;
    if ([mode isEqual:@"prepare"] && argc == 4) {
      __block bool done = false;
      __block NSDictionary* response = nil;
      BeginPrivilegedUpdatePreparation(@(argv[2]), @(argv[3]),
          ^(NSString* token, bool approval, NSString* error) {
            response = @{@"token":token ?: @"", @"approval":@(approval),
                         @"error":error ?: @""};
            done = true;
          });
      while (!done) [[NSRunLoop currentRunLoop]
          runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
      result = response;
    } else if ([mode isEqual:@"generation"] && argc == 3) {
      NSString* scenario = @(argv[2]);
      __block int checks = 0;
      __block int refreshes = 0;
      __block int completions = 0;
      __block bool done = false;
      __block NSDictionary* response = nil;
      internal::RunServiceGenerationWorkflow(
          ^(internal::GenerationReply checked) {
            ++checks;
            const bool stale = ![scenario isEqual:@"current"] &&
                (checks == 1 || [scenario isEqual:@"still-stale"]);
            dispatch_async(dispatch_get_main_queue(), ^{
              NSString* error = [scenario isEqual:@"busy"] ? @"active transaction" : nil;
              checked(stale, error);
              // Misbehaving/late duplicate replies must not trigger another
              // refresh, another prepare, or a second final callback.
              checked(stale, error);
            });
          },
          ^(internal::ServiceRefreshReply refreshed) {
            ++refreshes;
            dispatch_async(dispatch_get_main_queue(), ^{
              const bool approval = [scenario isEqual:@"approval"];
              NSString* error = [scenario isEqual:@"refresh-failed"] ? @"unregister failed" : nil;
              refreshed(approval, error);
              refreshed(approval, error);
            });
          },
          ^(bool approval, NSString* error) {
            ++completions;
            response = @{@"checks":@(checks), @"refreshes":@(refreshes),
                @"completions":@(completions), @"approval":@(approval),
                @"error":error ?: @""};
            done = true;
          });
      while (!done) [[NSRunLoop currentRunLoop]
          runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
      result = response;
    } else if ([mode isEqual:@"ownership"] && argc == 3) {
      result = @{@"requires_elevation":@(UpdateRequiresElevation(@(argv[2])))};
    } else if ([mode isEqual:@"write"] && argc == 4) {
      NSString* error = nil;
      bool ok = WritePrivilegedUpdateResult(@(argv[2]), @"2.0", @"launching",
                                             @"test", &error, @(argv[3]));
      result = @{@"ok":@(ok), @"error":error ?: @""};
    } else if ([mode isEqual:@"read"] && argc == 4) {
      NSString* error = nil;
      result = ReadPrivilegedUpdateResult(@(argv[2]), &error, @(argv[3]))
          ?: @{@"error":error ?: @"missing"};
    } else if ([mode isEqual:@"error"]) {
      NSError* nested = [NSError errorWithDomain:NSOSStatusErrorDomain
          code:-128 userInfo:nil];
      NSError* outer = [NSError errorWithDomain:NSCocoaErrorDomain code:256
          userInfo:@{NSUnderlyingErrorKey:nested, @"private-url":@"must-not-leak"}];
      result = @{@"error":DescribeUpdateNSError(outer)};
    } else return 2;
    NSData* json = [NSJSONSerialization dataWithJSONObject:result options:0 error:nil];
    fwrite(json.bytes, 1, json.length, stdout);
    return 0;
  }
}
''')
        cls.client_probe = cls.build / 'privileged_client_probe'
        subprocess.run([
            'xcrun', 'clang++', '-std=c++20', '-fobjc-arc', '-fblocks',
            '-Wall', '-Wextra', '-Werror', '-I', str(ROOT), str(probe_source),
            str(UPDATER / 'xenon_update_mac_privileged_client.mm'),
            str(UPDATER / 'xenon_update_mac_privileged_util.mm'),
            str(UPDATER / 'xenon_update_mac_privileged_generation.mm'),
            str(UPDATER / 'xenon_update_mac_util.mm'),
            '-framework', 'Foundation', '-framework', 'AppKit',
            '-framework', 'Security', '-framework', 'ServiceManagement',
            '-o', str(cls.client_probe),
        ], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.build_directory.cleanup()

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='xenon-native-update-')
        self.root = Path(self.directory.name)
        self.addCleanup(self.directory.cleanup)
        self.parent = subprocess.Popen(['/bin/sleep', '60'])
        self.addCleanup(self.stop_parent)
        self.helpers = []
        self.addCleanup(self.stop_helpers)

    def stop_parent(self):
        if self.parent.poll() is None:
            self.parent.terminate()
        self.parent.wait(timeout=10)

    def stop_helpers(self):
        for process in self.helpers:
            if process.poll() is None:
                process.kill()
            process.communicate(timeout=10)

    def make_app(self, path, version, identifier='org.xenon.native-update-test'):
        (path / 'Contents/MacOS').mkdir(parents=True)
        (path / 'Contents/Resources').mkdir()
        (path / 'Contents/Info.plist').write_bytes(plistlib.dumps({
            'CFBundleIdentifier': identifier,
            'CFBundleExecutable': 'fixture',
            'CFBundlePackageType': 'APPL',
            'CFBundleShortVersionString': version,
        }))
        shutil.copy2(self.executable, path / 'Contents/MacOS/fixture')
        (path / 'Contents/Resources/version').write_text(version)
        (path / 'Contents/Resources/current').symlink_to('version')
        subprocess.run(['/usr/bin/codesign', '--force', '--sign', '-', str(path)],
                       check=True, capture_output=True)
        return path

    def pair(self, name='Browser.app'):
        target = self.make_app(self.root / name, '1.0')
        prepared = self.make_app(self.root / 'preparation' / name, '2.0')
        return target, prepared

    def start_helper(self, target, prepared):
        read_fd, write_fd = os.pipe()
        try:
            process = subprocess.Popen([
                str(self.helper), str(self.parent.pid), str(write_fd),
                str(prepared), str(target), '0',
            ], pass_fds=(write_fd,), cwd=self.root, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE)
            self.helpers.append(process)
        finally:
            os.close(write_fd)
        try:
            readable, _, _ = select.select([read_fd], [], [], 10)
            self.assertTrue(readable, 'helper failed to acknowledge or close pipe')
            ready = os.read(read_fd, 1)
        finally:
            os.close(read_fd)
        return process, ready

    def read_result(self, target):
        journal = Path(str(target) + '.xenon-update-result.json')
        self.assertEqual(journal.stat().st_mode & 0o777, 0o600)
        result = json.loads(journal.read_text())
        self.assertEqual(result['schema_version'], 1)
        self.assertEqual(Path(result['target']).resolve(), target.resolve())
        self.assertEqual(result['target_version'], '2.0')
        return result

    def quarantine_fixture(self, path):
        attribute = b'0081;12345678;Xenon Native Test;'
        subprocess.run(['/usr/bin/xattr', '-w', 'com.apple.quarantine',
                        attribute.decode(), str(path)], check=True)
        return attribute

    def test_wait_then_atomic_swap_preserves_modes_and_symlinks(self):
        target, prepared = self.pair('Browser$(touch injected) "test".app')
        process, ready = self.start_helper(target, prepared)
        self.assertEqual(ready, b'R')
        self.assertIsNone(process.poll())
        self.assertEqual((target / 'Contents/Resources/version').read_text(), '1.0')
        self.stop_parent()
        _, error = process.communicate(timeout=15)
        self.assertEqual(process.returncode, 0, error.decode())
        self.assertEqual((target / 'Contents/Resources/version').read_text(), '2.0')
        self.assertTrue((target / 'Contents/Resources/current').is_symlink())
        self.assertTrue(os.access(target / 'Contents/MacOS/fixture', os.X_OK))
        self.assertFalse(prepared.exists())
        self.assertFalse((self.root / 'injected').exists())
        self.assertEqual(self.read_result(target)['status'], 'installed_pending_launch')

    def test_quarantined_development_update_is_not_rejected_before_ready(self):
        cases = []
        for index, relative in enumerate(('', 'Contents/MacOS/fixture')):
            with self.subTest(quarantined_path=relative):
                target, prepared = self.pair(f'Browser{index}.app')
                path = prepared / relative
                attribute = self.quarantine_fixture(path)
                process, ready = self.start_helper(target, prepared)
                self.assertEqual(ready, b'R')
                self.assertIsNone(self.parent.poll())
                self.assertEqual((target / 'Contents/Resources/version').read_text(), '1.0')
                cases.append((process, target, relative, attribute))
        self.stop_parent()
        for process, target, relative, attribute in cases:
            with self.subTest(quarantined_path=relative):
                _, error = process.communicate(timeout=15)
                self.assertEqual(process.returncode, 0, error.decode())
                stored_attribute = subprocess.run([
                    '/usr/bin/xattr', '-p', 'com.apple.quarantine', str(target / relative)],
                    check=True, capture_output=True).stdout.rstrip(b'\n')
                self.assertEqual(stored_attribute, attribute)
                self.assertEqual((target / 'Contents/Resources/version').read_text(), '2.0')
                result = self.read_result(target)
                self.assertEqual(result['status'], 'installed_pending_launch')
                self.assertEqual((Path(result['retained_path']) /
                                  'Contents/Resources/version').read_text(), '1.0')

    def test_quarantined_unsigned_update_waits_for_launch(self):
        target, prepared = self.pair()
        for app in (target, prepared):
            subprocess.run(['/usr/bin/codesign', '--remove-signature',
                            str(app / 'Contents/MacOS/fixture')],
                           check=True, capture_output=True)
            shutil.rmtree(app / 'Contents/_CodeSignature')
        self.quarantine_fixture(prepared)
        process, ready = self.start_helper(target, prepared)
        self.assertEqual(ready, b'R')
        self.assertIsNone(self.parent.poll())
        self.stop_parent()
        _, error = process.communicate(timeout=15)
        self.assertEqual(process.returncode, 0, error.decode())
        self.assertEqual(self.read_result(target)['status'], 'installed_pending_launch')

    def test_quarantine_added_after_ready_is_preserved_for_user_approval(self):
        target, prepared = self.pair()
        process, ready = self.start_helper(target, prepared)
        self.assertEqual(ready, b'R')
        self.quarantine_fixture(prepared)
        self.stop_parent()
        _, error = process.communicate(timeout=15)
        self.assertEqual(process.returncode, 0, error.decode())
        self.assertEqual((target / 'Contents/Resources/version').read_text(), '2.0')
        self.assertFalse(prepared.exists())
        result = self.read_result(target)
        self.assertEqual(result['status'], 'installed_pending_launch')
        self.assertEqual((Path(result['retained_path']) /
                          'Contents/Resources/version').read_text(), '1.0')

    def test_install_pending_launch_replaces_previous_failure_journal(self):
        target, prepared = self.pair()
        (prepared / 'Contents/Resources/version').write_text('tampered')
        first, ready = self.start_helper(target, prepared)
        self.assertEqual(ready, b'')
        first.communicate(timeout=15)
        self.assertEqual(first.returncode, 5)
        self.assertEqual(self.read_result(target)['status'], 'validation_failed')
        shutil.rmtree(prepared)
        self.make_app(prepared, '2.0')
        second, ready = self.start_helper(target, prepared)
        self.assertEqual(ready, b'R')
        self.stop_parent()
        _, error = second.communicate(timeout=15)
        self.assertEqual(second.returncode, 0, error.decode())
        self.assertEqual(self.read_result(target)['status'], 'installed_pending_launch')
        self.assertEqual(list(self.root.glob('.xenon-result-*.tmp')), [])

    def test_readonly_installed_bundle_updates_without_changing_old_permissions(self):
        target, prepared = self.pair()

        def restore_fixture_directories():
            for directory, _, _ in os.walk(self.root):
                Path(directory).chmod(0o700)

        self.addCleanup(restore_fixture_directories)
        # Model a root-owned 0755 package as the ordinary user: every old
        # directory is searchable/readable but not writable. No root is needed.
        for directory, _, _ in os.walk(target):
            Path(directory).chmod(0o555)
        process, ready = self.start_helper(target, prepared)
        self.assertEqual(ready, b'R')
        self.stop_parent()
        _, error = process.communicate(timeout=15)
        self.assertEqual(process.returncode, 0, error.decode())
        self.assertEqual((target / 'Contents/Resources/version').read_text(), '2.0')
        backups = list(self.root.glob('.xenon-swap-*.app'))
        self.assertEqual(len(backups), 1, error.decode())
        backup = backups[0]
        self.assertEqual((backup / 'Contents/Resources/version').read_text(), '1.0')
        for directory, _, _ in os.walk(backup):
            self.assertEqual(Path(directory).stat().st_mode & 0o777, 0o555)
        self.assertIn(str(backup), error.decode())
        self.assertIn('previous application retained', error.decode())
        self.assertFalse(prepared.parent.exists())
        subprocess.run(['/usr/bin/codesign', '--verify', '--deep', '--strict',
                        str(target)], check=True, capture_output=True)

    def test_readonly_prepared_bundle_fails_promotion_without_chmod(self):
        target, prepared = self.pair()
        prepared.chmod(0o555)
        try:
            process, ready = self.start_helper(target, prepared)
            self.assertEqual(ready, b'R')
            self.stop_parent()
            _, error = process.communicate(timeout=15)
            self.assertEqual(process.returncode, 8, error.decode())
            self.assertEqual((target / 'Contents/Resources/version').read_text(), '1.0')
            self.assertEqual((prepared / 'Contents/Resources/version').read_text(), '2.0')
            self.assertEqual(prepared.stat().st_mode & 0o777, 0o555)
            self.assertEqual(list(self.root.glob('.xenon-swap-*.app')), [])
        finally:
            prepared.chmod(0o755)

    def test_failed_swap_retains_both_applications(self):
        target, prepared = self.pair()
        process, ready = self.start_helper(target, prepared)
        self.assertEqual(ready, b'R')
        prepared.parent.chmod(0o500)
        try:
            self.stop_parent()
            _, error = process.communicate(timeout=15)
            self.assertEqual(process.returncode, 8, error.decode())
            self.assertEqual((target / 'Contents/Resources/version').read_text(), '1.0')
            self.assertEqual((prepared / 'Contents/Resources/version').read_text(), '2.0')
        finally:
            prepared.parent.chmod(0o700)

    def test_corrupt_signature_is_rejected_before_shutdown(self):
        target, prepared = self.pair()
        (prepared / 'Contents/Resources/version').write_text('tampered')
        process, ready = self.start_helper(target, prepared)
        self.assertEqual(ready, b'')
        _, error = process.communicate(timeout=15)
        self.assertEqual(process.returncode, 5, error.decode())
        self.assertEqual((target / 'Contents/Resources/version').read_text(), '1.0')

    def test_second_helper_cannot_commit_concurrently(self):
        target, prepared = self.pair()
        first, ready = self.start_helper(target, prepared)
        self.assertEqual(ready, b'R')
        second, ready = self.start_helper(target, prepared)
        self.assertEqual(ready, b'')
        _, error = second.communicate(timeout=15)
        self.assertEqual(second.returncode, 3, error.decode())
        self.stop_parent()
        _, error = first.communicate(timeout=15)
        self.assertEqual(first.returncode, 0, error.decode())

    def test_revalidate_after_browser_exit(self):
        target, prepared = self.pair()
        process, ready = self.start_helper(target, prepared)
        self.assertEqual(ready, b'R')
        (prepared / 'Contents/Resources/version').write_text('tampered-after-ready')
        self.stop_parent()
        _, error = process.communicate(timeout=15)
        self.assertEqual(process.returncode, 8, error.decode())
        self.assertEqual((target / 'Contents/Resources/version').read_text(), '1.0')

    def test_unsigned_development_build_can_update(self):
        target, prepared = self.pair()
        for app in (target, prepared):
            subprocess.run([
                '/usr/bin/codesign', '--remove-signature',
                str(app / 'Contents/MacOS/fixture'),
            ], check=True, capture_output=True)
            shutil.rmtree(app / 'Contents/_CodeSignature')
        process, ready = self.start_helper(target, prepared)
        self.assertEqual(ready, b'R')
        self.stop_parent()
        _, error = process.communicate(timeout=15)
        self.assertEqual(process.returncode, 0, error.decode())
        self.assertEqual((target / 'Contents/Resources/version').read_text(), '2.0')

    def test_different_application_is_rejected(self):
        target = self.make_app(self.root / 'Browser.app', '1.0')
        prepared = self.make_app(self.root / 'preparation/Browser.app', '2.0',
                                 identifier='org.xenon.wrong-application')
        process, ready = self.start_helper(target, prepared)
        self.assertEqual(ready, b'')
        _, error = process.communicate(timeout=15)
        self.assertEqual(process.returncode, 5, error.decode())

    def test_broken_readiness_pipe_cancels_replacement(self):
        target, prepared = self.pair()
        read_fd, write_fd = os.pipe()
        os.close(read_fd)
        try:
            process = subprocess.Popen([
                str(self.helper), str(self.parent.pid), str(write_fd),
                str(prepared), str(target), '0',
            ], pass_fds=(write_fd,), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            self.helpers.append(process)
        finally:
            os.close(write_fd)
        _, error = process.communicate(timeout=15)
        self.assertEqual(process.returncode, 6, error.decode())
        self.stop_parent()
        self.assertEqual((target / 'Contents/Resources/version').read_text(), '1.0')
        self.assertEqual((prepared / 'Contents/Resources/version').read_text(), '2.0')

    def probe(self, *args):
        result = subprocess.run([str(self.client_probe), *map(str, args)],
                                check=True, capture_output=True, timeout=20)
        return json.loads(result.stdout)

    def test_privileged_preflight_rejects_development_bundle_without_registration(self):
        target, prepared = self.pair()
        result = self.probe('prepare', prepared, target)
        self.assertEqual(result['token'], '')
        self.assertFalse(result['approval'])
        self.assertTrue(result['error'])
        self.assertEqual((target / 'Contents/Resources/version').read_text(), '1.0')

    def test_privileged_helper_rejects_untrusted_target_before_handshake(self):
        target, _ = self.pair()
        read_fd, write_fd = os.pipe()
        try:
            process = subprocess.Popen([
                str(self.helper), '--privileged', str(os.getpid()), str(write_fd),
                '556DE44A-5314-4ECF-9E17-6E625E719E31', str(target), '2.0', '0',
            ], pass_fds=(write_fd,), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            self.helpers.append(process)
        finally:
            os.close(write_fd)
        try:
            readable, _, _ = select.select([read_fd], [], [], 10)
            self.assertTrue(readable)
            self.assertEqual(os.read(read_fd, 1), b'')
        finally:
            os.close(read_fd)
        _, error = process.communicate(timeout=10)
        self.assertEqual(process.returncode, 5, error.decode())
        self.assertEqual((target / 'Contents/Resources/version').read_text(), '1.0')

    def test_privileged_result_cache_is_private_and_rejects_links(self):
        target, _ = self.pair()
        cache = self.root / 'cache'
        cache.mkdir()
        self.assertTrue(self.probe('write', target, cache)['ok'])
        result = self.probe('read', target, cache)
        self.assertEqual(Path(result['target']).resolve(), target.resolve())
        self.assertEqual(result['status'], 'launching')
        journal, = cache.glob('XenonUpdaterResults/*/result.json')
        self.assertEqual(journal.stat().st_mode & 0o777, 0o600)
        self.assertEqual(journal.parent.stat().st_mode & 0o777, 0o700)
        outside = self.root / 'external-result'
        os.link(journal, outside)
        self.assertIn('error', self.probe('read', target, cache))
        outside.unlink()
        journal.unlink()
        journal.symlink_to(self.root / 'missing')
        self.assertIn('error', self.probe('read', target, cache))
        journal.unlink()
        journal.parent.rmdir()
        (cache / 'XenonUpdaterResults').rmdir()
        (cache / 'XenonUpdaterResults').symlink_to(self.root, target_is_directory=True)
        self.assertFalse(self.probe('write', target, cache)['ok'])

    def test_elevation_checks_root_and_every_nested_directory(self):
        target, _ = self.pair()
        self.assertFalse(self.probe('ownership', target)['requires_elevation'])
        self.assertTrue(self.probe('ownership', '/System/Library/CoreServices/Installer.app')
                        ['requires_elevation'])
        # An unreadable descendant must fail closed even when the app's root
        # and its parent look writable and owned. Never assume hidden entries
        # have the same owner as the bundle root.
        nested = target / 'Contents/Resources'
        nested.chmod(0)
        try:
            self.assertTrue(self.probe('ownership', target)['requires_elevation'])
        finally:
            nested.chmod(0o755)
        special = nested / 'unsafe-special-file'
        os.mkfifo(special)
        self.assertTrue(self.probe('ownership', target)['requires_elevation'])
        special.unlink()
        self.assertFalse(self.probe('ownership', target)['requires_elevation'])

    def test_installer_error_reports_underlying_cancel_without_user_info(self):
        message = self.probe('error')['error']
        self.assertIn('NSCocoaErrorDomain (256)', message)
        self.assertIn('NSOSStatusErrorDomain (-128)', message)
        self.assertNotIn('must-not-leak', message)

    def test_service_generation_refresh_is_ordered_bounded_and_approval_aware(self):
        cases = {
            'current': (1, 0, False, False),
            'stale': (2, 1, False, False),
            'still-stale': (2, 1, False, True),
            'busy': (1, 0, False, True),
            'refresh-failed': (1, 1, False, True),
            'approval': (1, 1, True, False),
        }
        for scenario, (checks, refreshes, approval, error) in cases.items():
            with self.subTest(scenario=scenario):
                result = self.probe('generation', scenario)
                self.assertEqual(result['checks'], checks)
                self.assertEqual(result['refreshes'], refreshes)
                self.assertEqual(result['completions'], 1)
                self.assertEqual(result['approval'], approval)
                self.assertEqual(bool(result['error']), error)


if __name__ == '__main__':
    unittest.main(verbosity=2)
