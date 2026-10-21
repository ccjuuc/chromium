#!/usr/bin/env python3
# Copyright 2026 The Xenon Overlay Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Embed the checked-in Chromium installer, never a script from an update."""

import argparse
from pathlib import Path


def generate(source: str) -> str:
    # read_text normalizes checkout line endings before embedding a shell file.
    if source.count('\nUPDATE_VERSION=\n') != 1:
        raise ValueError('Expected one empty UPDATE_VERSION assignment')
    if not source.startswith('#!/bin/bash -p\n'):
        raise ValueError('Unexpected Chromium installer interpreter')
    delimiter = 'XENON_INSTALL'
    if f'){delimiter}"' in source:
        raise ValueError('Installer contains the C++ raw string delimiter')
    return (
        '// Generated from chrome/updater/mac/.install.sh. Do not edit.\n'
        '#ifndef XENON_OVERLAY_CHROME_BROWSER_UPDATER_XENON_MAC_INSTALL_SCRIPT_H_\n'
        '#define XENON_OVERLAY_CHROME_BROWSER_UPDATER_XENON_MAC_INSTALL_SCRIPT_H_\n'
        '\nnamespace xenon::updater {\n'
        f'inline constexpr char kMacInstallScript[] = R"{delimiter}('
        f'{source}){delimiter}";\n'
        '}  // namespace xenon::updater\n\n'
        '#endif\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    header = generate(args.input.read_text(encoding='utf-8'))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open('w', encoding='utf-8', newline='\n') as output:
        output.write(header)


if __name__ == '__main__':
    main()
