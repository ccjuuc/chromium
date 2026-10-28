# Copyright 2026 The Xenon Overlay Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import importlib.util
from pathlib import Path
import plistlib
import tempfile
import unittest
from unittest import mock


SPEC = importlib.util.spec_from_file_location(
    'strip_electron_runtime', Path(__file__).with_name('strip_electron_runtime.py'))
strip = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(strip)
MACHO = b'\xcf\xfa\xed\xfe fixture native bytes'


class RuntimePruningTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='xenon-strip-test-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.bundle = self.root / 'Thunder.app'
        self.contents = self.bundle / 'Contents'
        self.info = {
            'CFBundleIdentifier': 'com.xunlei.Thunder',
            'CFBundleName': 'Thunder', 'CFBundleVersion': '25.1.13',
            'CFBundlePackageType': 'APPL', 'CFBundleExecutable': 'Thunder',
            'NSPrincipalClass': 'AtomApplication', 'CFBundleIconFile': 'Thunder.icns',
        }
        self.write('Contents/Info.plist', plistlib.dumps(self.info))
        self.write('Contents/MacOS/Thunder', MACHO)
        self.write('Contents/Resources/app/out.asar', b'original ASAR bytes')
        self.write('Contents/Resources/app/thunder_helper.node', MACHO)
        self.write('Contents/Resources/app/playercontrol.node', MACHO)
        self.write('Contents/Resources/Thunder.icns', b'TH icon')
        self.write('Contents/Resources/electron.icns', b'Electron icon')
        self.write('Contents/PkgInfo', b'APPL????')
        self.write('Contents/_CodeSignature/CodeResources', b'old root signature')
        for framework, identity in strip.RUNTIME_FRAMEWORKS.items():
            self.write(f'Contents/Frameworks/{framework}/Resources/Info.plist',
                       plistlib.dumps({'CFBundleIdentifier': identity}))
            self.write(f'Contents/Frameworks/{framework}/{framework[:-10]}', MACHO)
        self.write('Contents/Frameworks/Electron Framework.framework/'
                   'Libraries/libffmpeg.dylib', MACHO)
        helper = 'Contents/Frameworks/Thunder Helper.app/Contents'
        self.write(f'{helper}/Info.plist', plistlib.dumps({
            'CFBundleIdentifier': 'com.xunlei.Thunder.helper',
            'CFBundleExecutable': 'Thunder Helper'}))
        self.write(f'{helper}/MacOS/Thunder Helper', MACHO)
        for framework in ['MacPlayer', 'MachXPC', 'Sparkle', 'WCDBSwift', 'sqlcipher', 'sqliterk']:
            self.write(f'Contents/Frameworks/{framework}.framework/{framework}', MACHO)
            self.write(f'Contents/Frameworks/{framework}.framework/'
                       '_CodeSignature/CodeResources', b'preserve native signature')
        extension = 'Contents/PlugIns/Thunder Extension.appex/Contents'
        self.write(f'{extension}/MacOS/Thunder Extension', MACHO)
        self.write(f'{extension}/_CodeSignature/CodeResources',
                   b'preserve extension signature')
        self.dependencies = {
            'Thunder': ['@rpath/Electron Framework.framework/Electron Framework'],
            'Thunder Helper': ['@rpath/Electron Framework.framework/Electron Framework'],
            'thunder_helper.node': ['@rpath/Sparkle.framework/Versions/B/Sparkle'],
            'playercontrol.node': ['@rpath/MacPlayer.framework/Versions/A/MacPlayer'],
        }
        patcher = mock.patch.object(
            strip, '_dependencies',
            side_effect=lambda filename: self.dependencies.get(filename.name, []))
        patcher.start()
        self.addCleanup(patcher.stop)

    def write(self, relative, data):
        filename = self.bundle / relative
        filename.parent.mkdir(parents=True, exist_ok=True)
        filename.write_bytes(data)
        return filename

    def snapshot(self):
        return {str(filename.relative_to(self.bundle)): filename.read_bytes()
                for filename in self.bundle.rglob('*') if filename.is_file()}

    def test_prunes_only_runtime_preserves_native_signatures_and_identity(self):
        before = self.snapshot()
        report = strip.strip_runtime(self.bundle, self.root)
        self.assertTrue(report['changed'])
        self.assertFalse(report['independentlyLaunchable'])
        self.assertGreater(report['removedBytes'], 0)
        info = plistlib.loads((self.contents / 'Info.plist').read_bytes())
        self.assertEqual(info['CFBundlePackageType'], 'BNDL')
        self.assertNotIn('CFBundleExecutable', info)
        self.assertNotIn('NSPrincipalClass', info)
        for key in ['CFBundleIdentifier', 'CFBundleName', 'CFBundleVersion', 'CFBundleIconFile']:
            self.assertEqual(info[key], self.info[key])
        self.assertEqual(list((self.contents / 'MacOS').iterdir()), [])
        for relative, data in before.items():
            if relative == 'Contents/Info.plist':
                continue
            removed = any(relative == root or relative.startswith(root + '/')
                          for root in report['removed'])
            if removed:
                self.assertFalse((self.bundle / relative).exists(), relative)
            else:
                self.assertEqual((self.bundle / relative).read_bytes(), data, relative)
        self.assertTrue(any(row['path'].endswith('MacPlayer') for row in report['retainedNative']))

    def test_inspection_and_second_execution_do_not_modify_files(self):
        before = self.snapshot()
        report = strip.strip_runtime(self.bundle, self.root, check_only=True)
        self.assertTrue(report['checkOnly'])
        self.assertEqual(before, self.snapshot())
        strip.strip_runtime(self.bundle, self.root)
        self.write('Contents/_CodeSignature/CodeResources', b'new resource bundle signature')
        before = self.snapshot()
        report = strip.strip_runtime(self.bundle, self.root)
        self.assertFalse(report['changed'])
        self.assertEqual(report['removed'], [])
        self.assertEqual(before, self.snapshot())

    def test_retained_dependency_on_removed_runtime_prevents_all_mutation(self):
        for dependency in ['@rpath/Electron Framework.framework/Electron Framework',
                           '@rpath/Mantle.framework/Mantle', '@rpath/libffmpeg.dylib']:
            with self.subTest(dependency=dependency):
                before = self.snapshot()
                self.dependencies['playercontrol.node'] = [dependency]
                with self.assertRaisesRegex(ValueError, 'requires removed runtime'):
                    strip.strip_runtime(self.bundle, self.root)
                self.assertEqual(before, self.snapshot())

    def test_wrong_staging_root_framework_identity_and_launcher_fail(self):
        before = self.snapshot()
        with self.assertRaisesRegex(ValueError, 'staging root'):
            strip.strip_runtime(self.bundle, self.contents)
        self.dependencies['Thunder'] = []
        with self.assertRaisesRegex(ValueError, 'does not link Electron'):
            strip.strip_runtime(self.bundle, self.root)
        self.assertEqual(before, self.snapshot())
        self.dependencies['Thunder'] = ['@rpath/Electron Framework.framework/Electron Framework']
        self.write('Contents/Frameworks/Mantle.framework/Resources/Info.plist',
                   plistlib.dumps({'CFBundleIdentifier': 'custom.business.framework'}))
        before = self.snapshot()
        with self.assertRaisesRegex(ValueError, 'framework identity'):
            strip.strip_runtime(self.bundle, self.root)
        self.assertEqual(before, self.snapshot())

    def test_escaping_symlink_is_rejected_without_touching_external_content(self):
        outside = self.root / 'keep.txt'
        outside.write_text('keep')
        (self.contents / 'Resources/link').symlink_to(outside)
        with self.assertRaisesRegex(ValueError, 'escapes'):
            strip.strip_runtime(self.bundle, self.root)
        self.assertEqual(outside.read_text(), 'keep')
        self.assertTrue((self.contents / 'MacOS/Thunder').exists())

    def test_failed_final_verification_rolls_back_all_changes(self):
        before = self.snapshot()
        original_hash = strip._hash
        calls = {}

        def fail_on_second_hash(filename):
            calls[filename] = calls.get(filename, 0) + 1
            if calls[filename] > 1:
                return 'mismatch'
            return original_hash(filename)

        with mock.patch.object(strip, '_hash', side_effect=fail_on_second_hash):
            with self.assertRaisesRegex(ValueError, 'Retained native code changed'):
                strip.strip_runtime(self.bundle, self.root)
        self.assertEqual(before, self.snapshot())
        self.assertEqual(list(self.root.glob('.electron-runtime-*')), [])


if __name__ == '__main__':
    unittest.main()
