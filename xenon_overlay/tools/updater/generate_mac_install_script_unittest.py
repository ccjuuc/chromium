#!/usr/bin/env python3
# Copyright 2026 The Xenon Overlay Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import pathlib
import subprocess
import sys
import tempfile
import unittest

import generate_mac_install_script


class GenerateMacInstallScriptTest(unittest.TestCase):
    def test_checked_in_installer_is_embedded_without_behavior_changes(self):
        root = pathlib.Path(__file__).resolve().parents[3]
        source = root / 'chrome/updater/mac/.install.sh'
        with tempfile.TemporaryDirectory() as directory:
            header = pathlib.Path(directory) / 'gen/installer.h'
            subprocess.run([
                sys.executable, generate_mac_install_script.__file__,
                '--input', str(source), '--output', str(header)
            ], check=True)
            embedded = header.read_text(encoding='utf-8').split(
                'R"XENON_INSTALL(', 1)[1].split(')XENON_INSTALL";', 1)[0]
            self.assertEqual(source.read_text(encoding='utf-8'), embedded)

    def test_changed_upstream_version_contract_fails_build(self):
        for source in (
                '#!/bin/bash -p\nUPDATE_VERSION=1.0\n',
                '#!/bin/bash -p\nUPDATE_VERSION=\n\nUPDATE_VERSION=\n',
                '#!/bin/bash\nUPDATE_VERSION=\n'):
            with self.subTest(source=source), self.assertRaises(ValueError):
                generate_mac_install_script.generate(source)

    def test_raw_string_terminator_cannot_inject_header_code(self):
        with self.assertRaises(ValueError):
            generate_mac_install_script.generate(
                '#!/bin/bash -p\nUPDATE_VERSION=\n)XENON_INSTALL";\n')


if __name__ == '__main__':
    unittest.main()
