#!/usr/bin/env python3
# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from unittest import mock

from import_electron_app import inventory
import package_windows as package
import verify_windows_installer as verifier
from windows_electron_runtime_unittest import pe_image
from windows_installer_resources import packed_resources


def resource_image(payload):
    data = bytearray(pe_image())
    length = max(512, 256 + len(payload))
    data.extend(b'\0' * max(0, 512 + length - len(data)))
    struct.pack_into('<IIII', data, 0x98 + 240 + 8, length, 0x1000, length, 512)
    struct.pack_into('<II', data, 0x98 + 112 + 16, 0x1000, length)
    for relative, named, numbered in ((0, 1, 0), (32, 1, 0), (64, 0, 1)):
        struct.pack_into('<HH', data, 512 + relative + 12, named, numbered)
    struct.pack_into('<II', data, 512 + 16, 0x80000080, 0x80000020)
    struct.pack_into('<II', data, 512 + 48, 0x80000090, 0x80000040)
    struct.pack_into('<II', data, 512 + 80, 1033, 96)
    struct.pack_into('<IIII', data, 512 + 96, 0x1100, len(payload), 0, 0)
    for offset, text in ((128, 'B7'), (144, 'CHROME.PACKED.7Z')):
        struct.pack_into('<H', data, 512 + offset, len(text))
        data[514 + offset:514 + offset + len(text) * 2] = text.encode('utf-16le')
    data[768:768 + len(payload)] = payload
    return data


BODY = '''target_output_name = fixture_installer
build packed_files.res: rc packed_files.rc || phony/archive
build ./fixture_installer.exe ./fixture_installer.exe.pdb: link packed_files.res fixture.obj | fixture.lib || phony/browser
  output_dir = .
  output_extension = .exe
'''
TOOLCHAIN = '''rule rc
  command = resource-compiler $in
rule link
  command = linker $in
  pool = build_toolchain_link_pool
rule archive
  command = H$:/python3.exe ../../xenon_overlay/tools/create_xenon_installer_archive.py --build_dir . --xenon-packaging-manifest=xenon_installer_manifest.json
'''


class PackagingTest(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix='windows-packaging-')
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.build = self.root / 'build'
        self.build.mkdir()

    def test_repack_keeps_real_object_inputs_and_rejects_new_build_stages(self):
        graph = package.repack_graph(TOOLCHAIN, BODY)
        self.assertIn('fixture.obj | fixture.lib', graph)
        self.assertNotIn('phony/browser', graph)
        self.assertIn('pool build_toolchain_link_pool', graph)
        with self.assertRaisesRegex(ValueError, 'exactly'):
            package.repack_graph(TOOLCHAIN, BODY + 'build extra: cxx extra.cc\n')
        with self.assertRaisesRegex(ValueError, 'includes'):
            package.repack_graph(TOOLCHAIN, 'subninja other.ninja\n' + BODY)

    def test_archive_action_rejects_stale_or_ambiguous_rules(self):
        self.assertTrue(package.archive_command(TOOLCHAIN).startswith('H:/python3.exe '))
        for text in (TOOLCHAIN + TOOLCHAIN,
                     TOOLCHAIN.replace('--xenon-packaging-manifest=', '--old='),
                     TOOLCHAIN.replace('H$:/python3.exe', 'cmd.exe /c')):
            with self.assertRaises(ValueError):
                package.archive_command(text)

    def test_generated_target_cannot_escape_build_directory(self):
        path = self.build / 'obj/chrome/installer/mini_installer/mini_installer.ninja'
        path.parent.mkdir(parents=True)
        path.write_text(BODY.replace('output_dir = .', 'output_dir = ../outside'))
        with self.assertRaisesRegex(ValueError, 'inside'):
            package.generated_installer(self.build)

    def test_repack_restores_previous_installer_on_link_failure(self):
        path = self.build / 'obj/chrome/installer/mini_installer/mini_installer.ninja'
        path.parent.mkdir(parents=True)
        path.write_text(BODY)
        (self.build / 'args.gn').write_text('is_win = true')
        (self.build / 'toolchain.ninja').write_text(TOOLCHAIN)
        installer = self.build / 'fixture_installer.exe'
        installer.write_bytes(b'previous verified installer')

        def run(command, **kwargs):
            if isinstance(command, list) and command[-1] == 'fixture_installer.exe':
                installer.write_bytes(b'incomplete link')
                raise subprocess.CalledProcessError(1, command)
            return subprocess.CompletedProcess(command, 0)

        with mock.patch.object(package.subprocess, 'run', side_effect=run), \
                mock.patch.object(package.sys, 'platform', 'win32'):
            report = package.package(self.build, self.root / 'repack', 'repack')
        self.assertFalse(report['passed'])
        self.assertTrue(report['previous_installer_restored'])
        self.assertEqual(installer.read_bytes(), b'previous verified installer')

    def test_verify_only_never_invokes_gn_or_build_tools(self):
        path = self.build / 'obj/chrome/installer/mini_installer/mini_installer.ninja'
        path.parent.mkdir(parents=True)
        path.write_text(BODY)
        (self.build / 'args.gn').write_text('fixture')
        with mock.patch.object(package.subprocess, 'run', side_effect=AssertionError('build invoked')), \
                mock.patch.object(package, 'verify', return_value={'passed': True, 'installer': {}}):
            self.assertTrue(package.package(self.build, self.root / 'verify', 'verify')['passed'])

    def test_unsafe_archive_names_are_rejected_before_extraction(self):
        for name in ('../escape', 'Chrome-bin/../escape', 'Chrome-bin/.. /escape',
                     'Chrome-bin/file:stream', 'Chrome-bin/CON', '/Chrome-bin/file'):
            with self.subTest(name=name), self.assertRaises(ValueError):
                verifier.archive_members(f'Path = {name}\nSize = 1')
        with self.assertRaises(ValueError):
            verifier.archive_members('Path = Chrome-bin/file\nSize = 1\n\n'
                                     'Path = Chrome-bin/FILE\nSize = 1')

    def test_resource_hash_matches_embedded_bytes_and_rejects_cycles(self):
        path = self.build / 'installer.exe'
        payload = b'compressed archive bytes'
        path.write_bytes(resource_image(payload))
        self.assertEqual(packed_resources(path)[0]['sha256'], hashlib.sha256(payload).hexdigest())
        damaged = bytearray(path.read_bytes())
        struct.pack_into('<I', damaged, 512 + 20, 0x80000000)
        path.write_bytes(damaged)
        with self.assertRaisesRegex(ValueError, 'Cyclic'):
            packed_resources(path)

    def test_resource_data_outside_file_is_rejected(self):
        path = self.build / 'installer.exe'
        data = resource_image(b'archive')
        struct.pack_into('<I', data, 512 + 96, 0xFFFF0000)
        path.write_bytes(data)
        with self.assertRaises(ValueError):
            packed_resources(path)

    def test_manifest_cannot_disable_known_hosted_cleanup(self):
        product = self.build / 'Chrome-bin'
        app = product / 'xenon_player'
        app.mkdir(parents=True)
        (app / 'XDASKernel.dll').write_bytes(b'leaked runtime')
        manifest = {'schema_version': 1, 'archive_root': 'Chrome-bin',
                    'applications': [{'path': 'xenon_player', 'profile': None}],
                    'tree': inventory(product)}
        with self.assertRaisesRegex(ValueError, 'hosted policy'):
            verifier.verify_tree(product, manifest)

    def test_real_archive_and_installer_roundtrip_and_tamper_detection(self):
        tool = verifier.ROOT / 'third_party/lzma_sdk/bin/host_platform/7za.exe'
        if not tool.is_file():
            self.skipTest('bundled 7za is unavailable')
        source = self.root / 'staging/Chrome-bin'
        source.mkdir(parents=True)
        (source / 'fixture.bin').write_bytes(b'product file\0\xff')
        manifest = {'schema_version': 1, 'archive_root': 'Chrome-bin',
                    'applications': [], 'tree': inventory(source)}
        manifest_path = self.build / 'xenon_installer_manifest.json'
        manifest_path.write_text(json.dumps(manifest))
        for archive, input_path in ((self.build / 'chrome.7z', source),
                                    (self.build / 'chrome.packed.7z', self.build / 'chrome.7z')):
            subprocess.run([str(tool), 'a', '-t7z', str(archive), str(input_path), '-mx0'],
                           check=True, capture_output=True)
        installer = self.build / 'fixture.exe'
        installer.write_bytes(resource_image((self.build / 'chrome.packed.7z').read_bytes()))
        self.assertTrue(verifier.verify(self.build, installer, self.root / 'good')['passed'])
        manifest['tree']['files'][0]['sha256'] = '0' * 64
        manifest_path.write_text(json.dumps(manifest))
        self.assertFalse(verifier.verify(self.build, installer, self.root / 'tampered')['passed'])


if __name__ == '__main__':
    unittest.main()
