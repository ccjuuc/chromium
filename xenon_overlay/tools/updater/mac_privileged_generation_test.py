#!/usr/bin/env python3
# Copyright 2026 The Xenon Overlay Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Unprivileged generation fixtures; no app launch, XPC or service registration."""

import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]
UPDATER = ROOT / 'xenon_overlay/chrome/browser/updater'
PLIST = 'com.xl153.updater.privileged.plist'
HELPER = 'xenon_privileged_update_helper'
HARNESS = r'''
#import <Foundation/Foundation.h>
#include <stdio.h>
#include <unistd.h>
#include "xenon_overlay/chrome/browser/updater/xenon_update_mac_privileged_generation.h"
int main(int argc, char** argv) {
  @autoreleasepool {
    if (argc < 3 || geteuid() == 0) return 2;
    NSString* target = [NSString stringWithUTF8String:argv[1]];
    NSString* mode = [NSString stringWithUTF8String:argv[2]];
    NSString* error = nil;
    NSMutableDictionary* result = [NSMutableDictionary dictionary];
    auto generation = [&](bool running) {
      return xenon::updater::PrivilegedServiceGeneration(target, running, &error);
    };
    if ([mode isEqual:@"replace"] && argc == 4) {
      result[@"before"] = generation(true) ?: NSNull.null;
      NSString* helper = [target stringByAppendingPathComponent:
          @"Contents/Helpers/xenon_privileged_update_helper"];
      if (rename(argv[3], helper.fileSystemRepresentation) != 0) return 3;
      result[@"static_after"] = generation(false) ?: NSNull.null;
      result[@"running_after"] = generation(true) ?: NSNull.null;
    } else {
      result[@"generation"] = generation([mode isEqual:@"running"]) ?: NSNull.null;
    }
    result[@"error"] = error ?: @"";
    NSData* bytes = [NSJSONSerialization dataWithJSONObject:result options:0 error:nil];
    fwrite(bytes.bytes, 1, bytes.length, stdout);
    return 0;
  }
}
'''


@unittest.skipUnless(sys.platform == 'darwin', 'macOS Security.framework')
class PrivilegedGenerationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if os.geteuid() == 0:
            raise unittest.SkipTest('Fixtures must run as an ordinary user')
        cls.build_directory = tempfile.TemporaryDirectory(prefix='xenon-generation-build-')
        cls.build = Path(cls.build_directory.name).resolve()
        cls.binary = cls.build / 'generation-fixture'
        source = cls.build / 'fixture.mm'
        source.write_text(HARNESS)
        subprocess.run([
            'xcrun', 'clang++', '-std=c++20', '-fobjc-arc', '-fblocks',
            '-mmacosx-version-min=13.0', '-Wall', '-Wextra', '-Werror',
            '-I', str(ROOT), str(source),
            str(UPDATER / 'xenon_update_mac_privileged_generation.mm'),
            '-framework', 'Foundation', '-framework', 'Security',
            '-o', str(cls.binary),
        ], check=True)
        cls.sign(cls.binary, 'com.xenon.test.generation')

    @staticmethod
    def sign(binary, identifier):
        subprocess.run(['codesign', '--force', '--sign', '-', '--identifier',
                        identifier, str(binary)], check=True, capture_output=True)

    @classmethod
    def tearDownClass(cls):
        cls.build_directory.cleanup()

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='xenon-generation-')
        self.root = Path(self.directory.name).resolve()
        self.app = self.root / 'Fixture.app'
        self.contents = self.app / 'Contents'
        self.helpers = self.contents / 'Helpers'
        self.launchd = self.contents / 'Library/LaunchDaemons'
        self.helpers.mkdir(parents=True)
        self.launchd.mkdir(parents=True)
        self.info = self.contents / 'Info.plist'
        self.write_version('1.0.0.1')
        self.plist = self.launchd / PLIST
        shutil.copyfile(UPDATER / PLIST, self.plist)
        self.helper = self.helpers / HELPER
        shutil.copy2(self.binary, self.helper)

    def tearDown(self):
        self.directory.cleanup()

    def write_version(self, version):
        self.info.write_bytes(plistlib.dumps({
            'CFBundleIdentifier': 'com.xenon.test.generation',
            'CFBundleShortVersionString': version,
        }))

    def read(self, mode='static', app=None, extra=()):
        # Execute only the command-line test fixture; never NSWorkspace/open.
        result = subprocess.run([str(self.helper), str(app or self.app), mode,
                                 *map(str, extra)], check=True, capture_output=True)
        return json.loads(result.stdout)

    def test_static_repeat_and_running_code_match(self):
        expected = self.read()['generation']
        self.assertRegex(expected, r'^[0-9a-f]{64}$')
        self.assertEqual(expected, self.read()['generation'])
        self.assertEqual(expected, self.read('running')['generation'])

    def test_version_plist_and_inode_each_change_generation(self):
        initial = self.read()['generation']
        self.write_version('1.0.0.2')
        changed_version = self.read()['generation']
        self.assertNotEqual(initial, changed_version)
        self.plist.write_bytes(self.plist.read_bytes() + b'\n')
        changed_plist = self.read()['generation']
        self.assertNotEqual(changed_version, changed_plist)
        clone = self.root / 'Clone.app'
        shutil.copytree(self.app, clone)
        self.assertNotEqual(changed_plist, self.read(app=clone)['generation'])

    def test_running_image_cannot_adopt_replacement_code_hash(self):
        replacement = self.helpers / 'replacement'
        shutil.copy2(self.binary, replacement)
        self.sign(replacement, 'com.xenon.test.generation.changed')
        result = self.read('replace', extra=[replacement])
        self.assertIsNotNone(result['before'])
        self.assertIsNotNone(result['static_after'])
        self.assertNotEqual(result['before'], result['static_after'])
        # Security may reject the changed disk image outright; either outcome
        # must avoid reporting its new hash as the currently executing helper.
        self.assertIn(result['running_after'], (None, result['before']))

    def test_reject_symlinked_or_hardlinked_plist(self):
        saved = self.root / 'saved.plist'
        self.plist.rename(saved)
        self.plist.symlink_to(saved)
        self.assertIsNone(self.read()['generation'])
        self.plist.unlink()
        os.link(saved, self.plist)
        self.assertIsNone(self.read()['generation'])

    def test_reject_oversized_plist_and_alias_directory(self):
        self.plist.write_bytes(b'X' * (64 * 1024 + 1))
        self.assertIsNone(self.read()['generation'])
        shutil.copyfile(UPDATER / PLIST, self.plist)
        alias = self.root / 'alias.app'
        alias.symlink_to(self.app, target_is_directory=True)
        self.assertIsNone(self.read(app=alias)['generation'])
        moved = self.contents / 'RealLaunchDaemons'
        self.launchd.rename(moved)
        self.launchd.symlink_to(moved, target_is_directory=True)
        self.assertIsNone(self.read()['generation'])

    def test_reject_missing_version_or_unsigned_helper(self):
        self.info.write_bytes(plistlib.dumps({'CFBundleVersion': '1'}))
        self.assertIsNone(self.read()['generation'])
        self.write_version('1.0.0.1')
        # The runner stays signed. Fingerprint a different bundle whose helper
        # is deliberately unsigned, without executing that file.
        clone = self.root / 'Unsigned.app'
        shutil.copytree(self.app, clone)
        subprocess.run(['codesign', '--remove-signature',
                        str(clone / 'Contents/Helpers' / HELPER)], check=True)
        self.assertIsNone(self.read(app=clone)['generation'])


if __name__ == '__main__':
    unittest.main()
