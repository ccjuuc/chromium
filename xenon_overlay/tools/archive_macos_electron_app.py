#!/usr/bin/env python3
# Copyright 2026 The Xenon Overlay Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Import a pinned official macOS DMG as a deterministic hosted app resource.

This is an offline source-import tool, not a build step. Download the DMG from
the URL recorded in source.json, then pass --app-id and --dmg. The DMG digest
must match the pinned GitHub release asset digest. No vendor JavaScript or
native helper is modified; only the outer Info.plist becomes a resource bundle.
"""

import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import plistlib
import stat
import subprocess
import tarfile
import tempfile


APPS = {
    'drawio': {
        'name': 'draw.io',
        'bundle': 'draw.io.app',
        'version': '32.6.10',
        'repository': 'jgraph/drawio-desktop',
        'dmg': 'draw.io-arm64-32.6.10.dmg',
        'dmg_sha256':
            '83e4027837f89bfba9dafbcc5c0e4befa76e2ac7f4e37f7a7b40c817141a6727',
    },
    'marktext': {
        'name': 'MarkText',
        'bundle': 'MarkText.app',
        'version': '0.23.0',
        'repository': 'marktext/marktext',
        'dmg': 'marktext-mac-arm64-0.23.0.dmg',
        'dmg_sha256':
            'd7e9ef4fc7c7f353003fc3f34d3a56514a8b05e19298a74ed6ce13c26fe2679f',
    },
    'motrix': {
        'name': 'Motrix',
        'bundle': 'Motrix.app',
        'version': '2.4.1',
        'repository': 'agalwood/Motrix',
        'dmg': 'Motrix-2.4.1-arm64.dmg',
        'dmg_sha256':
            'c90b30594562958bf683e6ae2d47927a7c2fbb597f249406e7192eee793798b1',
    },
}


def sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def write_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n',
                    encoding='utf-8')


def archive_app(app_id, dmg, output_dir):
    app = APPS[app_id]
    if sha256(dmg) != app['dmg_sha256']:
        raise ValueError('DMG SHA256 does not match the pinned official asset')
    output_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='xenon-macos-import-') as temporary:
        mount = Path(temporary) / 'mount'
        mount.mkdir()
        subprocess.run(['hdiutil', 'attach', str(dmg), '-mountpoint', str(mount),
                        '-nobrowse', '-readonly'], check=True,
                       stdout=subprocess.DEVNULL)
        try:
            source = mount / app['bundle']
            contents = source / 'Contents'
            original_plist = (contents / 'Info.plist').read_bytes()
            info = plistlib.loads(original_plist)
            if info.get('CFBundleShortVersionString') != app['version']:
                raise ValueError('Unexpected upstream app version')
            launcher = info.pop('CFBundleExecutable')
            info['CFBundlePackageType'] = 'BNDL'
            hosted_plist = plistlib.dumps(info, fmt=plistlib.FMT_XML,
                                         sort_keys=True)
            if not (contents / 'Resources/app.asar').is_file():
                raise ValueError('Expected upstream Contents/Resources/app.asar')

            # Keep the scope already exercised by the Xenon host. Finder
            # extensions, launch agents, Electron runtime and outer signatures
            # belong to the independently installed upstream application.
            selected = [source, contents, contents / 'Info.plist',
                        contents / 'Resources', contents / 'MacOS']
            selected.extend((contents / 'Resources').rglob('*'))
            # The host uses Contents/MacOS as its working directory even when
            # the resource bundle has no independent Electron executable.
            helpers = [path for path in (contents / 'MacOS').iterdir()
                       if path.name != launcher]
            for helper in helpers:
                selected.append(helper)
                if helper.is_dir() and not helper.is_symlink():
                    selected.extend(helper.rglob('*'))
            selected.sort(key=lambda path: path.relative_to(mount).as_posix())

            entries = []
            archive_tmp = output_dir / 'payload.tar.xz.tmp'
            try:
                with tarfile.open(archive_tmp, 'w:xz', format=tarfile.PAX_FORMAT,
                                  preset=6) as archive:
                    for path in selected:
                        relative = path.relative_to(mount).as_posix()
                        member = archive.gettarinfo(str(path), arcname=relative)
                        member.uid = member.gid = member.mtime = 0
                        member.uname = member.gname = ''
                        member.pax_headers = {}
                        entry = {'path': relative,
                                 'mode': format(stat.S_IMODE(path.lstat().st_mode),
                                                '04o')}
                        if path == contents / 'Info.plist':
                            member.size = len(hosted_plist)
                            archive.addfile(member, io.BytesIO(hosted_plist))
                            entry.update(type='file', size=member.size,
                                         sha256=hashlib.sha256(
                                             hosted_plist).hexdigest())
                        elif member.isfile():
                            with path.open('rb') as file:
                                archive.addfile(member, file)
                            entry.update(type='file', size=member.size,
                                         sha256=sha256(path))
                        elif member.isdir():
                            archive.addfile(member)
                            entry['type'] = 'directory'
                        elif member.issym():
                            path.resolve(strict=True).relative_to(source)
                            archive.addfile(member)
                            entry.update(type='symlink', target=member.linkname)
                        else:
                            raise ValueError(f'Unsupported file type: {path}')
                        entries.append(entry)
                if archive_tmp.stat().st_size >= 100 * 1024 * 1024:
                    raise ValueError('Payload exceeds the 100 MiB Git file limit')
                archive_path = output_dir / 'payload.tar.xz'
                os.replace(archive_tmp, archive_path)
            finally:
                archive_tmp.unlink(missing_ok=True)

            digest = sha256(archive_path)
            (output_dir / 'payload.sha256').write_text(
                f'{digest}  payload.tar.xz\n', encoding='ascii')
            write_json(output_dir / f'{app_id}.xenon.json', {
                'application': './' + app['bundle'],
                'name': app['name'],
                'userDataPolicy': 'legacy-host',
            })
            tag = 'v' + app['version']
            release = f"https://github.com/{app['repository']}/releases"
            write_json(output_dir / 'source.json', {
                'format_version': 1,
                'app_id': app_id,
                'version': app['version'],
                'architecture': 'arm64',
                'release_url': f'{release}/tag/{tag}',
                'download_url': f"{release}/download/{tag}/{app['dmg']}",
                'dmg_filename': app['dmg'],
                'dmg_size': dmg.stat().st_size,
                'dmg_sha256': app['dmg_sha256'],
                'digest_source': (
                    f"https://api.github.com/repos/{app['repository']}"
                    f'/releases/tags/{tag}'),
                'digest_source_field': 'assets[].digest',
                'bundle': app['bundle'],
                'upstream_info_plist_sha256': hashlib.sha256(
                    original_plist).hexdigest(),
                'transformations': [
                    'Set outer Contents/Info.plist CFBundlePackageType to BNDL',
                    'Remove outer Contents/Info.plist CFBundleExecutable',
                    'Serialize outer Info.plist as sorted XML',
                    'Normalize archive timestamps, uid and gid to zero',
                    'Omit filesystem extended attributes from archive',
                ],
                'retained': [
                    'Contents/Info.plist (transformed as listed above)',
                    'Contents/Resources (all original bytes and permissions)',
                    'Contents/MacOS directory and helpers other than the '
                    'Electron launcher',
                ],
                'omitted_upstream': [
                    str(path.relative_to(source))
                    for path in sorted(contents.iterdir())
                    if path.name not in ('Info.plist', 'Resources', 'MacOS')
                ] + [f'Contents/MacOS/{launcher}'],
                'archive': {
                    'file': 'payload.tar.xz',
                    'size': archive_path.stat().st_size,
                    'sha256': digest,
                    'format': 'PAX tar, XZ preset 6',
                },
                'entries': entries,
            })
            print(f'{app_id}: {archive_path.stat().st_size} bytes, {digest}',
                  flush=True)
        finally:
            subprocess.run(['hdiutil', 'detach', str(mount)], check=True,
                           stdout=subprocess.DEVNULL)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app-id', choices=APPS, required=True)
    parser.add_argument('--dmg', required=True, type=Path)
    parser.add_argument('--output-dir', type=Path)
    args = parser.parse_args()
    output_dir = args.output_dir or (
        Path(__file__).resolve().parent.parent / 'resources/mac_electron_apps' /
        args.app_id)
    archive_app(args.app_id, args.dmg.resolve(), output_dir.resolve())


if __name__ == '__main__':
    main()
