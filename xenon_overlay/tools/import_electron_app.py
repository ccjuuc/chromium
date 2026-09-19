#!/usr/bin/env python3
# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Import a complete Electron application without rearranging its files.

Archives and native libraries are opaque files. Symlinks, junctions and other
reparse points are rejected explicitly; they are never followed or flattened.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import sys
import tempfile
import uuid


class ImportFailure(ValueError):
    """The release cannot be imported without changing its layout."""


def absolute_path(path: Path) -> Path:
    # Do not resolve links before checking them: that would hide the redirect.
    return Path(os.path.abspath(path))


def reject_link(path: Path) -> None:
    metadata = path.lstat()
    if (stat.S_ISLNK(metadata.st_mode) or
            getattr(metadata, 'st_file_attributes', 0) & 0x400):
        raise ImportFailure(f'links and reparse points are unsupported: {path}')


def check_ancestors(path: Path) -> None:
    for item in (*reversed(path.parents), path):
        if os.path.lexists(item):
            reject_link(item)


def contains(parent: Path, child: Path) -> bool:
    return child == parent or parent in child.parents


def validate_paths(source: Path, target: Path,
                   manifest: Path | None = None) -> tuple[Path, Path, Path | None]:
    source, target = absolute_path(source), absolute_path(target)
    check_ancestors(source)
    check_ancestors(target)
    if not source.is_dir():
        raise ImportFailure(f'source is not a directory: {source}')
    if contains(source, target) or contains(target, source):
        raise ImportFailure('source and target directories must not overlap')
    if target.exists() and not target.is_dir():
        raise ImportFailure(f'target is not a directory: {target}')
    if manifest is not None:
        manifest = absolute_path(manifest)
        check_ancestors(manifest)
        if contains(source, manifest) or contains(target, manifest):
            raise ImportFailure('manifest must be outside source and target')
        if manifest.exists() and not manifest.is_file():
            raise ImportFailure(f'manifest is not a file: {manifest}')
    return source, target, manifest


def inventory(root: Path) -> dict:
    """Record every regular file and directory without following links."""
    check_ancestors(root)
    directories = []
    files = []

    def visit(directory: Path) -> None:
        with os.scandir(directory) as entries:
            children = sorted(entries, key=lambda entry: entry.name)
        for entry in children:
            path = directory / entry.name
            reject_link(path)
            relative = path.relative_to(root).as_posix()
            metadata = path.lstat()
            if stat.S_ISDIR(metadata.st_mode):
                directories.append(relative)
                visit(path)
            elif stat.S_ISREG(metadata.st_mode):
                digest = hashlib.sha256()
                length = 0
                with path.open('rb') as stream:
                    while chunk := stream.read(1024 * 1024):
                        digest.update(chunk)
                        length += len(chunk)
                files.append({'path': relative, 'bytes': length,
                              'sha256': digest.hexdigest()})
            else:
                raise ImportFailure(f'unsupported filesystem entry: {path}')

    visit(root)
    return {'directories': sorted(directories),
            'files': sorted(files, key=lambda entry: entry['path'])}


def detect_layout(source: Path) -> dict:
    """Describe common layouts, without interpreting or opening an ASAR."""
    candidates = []
    for relative in ('Contents/Resources/app.asar',
                     'Contents/Resources/app/package.json',
                     'resources/app.asar', 'resources/app/package.json',
                     'package.json'):
        path = source / relative
        if not path.is_file():
            continue
        candidate = {'path': relative}
        if relative.endswith('.asar'):
            candidate['kind'] = 'archive'
        else:
            try:
                package = json.loads(path.read_text(encoding='utf-8-sig'))
            except (UnicodeError, ValueError) as error:
                raise ImportFailure(f'invalid package metadata: {path}') from error
            if not isinstance(package, dict):
                raise ImportFailure(f'package metadata must be an object: {path}')
            candidate['kind'] = 'directory'
            candidate['package'] = {
                key: package[key] for key in ('name', 'version', 'main')
                if isinstance(package.get(key), str)}
        candidates.append(candidate)
    if not candidates:
        raise ImportFailure(
            'expected resources/app.asar, resources/app/package.json, '
            'Contents/Resources equivalents, or an application package.json')
    return {'kind': ('electron-release' if any(
        item['path'] != 'package.json' for item in candidates)
                     else 'application-directory'),
            'candidates': candidates}


def compare_inventories(expected: dict, actual: dict) -> dict:
    source_files = {entry['path']: entry for entry in expected['files']}
    target_files = {entry['path']: entry for entry in actual['files']}
    source_dirs, target_dirs = set(expected['directories']), set(actual['directories'])
    return {
        'missing_files': sorted(source_files.keys() - target_files.keys()),
        'extra_files': sorted(target_files.keys() - source_files.keys()),
        'changed_files': sorted(name for name in source_files.keys() & target_files.keys()
                                if source_files[name] != target_files[name]),
        'missing_directories': sorted(source_dirs - target_dirs),
        'extra_directories': sorted(target_dirs - source_dirs),
    }


def check_application(source: Path, target: Path) -> dict:
    source, target, _ = validate_paths(source, target)
    expected = inventory(source)
    detect_layout(source)
    if not target.is_dir():
        raise ImportFailure(f'target directory does not exist: {target}')
    differences = compare_inventories(expected, inventory(target))
    return {'equal': not any(differences.values()), **differences}


def remove_owned_tree(path: Path, parent: Path) -> None:
    # Every recursive removal is restricted to a verified immediate child of
    # the destination parent. Modern rmtree does not traverse child junctions.
    path, parent = absolute_path(path), absolute_path(parent)
    check_ancestors(path)
    if path.parent != parent or path == parent:
        raise ImportFailure(f'refusing to remove an unexpected directory: {path}')
    shutil.rmtree(path)


def import_application(source: Path, target: Path,
                       manifest: Path | None = None) -> dict:
    source, target, manifest = validate_paths(source, target, manifest)
    expected = inventory(source)
    layout = detect_layout(source)
    if target.exists():
        inventory(target)  # Reject redirects in the old tree before replacing it.
    record = {'schema_version': 1, 'source': source.as_posix(),
              'target': target.as_posix(), 'layout': layout, **expected}
    target.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix=f'.{target.name}.stage-', dir=target.parent))
    backup = target.parent / f'.{target.name}.backup-{uuid.uuid4().hex}'
    staged_manifest = None
    old_moved = False
    installed = False
    try:
        for relative in expected['directories']:
            (stage / relative).mkdir(parents=True, exist_ok=True)
        for entry in expected['files']:
            original = source / entry['path']
            check_ancestors(original)
            shutil.copy2(original, stage / entry['path'], follow_symlinks=False)
        # Recheck both sides: the source must remain a stable release throughout
        # import, and copying must not silently change a file or directory.
        if inventory(source) != expected or inventory(stage) != expected:
            raise ImportFailure('release changed during import or copied contents differ')
        if manifest is not None:
            manifest.parent.mkdir(parents=True, exist_ok=True)
            descriptor, filename = tempfile.mkstemp(
                prefix=f'.{manifest.name}.stage-', dir=manifest.parent)
            staged_manifest = Path(filename)
            with os.fdopen(descriptor, 'w', encoding='utf-8', newline='\n') as stream:
                json.dump(record, stream, ensure_ascii=False, indent=2)
                stream.write('\n')
        check_ancestors(target)
        if target.exists():
            os.replace(target, backup)
            old_moved = True
        try:
            os.replace(stage, target)
            installed = True
            if staged_manifest is not None:
                check_ancestors(manifest)
                os.replace(staged_manifest, manifest)
        except (OSError, ImportFailure) as error:
            try:
                if installed:
                    os.replace(target, stage)
                if old_moved:
                    os.replace(backup, target)
            except OSError as rollback_error:
                raise ImportFailure(
                    f'import failed and rollback could not finish; '
                    f'original directory retained at {backup}') from rollback_error
            raise ImportFailure('import failed; previous directory restored') from error
        if old_moved:
            try:
                remove_owned_tree(backup, target.parent)
            except (OSError, ImportFailure) as error:
                # Installation already succeeded. Keep the recoverable backup
                # if antivirus, permissions or another process prevent cleanup.
                print(f'warning: old directory retained at {backup}: {error}',
                      file=sys.stderr)
        return record
    finally:
        if stage.exists():
            try:
                remove_owned_tree(stage, target.parent)
            except (OSError, ImportFailure) as error:
                print(f'warning: staging directory retained at {stage}: {error}',
                      file=sys.stderr)
        if staged_manifest is not None and staged_manifest.exists():
            try:
                staged_manifest.unlink()
            except OSError as error:
                print(f'warning: temporary manifest retained at '
                      f'{staged_manifest}: {error}', file=sys.stderr)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--src', type=Path, required=True,
                        help='Complete, stable Electron release or application directory')
    parser.add_argument('--out', type=Path, required=True,
                        help='Destination application directory, not its parent')
    parser.add_argument('--manifest', type=Path,
                        help='Optional layout and SHA-256 manifest outside src/out')
    parser.add_argument('--check', action='store_true',
                        help='Only compare source and destination; never write files')
    args = parser.parse_args(argv)
    if args.check and args.manifest:
        parser.error('--check cannot write --manifest')
    try:
        if args.check:
            comparison = check_application(args.src, args.out)
            print(json.dumps(comparison, ensure_ascii=False))
            return 0 if comparison['equal'] else 1
        result = import_application(args.src, args.out, args.manifest)
        print(json.dumps({'layout': result['layout']['kind'],
                          'files': len(result['files']),
                          'directories': len(result['directories']),
                          'target': result['target']}, ensure_ascii=False))
        return 0
    except (OSError, ImportFailure) as error:
        print(f'error: {error}', file=sys.stderr)
        return 1


def legacy_import_main(application_directory: str,
                       argv: list[str] | None = None) -> int:
    """Keep old script names while requiring one complete release as input."""
    parser = argparse.ArgumentParser(
        description='Import one complete Electron release without changing its layout. '
                    'Build, SDK, frontend and native assembly options are retired.')
    parser.add_argument('--src', type=Path, required=True,
                        help='Complete release directory, not app/build or app/dist')
    parser.add_argument('--out', type=Path,
                        default=Path(__file__).resolve().parents[1] / 'resources',
                        help=f'Parent directory containing {application_directory}')
    parser.add_argument('--manifest', type=Path,
                        help='Optional manifest outside the application directory')
    parser.add_argument('--check', action='store_true',
                        help='Only compare source and destination')
    args, retired = parser.parse_known_args(argv)
    if retired:
        parser.error('unsupported legacy assembly options: ' + ' '.join(retired) +
                     '; provide one complete release with --src instead')
    forwarded = ['--src', str(args.src), '--out',
                 str(args.out / application_directory)]
    if args.manifest is not None:
        forwarded.extend(['--manifest', str(args.manifest)])
    if args.check:
        forwarded.append('--check')
    return main(forwarded)


if __name__ == '__main__':
    sys.exit(main())
