#!/usr/bin/env python3
# Copyright 2026 The Xenon Overlay Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import json
from pathlib import Path
import shutil
import struct
import tempfile
import unittest

import asar_sync_support as sync


# Public half of an independently generated test key. No private fixture key.
PUBLIC_KEY = b'''-----BEGIN PUBLIC KEY-----
MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAmx91yb6LlcAIfTUy+aGu
k5OiL/ESFc50VuRdat0AlD0Zj9R/PuBdFFf0ytzq/e2komUD2O6bI4vv7+UOKqPf
SpIKIdyzRNxmgrqmtvrey5Yt2aUhnEfMZyLgpL9Jtvk4UU96x1Xjy09nQD5Hafr3
avwJqraXZu2o+KWLv0QoZF+4eYe/SBZaoYp44+JSsrp/ga37Exv/ZJQsAmqNF93u
iaHD/xQ16QqN4zc+7bYN1jRYuUcm6UAujbIhyA9M3R/q/vgn4/vxG50Q8T4kexPW
/6QA85WKOpy0MLQhQLOzBg8wF+PWF9NmsBO9TK09LEC41bvBHgDChPohoLlVaiGD
tQIDAQAB
-----END PUBLIC KEY-----
'''
RSA_PUBLIC_KEY = b'''-----BEGIN RSA PUBLIC KEY-----
MIIBCgKCAQEAmx91yb6LlcAIfTUy+aGuk5OiL/ESFc50VuRdat0AlD0Zj9R/PuBd
FFf0ytzq/e2komUD2O6bI4vv7+UOKqPfSpIKIdyzRNxmgrqmtvrey5Yt2aUhnEfM
ZyLgpL9Jtvk4UU96x1Xjy09nQD5Hafr3avwJqraXZu2o+KWLv0QoZF+4eYe/SBZa
oYp44+JSsrp/ga37Exv/ZJQsAmqNF93uiaHD/xQ16QqN4zc+7bYN1jRYuUcm6UAu
jbIhyA9M3R/q/vgn4/vxG50Q8T4kexPW/6QA85WKOpy0MLQhQLOzBg8wF+PWF9Nm
sBO9TK09LEC41bvBHgDChPohoLlVaiGDtQIDAQAB
-----END RSA PUBLIC KEY-----
'''


def standard_asar(index=None):
    header = json.dumps({'files': {}} if index is None else index).encode()
    padded = header + b'\0' * (-len(header) % 4)
    pickle = struct.pack('<II', len(padded) + 4, len(header)) + padded
    return struct.pack('<II', 4, len(pickle)) + pickle


class AsarSyncPreflightTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='asar-sync-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.key = self.root / 'rsa-pub.pem'

    @unittest.skipUnless(shutil.which('node'), 'Node.js public key validation')
    def test_accepts_only_supported_public_key_formats(self):
        for pem in (PUBLIC_KEY, RSA_PUBLIC_KEY):
            with self.subTest(pem_type=pem.splitlines()[0]):
                self.key.write_bytes(pem)
                self.assertEqual(sync.read_public_key(self.key), pem)

    def test_rejects_private_combined_and_malformed_pem(self):
        for pem in (
                b'-----BEGIN PRIVATE KEY-----\nAAAA\n-----END PRIVATE KEY-----',
                PUBLIC_KEY + b'-----BEGIN RSA PRIVATE KEY-----\nsecret',
                PUBLIC_KEY + PUBLIC_KEY,
                b'-----BEGIN CERTIFICATE-----\nAAAA\n-----END CERTIFICATE-----',
                PUBLIC_KEY.replace(b'MIIB', b'????'),
                b'x' * (16 * 1024 + 1)):
            with self.subTest(size=len(pem)):
                self.key.write_bytes(pem)
                with self.assertRaises(ValueError):
                    sync.read_public_key(self.key)

    @unittest.skipUnless(shutil.which('node'), 'Node.js public key validation')
    def test_valid_base64_must_contain_an_rsa_public_key(self):
        self.key.write_bytes(
            b'-----BEGIN PUBLIC KEY-----\nAAAA\n-----END PUBLIC KEY-----\n')
        with self.assertRaisesRegex(ValueError, 'invalid RSA-2048'):
            sync.read_public_key(self.key)

    def test_standard_archive_needs_no_key(self):
        archive = self.root / 'app.asar'
        archive.write_bytes(standard_asar())
        self.assertTrue(sync.is_standard_asar(archive))
        self.assertIsNone(sync.preflight_asar_public_key(
            [self.root], None, self.key))

    def test_nonstandard_archive_without_key_fails(self):
        archive = self.root / 'app.asar'
        archive.write_bytes(b'opaque archive bytes')
        with self.assertRaisesRegex(ValueError, 'requires --asar-public-key'):
            sync.preflight_asar_public_key([self.root], None, self.key)

    def test_clear_index_with_encrypted_members_still_requires_key(self):
        archive = self.root / 'app.asar'
        archive.write_bytes(standard_asar({'files': {
            'nested': {'files': {'main.js': {'encrypted': True, 'size': 0}}}}}))
        with self.assertRaisesRegex(ValueError, 'requires --asar-public-key'):
            sync.preflight_asar_public_key([self.root], None, self.key)

    def test_truncated_and_oversized_headers_are_not_standard(self):
        archive = self.root / 'app.asar'
        for payload in (standard_asar()[:-1],
                        struct.pack('<IIII', 4, 0xffffffff, 0xfffffffb, 0),
                        b''):
            archive.write_bytes(payload)
            self.assertFalse(sync.is_standard_asar(archive))

    def test_explicit_missing_key_never_falls_back(self):
        with self.assertRaises(OSError):
            sync.preflight_asar_public_key([], self.key, self.root / 'default.pem')


if __name__ == '__main__':
    unittest.main()
