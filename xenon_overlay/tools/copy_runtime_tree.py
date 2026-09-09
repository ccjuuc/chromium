#!/usr/bin/env python3
# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Recursively synchronize a directory tree to the build output directory."""

from __future__ import annotations

import argparse
import os
import posixpath
import shutil
import sys
from pathlib import Path

# Add src root to sys.path so we can import build.action_helpers if needed
_SRC_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(_SRC_ROOT))
try:
    from build.action_helpers import write_depfile
except ImportError:
    write_depfile = None


def fallback_write_depfile(depfile_path: str, first_gn_output: str, inputs: list[str]) -> None:
    def _process_path(p: str) -> str:
        p = p.replace('\\', '/')
        return p.replace(' ', '\\ ')

    sb = [_process_path(first_gn_output), ': \\\n ']
    sb.append(' \\\n '.join(sorted(_process_path(p) for p in set(inputs))))
    sb.append('\n')
    path = Path(depfile_path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, 'w', encoding='utf-8') as f:
        f.write(''.join(sb))


def sync_tree(src_dir: Path, dst_dir: Path) -> tuple[int, int, list[str]]:
    """Synchronize src_dir to dst_dir. Returns (copied_count, skipped_count, relative_inputs_to_cwd)."""
    copied = 0
    skipped = 0
    cwd = Path.cwd()
    input_rel_paths = []

    dst_dir.mkdir(parents=True, exist_ok=True)

    for root, _, files in os.walk(src_dir):
        for file_name in files:
            if file_name.lower() == 'xdaskernel.dll':
                continue
            src_file = Path(root) / file_name
            rel_to_src = src_file.relative_to(src_dir)
            dst_file = dst_dir / rel_to_src

            try:
                rel_to_cwd = str(src_file.relative_to(cwd)).replace('\\', '/')
            except ValueError:
                rel_to_cwd = os.path.relpath(src_file, cwd).replace('\\', '/')
            input_rel_paths.append(rel_to_cwd)

            needs_copy = True
            if dst_file.exists():
                try:
                    src_stat = src_file.stat()
                    dst_stat = dst_file.stat()
                    if src_stat.st_size == dst_stat.st_size and abs(src_stat.st_mtime - dst_stat.st_mtime) < 0.01:
                        needs_copy = False
                except OSError:
                    needs_copy = True

            if needs_copy:
                dst_file.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(src_file, dst_file)
                copied += 1
            else:
                skipped += 1

    for legacy_rel in ('XDASKernel.dll', 'main/XDASKernel.dll'):
        legacy_dst = dst_dir / legacy_rel
        if legacy_dst.is_file():
            try:
                legacy_dst.unlink()
            except OSError:
                pass

    return copied, skipped, input_rel_paths


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--src', type=Path, required=True, help='Source directory')
    parser.add_argument('--dest', type=Path, required=True, help='Destination directory')
    parser.add_argument('--stamp', type=Path, required=True, help='Stamp file to touch')
    parser.add_argument('--depfile', type=Path, help='Depfile path for Ninja tracking')
    args = parser.parse_args()

    src = args.src.resolve()
    dest = args.dest.resolve()

    if not src.is_dir():
        print(f'Error: source directory does not exist: {src}', file=sys.stderr)
        return 1

    copied, skipped, input_rel_paths = sync_tree(src, dest)

    if args.depfile:
        cwd = Path.cwd()
        try:
            stamp_rel = str(args.stamp.relative_to(cwd)).replace('\\', '/')
        except ValueError:
            stamp_rel = os.path.relpath(args.stamp, cwd).replace('\\', '/')
        
        depfile_str = str(args.depfile)
        if write_depfile:
            write_depfile(depfile_str, stamp_rel, input_rel_paths)
        else:
            fallback_write_depfile(depfile_str, stamp_rel, input_rel_paths)

    # Touch stamp file
    args.stamp.parent.mkdir(parents=True, exist_ok=True)
    with open(args.stamp, 'w', encoding='utf-8') as f:
        f.write(f'Copied: {copied}, Skipped: {skipped}\n')

    return 0


if __name__ == '__main__':
    sys.exit(main())
