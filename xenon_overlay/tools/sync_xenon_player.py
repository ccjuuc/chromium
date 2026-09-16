#!/usr/bin/env python3
# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Sync PL-E (xmp_xdas_2 / Player Electron) main + main-renderer into xenon_player dirs.

Example:
  python xenon_overlay/tools/sync_xenon_player.py \\
      --src F:/xl-player/xmp_xdas_2/app/build \\
      --out out/Release_64 \\
      --player-sdk F:/xl-player/xmp_xdas_2/bin \\
      --native-dir F:/xl-player/xmp_xdas_2/cppsrc/build/Release
"""

from __future__ import annotations

import argparse
import base64
import json
import shutil
import subprocess
import sys
from pathlib import Path


def normalize_preloads(source: Path, archive: Path,
                       asar_module: Path) -> tuple[dict[str, bytes], int]:
    """Validate preloads and decode only exact matching vendor archive entries."""
    node = shutil.which('node')
    if not node:
        raise RuntimeError('Node.js is required to validate application preloads')
    helper = Path(__file__).with_name('normalize_player_preloads.cjs')
    result = subprocess.run(
        [node, str(helper), str(source), str(archive), str(asar_module)],
        capture_output=True, text=True, encoding='utf-8', check=False)
    if result.returncode:
        raise RuntimeError(result.stderr.strip() or 'Preload normalization failed')
    payload = json.loads(result.stdout)
    return ({name: base64.b64decode(data)
             for name, data in payload['scripts'].items()}, payload['normalized'])


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


def overlay_tree(src: Path, dst: Path, *, include_maps: bool) -> tuple[int, int]:
    """Copy a tree over an existing runtime tree without deleting base files."""
    dst.mkdir(parents=True, exist_ok=True)
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


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        '--src',
        type=Path,
        default=Path('F:/xl-player/xmp_xdas_2/app/build'),
        help='PL-E app/build output')
    default_out = Path(__file__).resolve().parents[1] / 'resources'
    parser.add_argument(
        '--out',
        type=Path,
        default=default_out,
        help=f'Target dir containing xenon_player (defaults to {default_out})')
    parser.add_argument(
        '--player-sdk',
        type=Path,
        default=Path('F:/xl-player/xmp_xdas_2/bin'),
        help='PL-E player SDK directory')
    parser.add_argument(
        '--native-dir',
        type=Path,
        help=(
            'PL-E native build output. Defaults to '
            '<project>/cppsrc/build/Release when present, otherwise '
            '--player-sdk'))
    parser.add_argument('--include-maps', action='store_true')
    parser.add_argument(
        '--app-version', default='1.0.0',
        help='Application version written to the generated package.json')
    parser.add_argument(
        '--source-archive', type=Path,
        help='Matching original vendor archive (default: <player-sdk>/resources/app/out.asar)')
    parser.add_argument(
        '--vendor-asar-module', type=Path,
        help='Original vendor ASAR module (default: <src>/../asar/security-asar/lib/asar.js)')
    args = parser.parse_args()
    args.src = args.src.resolve()
    args.out = args.out.resolve()
    if args.player_sdk:
        args.player_sdk = args.player_sdk.resolve()
    if args.native_dir:
        args.native_dir = args.native_dir.resolve()
        if not args.native_dir.is_dir():
            print(f'native build output not found: {args.native_dir}', file=sys.stderr)
            return 1
    else:
        inferred_native_dir = (
            args.src.parent.parent / 'cppsrc' / 'build' / 'Release')
        args.native_dir = (
            inferred_native_dir.resolve()
            if inferred_native_dir.is_dir()
            else args.player_sdk)

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
    if not args.player_sdk or not args.player_sdk.is_dir():
        print(f'player SDK not found: {args.player_sdk}', file=sys.stderr)
        return 1

    executable_src = args.player_sdk / 'xmp.exe'
    if not executable_src.is_file():
        print(f'application executable not found: {executable_src}', file=sys.stderr)
        return 1

    missing_native_files = [
        name for name in (
            'dk_addon.node',
            'pc_addon.node',
            'player_helper.node',
            'xmp_helper.node',
        )
        if not (args.native_dir / name).is_file()
    ]
    if missing_native_files:
        print(
            'native build output is incomplete: ' +
            ', '.join(missing_native_files),
            file=sys.stderr)
        return 1

    # The vendor postbuild encrypts app/build scripts in place. Convert its
    # preloads back to public CommonJS source before deleting any old runtime.
    # Product encryption formats and keys stay in the source project's tool.
    archive = (args.source_archive or
               args.player_sdk / 'resources' / 'app' / 'out.asar').resolve()
    asar_module = (args.vendor_asar_module or
                   args.src.parent / 'asar' / 'security-asar' / 'lib' /
                   'asar.js').resolve()
    try:
        preload_scripts, normalized_count = normalize_preloads(
            args.src, archive, asar_module)
    except (RuntimeError, ValueError, OSError) as error:
        print(f'preload validation failed: {error}', file=sys.stderr)
        return 1

    main_dst = args.out / 'xenon_player' / 'main'
    frontend_dst = args.out / 'xenon_player' / 'frontend'

    # Do not follow a junction or symlink when replacing an existing runtime.
    # Check both destinations before deleting either tree.
    for destination in (main_dst, frontend_dst):
        if destination.resolve() != destination:
            print(f'unsafe runtime destination: {destination}', file=sys.stderr)
            return 1

    if main_dst.exists():
        shutil.rmtree(main_dst)
    main_dst.mkdir(parents=True)
    shutil.copy2(main_js, main_dst / 'main.js')
    # Preserve the real application identity; Xenon hosts its main process.
    shutil.copy2(executable_src, main_dst / 'xmp.exe')

    # Copy chunk files next to main.js
    for chunk in args.src.glob('*.js'):
        if chunk.name == 'main.js':
            continue
        shutil.copy2(chunk, main_dst / chunk.name)

    for sub in ('preload', 'preload-native', 'public', 'static'):
        src = args.src / sub
        if src.is_dir():
            copied, skipped = copy_tree(
                src, main_dst / sub, include_maps=args.include_maps)
            print(f'main/{sub}: copied {copied} (skipped maps={skipped})')

    for relative, source in preload_scripts.items():
        (main_dst / relative).write_bytes(source)
    print(f'preloads: validated {len(preload_scripts)}, normalized {normalized_count}')

    # Minimal package.json
    (main_dst / 'package.json').write_text(
        json.dumps({
            'name': 'xmp',
            'version': args.app_version,
            'main': 'main.js',
        }, indent=2) + '\n',
        encoding='utf-8')

    # Native addons and SDK
    player_native_names = (
        'dk_addon.node',
        'pc_addon.node',
        'player_helper.node',
        'xmp_helper.node',
    )
    if args.player_sdk and args.player_sdk.is_dir():
        native_dst = main_dst / 'build' / 'Release'
        native_dst.mkdir(parents=True, exist_ok=True)
        (main_dst / 'build').mkdir(parents=True, exist_ok=True)

        copied_natives = 0
        for name in player_native_names:
            src = args.native_dir / name
            if src.is_file():
                shutil.copy2(src, main_dst / name)
                shutil.copy2(src, main_dst / 'build' / name)
                shutil.copy2(src, native_dst / name)
                copied_natives += 1
        sdk_file_count = copied_natives

        for name in ('player', 'SDK', 'Res', 'resources'):
            src_dir = args.player_sdk / name
            if src_dir.is_dir():
                copied, _ = copy_tree(
                    src_dir,
                    main_dst / name,
                    include_maps=True)
                sdk_file_count += copied

        # PL-E's own packaging first lays down the released runtime and then
        # overlays the freshly built native output. Mirror that order so a
        # newer main bundle is never paired with stale addons/player files.
        native_player_dir = args.native_dir / 'player'
        if (args.native_dir != args.player_sdk and
                native_player_dir.is_dir()):
            copied, _ = overlay_tree(
                native_player_dir,
                main_dst / 'player',
                include_maps=True)
            sdk_file_count += copied

        # Copy the released SDK's companion resources beside the main bundle.
        # Only known helper executables are included: xmp.exe was copied above,
        # and an unrelated application host must not be imported with the SDK.
        helper_executables = {
            'mediainfo.exe', 'associatehelper.exe', 'xlbugreport.exe',
        }
        companion_suffixes = {'.dll', '.pem', '.vsr', '.xml', '.ico'}
        for companion_path in args.player_sdk.iterdir():
            if not companion_path.is_file():
                continue
            if companion_path.name.lower() == 'xdaskernel.dll':
                continue
            if (companion_path.suffix.lower() not in companion_suffixes and
                    companion_path.name.lower() not in helper_executables):
                continue
            shutil.copy2(companion_path, main_dst / companion_path.name)
            sdk_file_count += 1
        print(
            f'PL-E SDK: copied {sdk_file_count} files and runtime to {main_dst}')
        print(f'PL-E native build: {args.native_dir}')

    copied, skipped = copy_tree(
        frontend_src, frontend_dst, include_maps=args.include_maps)
    print(f'frontend: copied {copied} to {frontend_dst} (skipped maps={skipped})')

    print(f'main: {main_dst / "main.js"}')
    print('Launch with PL-E packages in xenon_player/main, or:')
    print(f'  --xenon-player-frontend-dir={frontend_dst}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
