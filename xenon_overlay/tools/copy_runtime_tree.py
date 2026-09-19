#!/usr/bin/env python3
# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Recursively synchronize a directory tree to the build output directory."""

from __future__ import annotations

import argparse
import os
import shutil
import stat
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


def _metadata_without_links(path: Path) -> os.stat_result | None:
    try:
        metadata = path.lstat()
    except FileNotFoundError:
        return None
    if (stat.S_ISLNK(metadata.st_mode) or
            getattr(metadata, 'st_file_attributes', 0) & 0x400 or
            (stat.S_ISREG(metadata.st_mode) and metadata.st_nlink > 1)):
        raise ValueError(f'links and reparse points are unsupported: {path}')
    return metadata


def _check_ancestors(path: Path) -> None:
    for candidate in (*reversed(path.parents), path):
        _metadata_without_links(candidate)


def _contains(parent: Path, child: Path) -> bool:
    return child == parent or parent in child.parents


def _validate_roots(source: Path, target: Path) -> tuple[Path, Path]:
    # Resolve relative spelling, but do not follow links before checking them.
    source, target = Path(os.path.abspath(source)), Path(os.path.abspath(target))
    _check_ancestors(source)
    _check_ancestors(target)
    if not source.is_dir():
        raise ValueError(f'source is not a directory: {source}')
    source, target = source.resolve(), target.resolve()
    if _contains(source, target) or _contains(target, source):
        raise ValueError('source and destination directories must not overlap')
    if target.exists() and not target.is_dir():
        raise ValueError(f'destination is not a directory: {target}')
    return source, target


def _inventory(root: Path) -> tuple[set[str], set[str]]:
    # String keys retain filename casing even on case-insensitive hosts.
    directories, files = {'.'}, set()

    def visit(directory: Path) -> None:
        _check_ancestors(directory)
        with os.scandir(directory) as entries:
            children = sorted(entries, key=lambda item: item.name)
        for entry in children:
            path = directory / entry.name
            metadata = _metadata_without_links(path)
            if metadata is None:
                raise ValueError(f'runtime changed while being scanned: {path}')
            relative = path.relative_to(root).as_posix()
            if stat.S_ISDIR(metadata.st_mode):
                directories.add(relative)
                visit(path)
            elif stat.S_ISREG(metadata.st_mode):
                files.add(relative)
            else:
                raise ValueError(f'unsupported filesystem entry: {path}')

    visit(root)
    return directories, files


def _checked_destination(root: Path, path: Path, *, allow_root: bool = False) -> Path:
    _check_ancestors(path)
    resolved_root, resolved_path = root.resolve(), path.resolve()
    if (not _contains(resolved_root, resolved_path) or
            (resolved_path == resolved_root and not allow_root)):
        raise ValueError(f'refusing to modify a path outside destination: {path}')
    return path


def _same_contents(source: Path, target: Path) -> bool:
    if source.stat().st_size != target.stat().st_size:
        return False
    # Timestamp equality alone cannot establish byte equality: release tools
    # can preserve timestamps when replacing an archive or native library.
    with source.open('rb') as original, target.open('rb') as existing:
        while True:
            original_bytes = original.read(1024 * 1024)
            if original_bytes != existing.read(1024 * 1024):
                return False
            if not original_bytes:
                return True


def sync_tree(src_dir: Path, dst_dir: Path) -> tuple[int, int, list[str]]:
    """Mirror layout and bytes incrementally; return copied, skipped, inputs."""
    src_dir, dst_dir = _validate_roots(src_dir, dst_dir)
    source_dirs, source_files = _inventory(src_dir)
    target_dirs, target_files = (_inventory(dst_dir) if dst_dir.exists()
                                 else (set(), set()))
    # Validate both complete trees before deleting stale output or copying.
    _checked_destination(dst_dir, dst_dir, allow_root=True).mkdir(
        parents=True, exist_ok=True)
    for relative in sorted(target_files - source_files):
        _checked_destination(dst_dir, dst_dir / relative).unlink()
    for relative in sorted(target_dirs - source_dirs,
                           key=lambda path: (path.count('/'), path), reverse=True):
        # Children were inventoried and removed individually. Never recurse
        # through a directory that could have become a junction or symlink.
        _checked_destination(dst_dir, dst_dir / relative).rmdir()
    for relative in sorted(source_dirs, key=lambda path: (path.count('/'), path)):
        _checked_destination(dst_dir, dst_dir / relative, allow_root=True).mkdir(
            exist_ok=True)

    copied = skipped = 0
    for relative in sorted(source_files):
        source, target = src_dir / relative, dst_dir / relative
        _check_ancestors(source)
        _checked_destination(dst_dir, target)
        if target.exists() and _same_contents(source, target):
            skipped += 1
        else:
            shutil.copy2(source, target)
            copied += 1

    # Directories are dependencies too: otherwise creating or removing an
    # entry that was absent from the previous depfile would not rerun Ninja.
    inputs = sorted(
        os.path.relpath(src_dir / relative, Path.cwd()).replace('\\', '/')
        for relative in source_dirs | source_files)
    return copied, skipped, inputs


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--src', type=Path, required=True, help='Source directory')
    parser.add_argument('--dest', type=Path, required=True, help='Destination directory')
    parser.add_argument('--stamp', type=Path, required=True, help='Stamp file to touch')
    parser.add_argument('--depfile', type=Path, help='Depfile path for Ninja tracking')
    args = parser.parse_args()

    try:
        copied, skipped, input_rel_paths = sync_tree(args.src, args.dest)
    except (OSError, ValueError) as error:
        print(f'Error: {error}', file=sys.stderr)
        return 1

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
