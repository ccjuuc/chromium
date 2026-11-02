#!/usr/bin/env python3
# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Verify the installer/archive chain against this package's staging manifest."""

import argparse
import hashlib
import json
from pathlib import Path, PureWindowsPath
import subprocess

from import_electron_app import check_ancestors, inventory
from windows_electron_runtime import validate_projection
from windows_installer_resources import packed_resources


ROOT = Path(__file__).resolve().parents[2]


def digest_file(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def archive_members(listing):
    members = []
    for block in listing.strip().split('\n\n'):
        fields = dict(line.split(' = ', 1) for line in block.splitlines()
                      if ' = ' in line)
        if 'Path' not in fields or 'Size' not in fields:
            continue
        name = fields['Path'].replace('\\', '/')
        parts = name.split('/')
        if (parts[0] != 'Chrome-bin' or
                any(part in ('', '.', '..') for part in parts) or
                any(part.rstrip(' .') != part or PureWindowsPath(part).is_reserved()
                    for part in parts) or
                any(char in name for char in (':', '\0', '\r', '\n')) or
                'Symbolic Link' in fields or 'Hard Link' in fields):
            raise ValueError(f'Unsafe archive member: {name}')
        members.append(name)
    if not members or len({name.casefold() for name in members}) != len(members):
        raise ValueError('Archive is empty or has colliding paths')
    return members


def verify_tree(product, manifest):
    if manifest.get('schema_version') != 1 or manifest.get('archive_root') != 'Chrome-bin':
        raise ValueError('Unsupported packaging manifest')
    if inventory(product) != manifest['tree']:
        raise ValueError('Extracted files differ from the packaging manifest')
    configuration = json.loads((ROOT / 'xenon_overlay/resources/windows_hosted_apps.json').read_text())
    known = {item['application']: item['profile'] for item in configuration}
    applications = {app['path']: app for app in manifest['applications']}
    if len(applications) != len(manifest['applications']):
        raise ValueError('Duplicate application in packaging manifest')
    for path, profile in known.items():
        if path in manifest['tree']['directories']:
            if path not in applications or applications[path]['profile'] != profile:
                raise ValueError(f'Missing or incorrect hosted policy for {path}')
    for app in manifest['applications']:
        if app['profile'] is not None:
            # Only manifest paths which are real archive directories can be
            # inspected. Do not let an edited manifest address external code.
            path = Path(app['path'])
            if (path.is_absolute() or path.drive or '..' in path.parts or
                    app['path'] not in manifest['tree']['directories']):
                raise ValueError('Invalid application path in packaging manifest')
            validate_projection(product / path, inventory(product / path), app['profile'])


def verify(build, installer, output, sevenzip=None):
    build, installer, output = (Path(value).absolute() for value in (build, installer, output))
    check_ancestors(output)
    output.mkdir(parents=True, exist_ok=False)
    tool = Path(sevenzip) if sevenzip else ROOT / 'third_party/lzma_sdk/bin/host_platform/7za.exe'
    report = {'passed': False, 'checks': [],
              'scope': 'Archive integrity and embedded payload; does not run or install applications.'}

    def check(name, passed, **details):
        report['checks'].append({'name': name, 'passed': bool(passed), **details})
        if not passed:
            raise ValueError(name)

    def command(label, arguments):
        result = subprocess.run([str(tool), *map(str, arguments)],
                                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT)
        (output / (label + '.log')).write_bytes(result.stdout)
        check(label, result.returncode == 0)
        return result.stdout

    try:
        manifest_path = build / 'xenon_installer_manifest.json'
        manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
        archive = build / 'chrome.7z'
        packed = build / 'chrome.packed.7z'
        listing = command('list-archive', ['l', '-slt', '-ba', '-sccUTF-8', archive])
        archive_members(listing.decode('utf-8').replace('\r\n', '\n'))
        command('extract-archive', ['x', archive, '-o' + str(output / 'extracted'), '-y'])
        verify_tree(output / 'extracted/Chrome-bin', manifest)
        check('all_staged_file_hashes_match', True, files=len(manifest['tree']['files']))
        with (output / 'packed-stream.log').open('wb') as errors:
            with subprocess.Popen([str(tool), 'x', '-so', '-bd', str(packed), 'chrome.7z'],
                                  stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                  stderr=errors) as process:
                digest, size = hashlib.sha256(), 0
                for chunk in iter(lambda: process.stdout.read(8 * 1024 * 1024), b''):
                    digest.update(chunk)
                    size += len(chunk)
                check('packed_archive_roundtrip', process.wait() == 0 and
                      size == archive.stat().st_size and digest.hexdigest() == digest_file(archive))
        resources = packed_resources(installer)
        check('installer_embedded_archive_matches', len(resources) == 1 and
              resources[0]['bytes'] == packed.stat().st_size and
              resources[0]['sha256'] == digest_file(packed))
        report.update(passed=True, manifest_sha256=digest_file(manifest_path),
                      installer={'path': str(installer), 'bytes': installer.stat().st_size,
                                 'sha256': digest_file(installer)},
                      applications=manifest['applications'])
    except (OSError, ValueError, KeyError, TypeError) as error:
        report['error'] = str(error)
    finally:
        (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'out/Release_64')
    parser.add_argument('--installer', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True, help='New evidence directory')
    parser.add_argument('--sevenzip', type=Path)
    args = parser.parse_args()
    try:
        report = verify(args.build_dir, args.installer, args.output_dir, args.sevenzip)
    except (OSError, ValueError) as error:
        parser.exit(1, f'error: {error}\n')
    print(json.dumps(report))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
