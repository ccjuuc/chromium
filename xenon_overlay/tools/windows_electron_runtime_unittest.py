#!/usr/bin/env python3
# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

from pathlib import Path
import struct
import tempfile
import unittest

from import_electron_app import inventory
import windows_electron_runtime as policy


def pe_image(imported=None, delay=False):
    data = bytearray(1024)
    data[:2] = b'MZ'
    struct.pack_into('<I', data, 0x3c, 0x80)
    data[0x80:0x84] = b'PE\0\0'
    struct.pack_into('<H', data, 0x86, 1)
    struct.pack_into('<H', data, 0x94, 240)
    struct.pack_into('<H', data, 0x98, 0x20b)
    struct.pack_into('<Q', data, 0x98 + 24, 0x180000000)
    struct.pack_into('<I', data, 0x98 + 108, 16)
    struct.pack_into('<IIII', data, 0x98 + 240 + 8, 512, 0x1000, 512, 512)
    if imported:
        entry, width = (13, 32) if delay else (1, 20)
        struct.pack_into('<II', data, 0x98 + 112 + 8 * entry, 0x1000, width * 2)
        if delay:
            struct.pack_into('<II', data, 512, 1, 0x1080)
        else:
            struct.pack_into('<I', data, 512 + 12, 0x1080)
        name = imported.encode('ascii') + b'\0'
        data[640:640 + len(name)] = name
    return data


class RuntimePolicyTest(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)

    def write(self, name, data):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return path

    def test_projection_preserves_identity_business_code_and_private_libraries(self):
        for name in ('XDASKernel.dll', 'locales/zh-CN.pak', 'resources.pak'):
            self.write(name, b'redundant runtime')
        retained = ('Thunder.exe', 'resources/app/out.asar', 'thunder_helper.node',
                    'player/vulkan-1.dll', 'LICENSES.chromium.html', 'd3dcompiler_47.dll')
        for name in retained:
            self.write(name, pe_image() if name.endswith(('.exe', '.dll', '.node')) else b'opaque')
        original = inventory(self.root)
        projected = policy.project_inventory(original)
        self.assertEqual({entry['path'] for entry in projected['files']}, set(retained))
        policy.validate_projection(self.root, projected)
        self.assertEqual(inventory(self.root), original)

    def test_reintroduced_runtime_is_rejected(self):
        self.write('xdaskernel.DLL', pe_image())
        with self.assertRaisesRegex(ValueError, 'runtime remains'):
            policy.validate_projection(self.root, inventory(self.root))

    def test_regular_and_delay_imports_prevent_unsafe_removal(self):
        for delayed in (False, True):
            self.write('addon.node', pe_image('XDASKernel.dll', delayed))
            with self.assertRaisesRegex(ValueError, 'needs removed runtime'):
                policy.validate_projection(self.root, inventory(self.root))

    def test_private_player_dependency_is_preserved(self):
        self.write('player/codec.dll', pe_image('vulkan-1.dll'))
        self.write('player/vulkan-1.dll', pe_image())
        policy.validate_projection(self.root, inventory(self.root))
        (self.root / 'player/vulkan-1.dll').unlink()
        with self.assertRaisesRegex(ValueError, 'needs removed runtime'):
            policy.validate_projection(self.root, inventory(self.root))

    def test_malformed_image_does_not_pass_validation(self):
        self.write('addon.node', b'MZ')
        with self.assertRaisesRegex(ValueError, 'Malformed'):
            policy.validate_projection(self.root, inventory(self.root))

    def test_ple_keeps_required_root_vulkan_and_removes_debug_log(self):
        self.write('libplacebo-274.dll', pe_image('vulkan-1.dll'))
        self.write('vulkan-1.dll', pe_image())
        self.write('XDASKernel.dll', pe_image())
        self.write('debug.log', b'packaged diagnostic log')
        self.write('libEGL.dll', pe_image())
        self.write('libGLESv2.dll', pe_image())
        projected = policy.project_inventory(inventory(self.root), 'ple')
        self.assertEqual({entry['path'] for entry in projected['files']},
                         {'libplacebo-274.dll', 'vulkan-1.dll',
                          'libEGL.dll', 'libGLESv2.dll'})
        policy.validate_projection(self.root, projected, 'ple')
        (self.root / 'vulkan-1.dll').unlink()
        with self.assertRaisesRegex(ValueError, 'needs removed runtime'):
            policy.validate_projection(
                self.root, policy.project_inventory(inventory(self.root), 'ple'), 'ple')

    def test_profile_does_not_silently_accept_unknown_values(self):
        with self.assertRaisesRegex(ValueError, 'Unknown'):
            policy.excluded('XDASKernel.dll', 'unknown')


if __name__ == '__main__':
    unittest.main()
