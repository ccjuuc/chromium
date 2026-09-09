#!/usr/bin/env python3
# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Sync a Thunder Electron application into one relocatable runtime tree.

Example:
  python xenon_overlay/tools/sync_thunder_2025.py \\
      --src F:/thunder_2025/app/dist \\
      --out out/Release_64 \\
      --player-sdk F:/thunder_2025/bin/Release
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def copy_tree(src: Path, dst: Path, *, include_maps: bool) -> tuple[int, int]:
    if dst.exists():
        shutil.rmtree(dst)
    dst.mkdir(parents=True)
    copied = 0
    skipped = 0
    for path in src.rglob('*'):
        if not path.is_file():
            continue
        if not include_maps and path.suffix == '.map':
            skipped += 1
            continue
        rel = path.relative_to(src)
        out = dst / rel
        out.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, out)
        copied += 1
    return copied, skipped


def find_packaged_native(root: Path, name: str) -> Path | None:
    """Find a native module in conventional unpacked Electron layouts."""
    for relative_dir in (
            Path(),
            Path('build') / 'Release',
            Path('Release'),
            Path('resources') / 'app' / 'Release'):
        candidate = root / relative_dir / name
        if candidate.is_file():
            return candidate
    return None


def normalize_plugin_asars(plugins_dir: Path, app_root: Path) -> int:
    """Convert vendor ASAR variants to the standard Electron ASAR format.

    The hosted runtime deliberately implements only the public Electron ASAR
    layout. Some packaged plugins use Thunder's build-time encrypted variant;
    decrypt those with the source application's own build tool and immediately
    repack them as standard ASARs. No product keys or formats enter Xenon's
    runtime filesystem implementation.
    """
    archives = sorted(path for path in plugins_dir.rglob('*.asar')
                      if path.is_file())
    if not archives:
        return 0
    node = shutil.which('node')
    resolver = app_root / 'asar' / 'resolve-asar.js'
    if not node or not resolver.is_file():
        print(
            'plugin ASAR normalization requires node and ' + str(resolver),
            file=sys.stderr)
        return -1
    script = r"""
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const {loadAsar} = require(process.argv[1]);
const archives = JSON.parse(process.argv[2]);
const standard = loadAsar('standard');
let security;
(async () => {
  for (const archive of archives) {
    try {
      standard.listPackage(archive);
      continue;
    } catch {}
    const temp = fs.mkdtempSync(path.join(os.tmpdir(), 'thunder-asar-'));
    try {
      security ||= loadAsar('security');
      const extracted = path.join(temp, 'extracted');
      const repacked = path.join(temp, 'standard.asar');
      fs.mkdirSync(extracted);
      const originalLog = console.log;
      console.log = () => {};
      try {
        security.extractAll(archive, extracted);
      } finally {
        console.log = originalLog;
      }
      await standard.createPackage(extracted, repacked);
      fs.copyFileSync(repacked, archive);
      originalLog(`plugin ASAR normalized: ${archive}`);
    } finally {
      fs.rmSync(temp, {recursive: true, force: true});
    }
  }
})().catch(error => {
  console.error(error && error.stack || error);
  process.exitCode = 1;
});
"""
    subprocess.run(
        [node, '-e', script, str(resolver),
         json.dumps([str(path) for path in archives])],
        check=True)
    return len(archives)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        '--src',
        type=Path,
        required=True,
        help='thunder_2025 app/dist output')
    default_out = Path(__file__).resolve().parents[1] / 'resources'
    parser.add_argument(
        '--out',
        type=Path,
        default=default_out,
        help=f'Target dir containing thunder_2025 (defaults to {default_out})')
    parser.add_argument('--include-maps', action='store_true')
    parser.add_argument(
        '--skip-asar',
        action='store_true',
        help='do not create the standard renderer resource ASAR')
    parser.add_argument(
        '--player-sdk',
        type=Path,
        help='player SDK directory for Thunder (e.g. F:/thunder_2025/bin/Release)')
    args = parser.parse_args()
    args.src = args.src.resolve()
    args.out = args.out.resolve()
    if args.player_sdk:
        args.player_sdk = args.player_sdk.resolve()

    if not args.src.is_dir():
        print(f'source not found: {args.src}', file=sys.stderr)
        return 1

    main_js = args.src / 'main.js'
    frontend_src = args.src / 'main-renderer'
    if not main_js.is_file():
        print(f'missing {main_js}', file=sys.stderr)
        return 1
    if not frontend_src.is_dir():
        print(f'missing {frontend_src}', file=sys.stderr)
        return 1

    if args.player_sdk:
        executable_src = args.player_sdk / 'thunder.exe'
        if not executable_src.is_file():
            print(f'application executable not found: {executable_src}', file=sys.stderr)
            return 1

    runtime_dst = args.out / 'thunder_2025'
    app_dst = runtime_dst / 'resources' / 'app'
    main_dst = app_dst / 'out'

    # The old layout mirrored the same payload into main, frontend and ASAR
    # trees. Remove only those legacy sync outputs; other build artifacts in
    # the output directory belong to Chromium or other hosted applications.
    for legacy_path in (
            args.out / 'thunder_2025_main',
            args.out / 'thunder_2025_frontend'):
        if legacy_path.exists():
            shutil.rmtree(legacy_path)
    legacy_archive = args.out / 'thunder_2025_resources.asar'
    if legacy_archive.exists():
        legacy_archive.unlink()
    legacy_executable = args.out / 'Thunder.exe'
    if legacy_executable.exists():
        legacy_executable.unlink()

    if runtime_dst.exists():
        shutil.rmtree(runtime_dst)
    main_dst.mkdir(parents=True)
    shutil.copy2(main_js, main_dst / 'main.js')
    # Webpack may emit sibling chunk files (e.g. 708.js) next to main.js.
    for chunk in args.src.glob('*.js'):
        if chunk.name == 'main.js':
            continue
        shutil.copy2(chunk, main_dst / chunk.name)
        print(f'main chunk: {chunk.name}')
    for name in ('main.js.LICENSE.txt',):
        src = args.src / name
        if src.is_file():
            shutil.copy2(src, main_dst / name)

    # main.js resolves preload relative to __dirname. Renderer bundles are
    # intentionally excluded here and stored once in renderer.asar below.
    for sub in ('preload',):
        src = args.src / sub
        if src.is_dir():
            copied, skipped = copy_tree(
                src, main_dst / sub, include_maps=args.include_maps)
            print(f'main/{sub}: copied {copied} (skipped maps={skipped})')

    player_native_names = (
        'dk_addon.node',
        'pc_addon.node',
        'player_helper.node',
        'thunder_helper.node',
        'lkhb.node',
    )
    optional_native_names = (
        'xmp_helper.node',
        'node_sqlite3.node',
    )
    if args.player_sdk:
        if not args.player_sdk.is_dir():
            print(f'player SDK not found: {args.player_sdk}', file=sys.stderr)
            return 1
        missing_sdk_files = [
            name for name in player_native_names
            if not (args.player_sdk / name).is_file()
        ]
        if missing_sdk_files:
            print(
                'player SDK is missing required files: ' + ', '.join(missing_sdk_files),
                file=sys.stderr)
            return 1
        missing_sdk_dirs = [
            name for name in ('player', 'SDK')
            if not (args.player_sdk / name).is_dir()
        ]
        if missing_sdk_dirs:
            print(
                'player SDK is incomplete: ' + ', '.join(missing_sdk_dirs),
                file=sys.stderr)
            return 1

        copied_natives = 0
        for name in player_native_names + optional_native_names:
            src = find_packaged_native(args.player_sdk, name)
            if src:
                if name == 'node_sqlite3.node':
                    native_dst = app_dst / 'Release' / name
                    native_dst.parent.mkdir(parents=True, exist_ok=True)
                else:
                    native_dst = runtime_dst / name
                shutil.copy2(src, native_dst)
                copied_natives += 1
        sdk_file_count = copied_natives

        for name in ('player', 'SDK', 'service', 'addins'):
            src_dir = args.player_sdk / name
            if src_dir.is_dir():
                copied, _ = copy_tree(
                    src_dir,
                    runtime_dst / name,
                    include_maps=True)
                sdk_file_count += copied

        # Native SDKs resolve companion DLLs relative to process.execPath.
        # Exclude legacy Electron runtime DLLs (e.g. XDASKernel.dll) that Xenon replaces.
        for dll_path in args.player_sdk.glob('*.dll'):
            if dll_path.name.lower() == 'xdaskernel.dll':
                continue
            shutil.copy2(dll_path, runtime_dst / dll_path.name)
        # Retain the packaged executable's real identity/version resources.
        # It is not launched; Xenon runs the application's main module.
        shutil.copy2(executable_src, runtime_dst / 'Thunder.exe')
        for name in ('Thunder.ico', 'download-complete.wav'):
            source = args.player_sdk / name
            if source.is_file():
                shutil.copy2(source, runtime_dst / name)
        print(
            f'thunder SDK: copied {sdk_file_count} files and runtime to '
            f'{runtime_dst}')

    # Keep the standard Electron app metadata at resources/app. Preserve the
    # product metadata where available but point main at the extracted bundle.
    package = {
        'name': 'thunder',
        'version': '0.0.1',
        'main': './out/main.js',
    }
    if args.player_sdk:
        package_src = args.player_sdk / 'resources' / 'app' / 'package.json'
        if package_src.is_file():
            with package_src.open(encoding='utf-8') as package_file:
                package = json.load(package_file)
            package['main'] = './out/main.js'
    app_dst.mkdir(parents=True, exist_ok=True)
    (app_dst / 'package.json').write_text(
        json.dumps(package, ensure_ascii=False, separators=(',', ':')) + '\n',
        encoding='utf-8')

    # Copy built plugins (e.g. player-plugin.asar, thunder-pan-plugin.asar)
    plugin_src_dirs = []
    if args.player_sdk:
        plugin_src_dirs.append(
            args.player_sdk / 'resources' / 'app' / 'plugins')
    plugin_src_dirs.extend([
        args.src.parent.parent / 'bin' / 'Release' / 'resources' / 'app' /
        'plugins',
        args.src.parent.parent / 'bin' / 'release' / 'resources' / 'app' /
        'plugins',
    ])
    for p_dir in plugin_src_dirs:
        if p_dir.is_dir():
            plugins_dst = app_dst / 'plugins'
            plugins_dst.mkdir(parents=True, exist_ok=True)
            for item in p_dir.iterdir():
                if item.is_file():
                    shutil.copy2(item, plugins_dst / item.name)
                elif item.is_dir():
                    copy_tree(
                        item,
                        plugins_dst / item.name,
                        include_maps=args.include_maps)
            print(f'plugins: copied from {p_dir} to {plugins_dst}')
            if normalize_plugin_asars(plugins_dst, args.src.parent) < 0:
                return 1
            break

    if not args.skip_asar:
        node = shutil.which('node')
        if not node:
            print('node executable not found', file=sys.stderr)
            return 1
        archive_dst = app_dst / 'renderer.asar'
        renderer_dirs = (
            # Renderers resolve webview preloads relative to their HTML file.
            # Keep these siblings inside the archive as in Electron's dist.
            'preload',
            'main-renderer',
            'modal-renderer',
            'suspension-renderer',
            'thunder-im',
            'static',
        )
        with tempfile.TemporaryDirectory(prefix='thunder-renderer-') as temp:
            renderer_stage = Path(temp)
            for name in renderer_dirs:
                source = args.src / name
                if source.is_dir():
                    copied, skipped = copy_tree(
                        source,
                        renderer_stage / name,
                        include_maps=args.include_maps)
                    print(
                        f'renderer/{name}: copied {copied} '
                        f'(skipped maps={skipped})')
            pack_script = (
                f"const {{ asar }} = require('./asar/resolve-asar'); "
                f"asar.createPackage({json.dumps(str(renderer_stage))}, "
                f"{json.dumps(str(archive_dst))})"
                f".then(() => console.log('renderer ASAR packed successfully'));"
            )
            subprocess.run(
                [node, '-e', pack_script],
                cwd=str(args.src.parent),
                check=True)
        print(f'renderer ASAR: {archive_dst}')

    print(f'main: {main_dst / "main.js"}')
    print('Launch with the canonical thunder_2025 runtime, or:')
    print(
        '  --xenon-thunder-2025-frontend-dir=' +
        str(app_dst / 'renderer.asar' / 'main-renderer'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
