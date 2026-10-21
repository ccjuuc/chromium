#!/usr/bin/env python3
# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Exercise real GN branding/action generation without compiling or networking."""

import base64
import json
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

from validate_xenon_branding import validate

ROOT = pathlib.Path(__file__).resolve().parents[2]
PIN = base64.b64encode(bytes(range(32))).decode('ascii')


class ConfigurationTest(unittest.TestCase):

    def test_https_and_optional_team(self):
        validate('https://updates.example.test/omaha', '', PIN)
        validate('https://updates.example.test/omaha', 'AB12CD34EF', PIN)

    def test_http_is_explicit_and_loopback_only(self):
        for host in ('localhost', '127.0.0.1', '[::1]'):
            url = f'http://{host}:8999/omaha'
            with self.assertRaises(ValueError):
                validate(url, '', PIN)
            validate(url, '', PIN, True)
        for url in ('http://example.test/omaha', 'http://127.0.0.1.evil.test/',
                    'http://localhost@evil.test/', 'ftp://localhost/'):
            with self.subTest(url=url), self.assertRaises(ValueError):
                validate(url, '', PIN, True)

    def test_invalid_configuration_is_rejected(self):
        for url in ('', 'https://', 'https://host:0/', 'https://host:99999/',
                    'https://user:password@host/', 'https://host/#fragment',
                    'https://host/\n', 'https://host/"', 'https://host/\\'):
            with self.subTest(url=url), self.assertRaises(ValueError):
                validate(url, '', PIN)
        for pin in ('', 'abcd', PIN + '=', base64.b64encode(b'x' * 31).decode()):
            with self.subTest(pin=pin), self.assertRaises(ValueError):
                validate('https://example.test/', '', pin)
        for team in ('PLACEHOLDER', 'abc123', 'ABCDEFGHIJK', 'abcd123456'):
            with self.subTest(team=team), self.assertRaises(ValueError):
                validate('https://example.test/', team, PIN)


class BrandingGenerationTest(unittest.TestCase):

    def setUp(self):
        gn = ROOT / 'buildtools' / ('mac' if sys.platform == 'darwin' else 'linux64') / 'gn'
        if not gn.is_file():
            self.skipTest('GN binary unavailable')
        self.gn = str(gn)
        temporary = tempfile.TemporaryDirectory(prefix='xenon-branding-test-')
        self.addCleanup(temporary.cleanup)
        self.root = pathlib.Path(temporary.name)
        for relative in (
                'chrome/updater/branding.gni',
                'chrome/updater/branding_xenon.gni',
                'chrome/updater/xenon_updater.gni',
                'chrome/updater/apply_updater_branding.gni',
                'chrome/updater/updater_branding.h.in',
                'chrome/updater/validate_xenon_branding.py',
                'chrome/VERSION', 'build/util/process_version.gni',
                'build/util/lastchange.gni', 'build/util/version.py',
                'build/util/android_chrome_version.py',
                'build/util/LASTCHANGE.dummy'):
            destination = self.root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / relative, destination)
        (self.root / '.gn').write_text('buildconfig = "//BUILDCONFIG.gn"\n')
        (self.root / 'BUILDCONFIG.gn').write_text('''
declare_args() {
  is_mac = true
  is_chrome_branded = false
  is_official_build = false
}
set_default_toolchain("//:default")
''')
        config = self.root / 'build/config/chrome_build.gni'
        config.parent.mkdir(parents=True)
        config.write_text('# Stub only the unrelated Chromium global build config.\n')
        self.build = '''
import("//chrome/updater/apply_updater_branding.gni")
toolchain("default") {
  tool("stamp") { command = "touch {{output}}" }
}
apply_updater_branding("branding") {
  process_only = true
  template_file = "//chrome/updater/updater_branding.h.in"
  output = "$root_build_dir/updater_branding.h"
}
group("all") { deps = [ ":branding" ] }
'''

    def generate(self, args='', success=True):
        (self.root / 'BUILD.gn').write_text(self.build)
        result = subprocess.run([self.gn, 'gen', 'out', '--args=' + args],
                                cwd=self.root, capture_output=True, text=True)
        if success:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0)
        return result

    def run_action(self, target):
        result = subprocess.run([self.gn, 'desc', 'out', target, '--format=json'],
                                cwd=self.root, capture_output=True, text=True,
                                check=True)
        action = next(iter(json.loads(result.stdout).values()))
        for dependency in action.get('deps', []):
            self.run_action(dependency)
        subprocess.run([sys.executable, str(self.root / action['script'][2:]),
                        *action['args']], cwd=self.root / 'out', check=True,
                       capture_output=True, text=True)
        return action

    def xenon_args(self, extra=''):
        return ('enable_xenon_updater=true '
                'xenon_updater_update_url="https://updates.example.test/omaha" '
                f'xenon_updater_crx_public_key_hash="{PIN}" ' + extra)

    def test_generated_xenon_identity_and_no_google_services(self):
        self.generate(self.xenon_args('xenon_updater_mac_team_id="AB12CD34EF"'))
        action = self.run_action('//:branding')
        self.assertTrue(any('xenon_branding_validation' in dep
                            for dep in action['deps']))
        header = (self.root / 'out/updater_branding.h').read_text()
        for definition in ('UPDATER_IS_XENON_BRANDED 1',
                           'XENON_UPDATER_ALLOW_INSECURE_LOCALHOST 0',
                           'PRODUCT_FULLNAME_STRING "XenonUpdater"',
                           'COMPANY_SHORTNAME_STRING "Xenon"',
                           'BROWSER_APPID "org.chromium.Chromium"',
                           'MAC_BUNDLE_IDENTIFIER_STRING "com.xenon.XenonUpdater"',
                           'MAC_TEAM_IDENTIFIER_STRING "AB12CD34EF"',
                           'CRASH_UPLOAD_URL ""', 'UPDATER_EVENT_LOGGING_URL ""'):
            self.assertIn('#define ' + definition, header)
        self.assertNotIn('google.com', header)
        self.assertNotIn('googleapis.com', header)
        self.assertIn(f'#define CRX_PKHASH "{PIN}"', header)

    def test_local_http_macro_requires_explicit_opt_in(self):
        self.generate('enable_xenon_updater=true '
                      'xenon_updater_update_url="http://127.0.0.1:8999/omaha" '
                      f'xenon_updater_crx_public_key_hash="{PIN}" '
                      'xenon_updater_allow_insecure_localhost=true')
        self.run_action('//:branding')
        self.assertIn('#define XENON_UPDATER_ALLOW_INSECURE_LOCALHOST 1',
                      (self.root / 'out/updater_branding.h').read_text())

    def test_validation_action_rejects_http_without_opt_in(self):
        self.generate('enable_xenon_updater=true '
                      'xenon_updater_update_url="http://127.0.0.1:8999/omaha" '
                      f'xenon_updater_crx_public_key_hash="{PIN}"')
        with self.assertRaises(subprocess.CalledProcessError):
            self.run_action('//:branding')
        self.assertFalse((self.root / 'out/updater_branding.h').exists())

    def test_default_chromium_and_windows_are_unchanged(self):
        for mac in ('true', 'false'):
            self.generate('is_mac=' + mac)
            self.run_action('//:branding')
            header = (self.root / 'out/updater_branding.h').read_text()
            self.assertIn('#define UPDATER_IS_XENON_BRANDED 0', header)
            self.assertIn('#define PRODUCT_FULLNAME_STRING "ChromiumUpdater"', header)
            self.assertIn('https://omaha-qa.sandbox.google.com/service/update2/json', header)

    def test_missing_settings_and_unsupported_brands_fail_at_generation(self):
        cases = ('enable_xenon_updater=true',
                 'enable_xenon_updater=true xenon_updater_update_url="https://host/"',
                 self.xenon_args('is_mac=false'),
                 self.xenon_args('is_chrome_branded=true'),
                 self.xenon_args('xenon_updater_mac_team_id="bad\\\"value"'))
        for args in cases:
            with self.subTest(args=args):
                self.generate(args, success=False)

    def test_disabled_opt_in_keeps_google_brand_selection(self):
        # The proprietary branding file is not present in public checkouts.
        # A sentinel verifies that the original import remains selected.
        branding = self.root / 'chrome/updater/internal/branding_google.gni'
        branding.parent.mkdir()
        branding.write_text('updater_product_full_name = "Google sentinel"\n')
        self.build = '''
import("//chrome/updater/branding.gni")
assert(updater_product_full_name == "Google sentinel")
assert(updater_is_xenon_branded == 0)
assert(updater_xenon_allow_insecure_localhost == 0)
toolchain("default") { tool("stamp") { command = "touch {{output}}" } }
group("all") {}
'''
        self.generate('is_chrome_branded=true '
                      'xenon_updater_allow_insecure_localhost=true')

    def test_helper_bundle_identity_uses_the_independent_helper_id(self):
        plist = (ROOT / 'chrome/updater/mac/privileged_helper/Info.plist').read_text()
        self.assertIn('<string>${UPDATER_HELPER_BUNDLE_ID}</string>', plist)
        self.assertNotIn('${BROWSER_BUNDLE_IDENTIFIER}.UpdaterPrivilegedHelper', plist)


if __name__ == '__main__':
    unittest.main()
