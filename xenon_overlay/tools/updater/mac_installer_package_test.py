#!/usr/bin/env python3
# Copyright 2026 The Xenon Overlay Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Test PKG preparation and eligibility without opening Installer or any app."""

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]
UPDATER = ROOT / 'xenon_overlay/chrome/browser/updater'


@unittest.skipUnless(sys.platform == 'darwin', 'macOS native installer packages')
class InstallerPackageTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build_directory = tempfile.TemporaryDirectory(prefix='xenon-pkg-test-build-')
        cls.build = Path(cls.build_directory.name)
        cls.tool = cls.build / 'package_probe'
        source = cls.build / 'package_probe.mm'
        source.write_text(r'''
#include "xenon_overlay/chrome/browser/updater/xenon_update_mac_util.h"
#include <stdio.h>
int main() {
  @autoreleasepool {
    NSArray<NSString*>* args = NSProcessInfo.processInfo.arguments;
    NSString* error = nil;
    NSString* prepared = nil;
    bool ok = false;
    if (args.count == 3 && [args[1] isEqual:@"eligible"]) {
      ok = xenon::updater::CanInstallOwnedUpdateBundle(args[2], &error);
    } else if (args.count == 5 && [args[1] isEqual:@"prepare"]) {
      prepared = xenon::updater::PrepareInstallerPackageInCache(
          args[2], args[3], args[4], &error);
      ok = prepared != nil;
    }
    NSData* data = [NSJSONSerialization dataWithJSONObject:
        @{@"ok": @(ok), @"path": prepared ?: @"", @"error": error ?: @""}
        options:0 error:nil];
    fwrite(data.bytes, 1, data.length, stdout);
    return ok ? 0 : 1;
  }
}
''')
        subprocess.run([
            'xcrun', 'clang++', '-std=c++20', '-fobjc-arc', '-fblocks',
            '-Wall', '-Wextra', '-Werror', '-I', str(ROOT), str(source),
            str(UPDATER / 'xenon_update_mac_util.mm'),
            '-framework', 'Foundation', '-framework', 'Security',
            '-o', str(cls.tool),
        ], check=True)
        payload = cls.build / 'payload'
        payload.mkdir()
        (payload / 'fixture.txt').write_text('package preparation fixture\n')
        cls.package = cls.build / 'fixture.pkg'
        subprocess.run([
            '/usr/bin/pkgbuild', '--root', str(payload), '--identifier',
            'org.xenon.package-preparation-test', '--version', '1.0',
            '--install-location', '/Applications', str(cls.package),
        ], check=True, capture_output=True)
        cls.digest = hashlib.sha256(cls.package.read_bytes()).hexdigest()

    @classmethod
    def tearDownClass(cls):
        cls.build_directory.cleanup()

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='xenon-pkg-test-')
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.cache = self.root / 'cache'
        self.cache.mkdir()
        self.downloads = self.root / 'downloads'
        self.downloads.mkdir()
        self.source = self.downloads / 'download.pkg'
        shutil.copy2(self.package, self.source)

    def probe(self, *args):
        result = subprocess.run([str(self.tool), *map(str, args)],
                                capture_output=True, check=False)
        self.assertIn(result.returncode, (0, 1), result.stderr.decode())
        return json.loads(result.stdout)

    def prepare(self, source=None, digest=None, cache=None):
        return self.probe('prepare', source or self.source,
                          digest or self.digest, cache or self.cache)

    def test_private_verified_copy_survives_download_cleanup(self):
        result = self.prepare(digest=self.digest.upper())
        self.assertTrue(result['ok'], result['error'])
        copied = Path(result['path'])
        self.assertEqual(copied.suffix, '.pkg')
        self.assertEqual(copied.stat().st_mode & 0o777, 0o600)
        self.assertEqual(copied.parent.stat().st_mode & 0o777, 0o700)
        self.assertEqual(copied.parent.parent.stat().st_mode & 0o777, 0o700)
        self.assertEqual(hashlib.sha256(copied.read_bytes()).hexdigest(), self.digest)
        shutil.rmtree(self.downloads)
        self.assertTrue(copied.is_file())

    def test_quarantine_is_preserved(self):
        value = '0081;12345678;Xenon Installer Test;'
        subprocess.run(['/usr/bin/xattr', '-w', 'com.apple.quarantine',
                        value, str(self.source)], check=True)
        result = self.prepare()
        self.assertTrue(result['ok'], result['error'])
        attributes = []
        for path in (self.source, result['path']):
            actual = subprocess.run(['/usr/bin/xattr', '-p', 'com.apple.quarantine',
                                     str(path)], check=True, capture_output=True)
            attributes.append(actual.stdout.decode().strip())
        self.assertEqual(attributes[0], value)
        # Native copying can update origin/timestamp and add propagation bits.
        # The update must retain quarantine, not overwrite macOS provenance.
        self.assertEqual(int(attributes[1].split(';', 1)[0], 16) & 0x0081, 0x0081)

    def test_digest_mismatch_cleans_only_its_failed_copy(self):
        good = self.prepare()
        self.assertTrue(good['ok'])
        result = self.prepare(digest='0' * 64)
        self.assertFalse(result['ok'])
        self.assertIn('SHA-256', result['error'])
        self.assertEqual(list((self.cache / 'XenonUpdaterPackages').iterdir()),
                         [Path(good['path']).parent])
        self.assertTrue(self.source.is_file())

    def test_retry_reuses_the_verified_cached_file(self):
        first = self.prepare()
        self.assertTrue(first['ok'], first['error'])
        cached = Path(first['path'])
        inode = cached.stat().st_ino
        shutil.rmtree(self.downloads)
        for _ in range(3):
            retried = self.prepare(source=cached)
            self.assertTrue(retried['ok'], retried['error'])
            self.assertEqual(retried['path'], first['path'])
            self.assertEqual(cached.stat().st_ino, inode)
        self.assertEqual(list(cached.parent.parent.iterdir()), [cached.parent])

    def test_changed_cached_package_is_rejected_without_copying(self):
        first = self.prepare()
        self.assertTrue(first['ok'])
        cached = Path(first['path'])
        cached.write_bytes(b'xar!tampered cached package')
        retried = self.prepare(source=cached)
        self.assertFalse(retried['ok'])
        self.assertIn('SHA-256', retried['error'])
        self.assertEqual(list(cached.parent.parent.iterdir()), [cached.parent])
        self.assertEqual(cached.read_bytes(), b'xar!tampered cached package')

    def test_cached_package_must_keep_private_modes_and_single_link(self):
        first = self.prepare()
        self.assertTrue(first['ok'])
        cached = Path(first['path'])
        alias = self.root / 'hardlink.pkg'
        os.link(cached, alias)
        self.assertFalse(self.prepare(source=cached)['ok'])
        alias.unlink()
        cached.chmod(0o644)
        self.assertFalse(self.prepare(source=cached)['ok'])
        cached.chmod(0o600)
        cached.parent.chmod(0o755)
        self.assertFalse(self.prepare(source=cached)['ok'])
        cached.parent.chmod(0o700)
        self.assertEqual(list(cached.parent.parent.iterdir()), [cached.parent])
        self.assertTrue(self.prepare(source=cached)['ok'])

    def test_cached_attempt_directory_cannot_be_replaced_with_symlink(self):
        first = self.prepare()
        self.assertTrue(first['ok'])
        cached = Path(first['path'])
        moved = self.root / 'moved-cache'
        cached.parent.rename(moved)
        cached.parent.symlink_to(moved, target_is_directory=True)
        retried = self.prepare(source=cached)
        self.assertFalse(retried['ok'])
        self.assertIn('private original', retried['error'])
        self.assertEqual(list(cached.parent.parent.iterdir()), [cached.parent])
        self.assertTrue((moved / 'update.pkg').is_file())

    def test_rejects_symlink_and_directory_sources(self):
        alias = self.downloads / 'alias.pkg'
        alias.symlink_to(self.source)
        directory = self.downloads / 'directory.pkg'
        directory.mkdir()
        self.assertFalse(self.prepare(source=alias)['ok'])
        self.assertFalse(self.prepare(source=directory)['ok'])
        self.assertTrue(alias.is_symlink())
        self.assertTrue(self.source.is_file())

    def test_rejects_invalid_pkg_and_digest(self):
        self.assertFalse(self.prepare(digest='not-a-digest')['ok'])
        renamed = self.downloads / 'download.zip'
        shutil.copy2(self.source, renamed)
        self.assertFalse(self.prepare(source=renamed)['ok'])
        self.source.write_bytes(b'PK\x03\x04not a flat installer package')
        digest = hashlib.sha256(self.source.read_bytes()).hexdigest()
        result = self.prepare(digest=digest)
        self.assertFalse(result['ok'])
        self.assertIn('XAR', result['error'])

    def test_rejects_symlink_or_nonprivate_cache(self):
        alias = self.root / 'cache-alias'
        alias.symlink_to(self.cache, target_is_directory=True)
        self.assertFalse(self.prepare(cache=alias)['ok'])
        packages = self.cache / 'XenonUpdaterPackages'
        packages.mkdir(mode=0o755)
        self.assertFalse(self.prepare()['ok'])
        self.assertEqual(packages.stat().st_mode & 0o777, 0o755)

    def test_only_owned_apps_with_writable_parent_qualify_for_diff(self):
        app = self.root / 'Browser.app'
        app.mkdir()
        self.assertTrue(self.probe('eligible', app)['ok'])
        alias = self.root / 'Alias.app'
        alias.symlink_to(app, target_is_directory=True)
        self.assertFalse(self.probe('eligible', alias)['ok'])
        # Read-only inspection of an OS-owned bundle; never launch or modify it.
        self.assertFalse(self.probe('eligible',
            '/System/Library/CoreServices/Installer.app')['ok'])
        self.root.chmod(0o500)
        try:
            self.assertFalse(self.probe('eligible', app)['ok'])
        finally:
            self.root.chmod(0o700)


if __name__ == '__main__':
    unittest.main(verbosity=2)
