#!/usr/bin/env python3
# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Add explicitly configured, unchanged application payloads to Chrome staging.

GN only selects this wrapper when enable_xenon_service is enabled. Application
directories and external host files are arguments, not product-name rules.
--xenon-staging-only exercises the same staging and verification without 7z,
binary differencing, setup compression or compilation.
"""

from __future__ import annotations

import argparse
import glob
import hashlib
import importlib.util
import json
import os
from pathlib import Path, PureWindowsPath
import shutil
import sys

from import_electron_app import (ImportFailure, absolute_path, check_ancestors,
                                 contains, detect_layout, import_application,
                                 inventory)


class StagingComplete(Exception):
    """Stop the upstream pipeline immediately before archive creation."""


def insensitive_glob(pattern: str) -> list[str]:
    # A wildcard drive such as "[cC]:" is not a Windows drive. Preserve the
    # drive/UNC prefix while retaining upstream's case-insensitive matching.
    drive, tail = os.path.splitdrive(pattern)
    return glob.glob(drive + ''.join(
        f'[{character.lower()}{character.upper()}]' if character.isalpha()
        else character for character in tail))


def relative_path(value: str) -> Path:
    path = Path(value.replace('\\', '/'))
    if (not value or path == Path('.') or path.is_absolute() or
            PureWindowsPath(value).drive or '..' in path.parts):
        raise ImportFailure(f'payload path must be build-relative: {value}')
    return path


def digest_file(path: Path) -> dict:
    check_ancestors(path)
    if not path.is_file():
        raise ImportFailure(f'required installer file is missing: {path}')
    digest = hashlib.sha256()
    length = 0
    with path.open('rb') as stream:
        while chunk := stream.read(1024 * 1024):
            length += len(chunk)
            digest.update(chunk)
    return {'bytes': length, 'sha256': digest.hexdigest()}


def prepare_payloads(archive, options, wrapper_options) -> tuple[list, list]:
    build = absolute_path(Path(options.build_dir))
    check_ancestors(build)
    for value in wrapper_options.xenon_required_file:
        digest_file(build / relative_path(value))
    canonical_sources = {}
    for value in wrapper_options.xenon_payload_source:
        directory, separator, source = value.partition('=')
        relative = relative_path(directory)
        if not separator or not source or relative in canonical_sources:
            raise ImportFailure('payload source requires one directory=source declaration')
        canonical_sources[relative] = absolute_path(Path(source))
    directories = []
    for value in wrapper_options.xenon_payload_directory:
        relative = relative_path(value)
        source = build / relative
        if not source.is_dir():
            raise ImportFailure(f'required application directory is missing: {source}')
        if any(contains(previous['path'], relative) or
               contains(relative, previous['path']) for previous in directories):
            raise ImportFailure('application payload directories must not overlap')
        expected = inventory(source)
        detect_layout(source)
        canonical = canonical_sources.pop(relative, None)
        if canonical is not None and inventory(canonical) != expected:
            raise ImportFailure(
                f'build application differs from canonical source: {relative}; '
                'resynchronize the complete release before packaging')
        directories.append({'path': relative, 'inventory': expected,
                            'canonical_source': canonical})
    if canonical_sources:
        raise ImportFailure('payload source must name a declared application directory')
    file_paths = {relative_path(value) for value in wrapper_options.xenon_payload_file}
    for value in wrapper_options.xenon_payload_glob:
        pattern = relative_path(value)
        if glob.has_magic(str(pattern.parent)):
            raise ImportFailure('payload glob requires a fixed parent directory')
        watched = build / pattern.parent
        while not watched.exists() and watched != build:
            watched = watched.parent
        check_ancestors(watched)
        archive.g_archive_inputs.append(os.path.relpath(watched, build))
        for match in archive.insensiglob(str(build / pattern)):
            file_paths.add(Path(match).relative_to(build))
    files = []
    for relative in sorted(file_paths):
        if any(contains(directory['path'], relative) for directory in directories):
            raise ImportFailure('external payload file is already inside an application')
        files.append({'path': relative, **digest_file(build / relative)})
    return directories, files


def stage_payloads(archive, options, staging: Path, directories: list,
                   files: list) -> None:
    build = absolute_path(Path(options.build_dir))
    destination = staging / archive.CHROME_DIR
    check_ancestors(destination)
    destination.mkdir(parents=True, exist_ok=True)
    for payload in directories:
        relative = payload['path']
        copied = import_application(build / relative, destination / relative)
        expected = payload['inventory']
        if any(copied[key] != expected[key] for key in ('directories', 'files')):
            raise ImportFailure(f'application changed during staging: {relative}')
        # Track directories too, so adding/removing a nested file invalidates
        # the archive even when all previously listed files remain unchanged.
        archive.g_archive_inputs.append(str(relative))
        archive.g_archive_inputs.extend(str(relative / name)
                                        for name in expected['directories'])
        archive.g_archive_inputs.extend(str(relative / entry['path'])
                                        for entry in expected['files'])
        canonical = payload['canonical_source']
        if canonical is not None:
            source_inputs = [canonical]
            source_inputs.extend(canonical / name for name in expected['directories'])
            source_inputs.extend(canonical / entry['path'] for entry in expected['files'])
            for source_input in source_inputs:
                try:
                    dependency = os.path.relpath(source_input, build)
                except ValueError:  # A canonical source can be on another drive.
                    dependency = str(source_input)
                archive.g_archive_inputs.append(dependency)
    for payload in files:
        relative = payload['path']
        target = destination / relative
        check_ancestors(target)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(build / relative, target, follow_symlinks=False)
        archive.g_archive_inputs.append(str(relative))


def verify_payloads(archive, options, staging: Path, directories: list,
                    files: list) -> dict:
    build = absolute_path(Path(options.build_dir))
    destination = staging / archive.CHROME_DIR
    summary = {'staging_directory': destination.as_posix(),
               'applications': [], 'external_files': []}
    for payload in directories:
        relative, expected = payload['path'], payload['inventory']
        if (inventory(build / relative) != expected or
                inventory(destination / relative) != expected or
                (payload['canonical_source'] is not None and
                 inventory(payload['canonical_source']) != expected)):
            raise ImportFailure(f'installer application differs from build output: {relative}')
        summary['applications'].append({
            'path': relative.as_posix(), 'files': len(expected['files']),
            'directories': len(expected['directories']), 'verified': True})
    for payload in files:
        relative = payload['path']
        expected = {key: payload[key] for key in ('bytes', 'sha256')}
        if (digest_file(build / relative) != expected or
                digest_file(destination / relative) != expected):
            raise ImportFailure(f'installer host file differs from build output: {relative}')
        summary['external_files'].append(relative.as_posix())
    return summary


def load_archive_module():
    script = (Path(__file__).resolve().parents[2] / 'chrome/tools/build/win/'
              'create_installer_archive.py')
    specification = importlib.util.spec_from_file_location('chrome_installer_archive', script)
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(add_help=False, allow_abbrev=False)
    parser.add_argument('--xenon-required-file', action='append', default=[])
    parser.add_argument('--xenon-payload-directory', action='append', default=[])
    parser.add_argument('--xenon-payload-source', action='append', default=[])
    parser.add_argument('--xenon-payload-file', action='append', default=[])
    parser.add_argument('--xenon-payload-glob', action='append', default=[])
    parser.add_argument('--xenon-staging-only', action='store_true')
    wrapper_options, forwarded = parser.parse_known_args(argv)
    archive = load_archive_module()
    archive.insensiglob = insensitive_glob
    previous_argv = sys.argv
    try:
        sys.argv = [archive.__file__, *forwarded]
        options = archive._ParseOptions()
    finally:
        sys.argv = previous_argv
    archive.options = options
    try:
        if wrapper_options.xenon_staging_only and options.last_chrome_installer:
            raise ImportFailure('staging-only does not accept a differential installer baseline')
        build = absolute_path(Path(options.build_dir))
        staging_parent = absolute_path(Path(options.staging_dir))
        staging = staging_parent / archive.TEMP_ARCHIVE_DIR
        check_ancestors(staging)
        # Upstream recreates exactly this directory. Verify the final resolved
        # target before allowing its recursive deletion to run.
        if staging.resolve() != staging or contains(staging, build):
            raise ImportFailure('unsafe installer staging directory')
        directories, files = prepare_payloads(archive, options, wrapper_options)
        for payload in directories:
            source = build / payload['path']
            if contains(source, staging) or contains(staging, source):
                raise ImportFailure('staging must not overlap an application payload')
        for payload in files:
            if contains(staging, build / payload['path']):
                raise ImportFailure('staging must not contain an external payload input')
        for value in wrapper_options.xenon_required_file:
            if contains(staging, build / relative_path(value)):
                raise ImportFailure('staging must not contain a required input')
        if staging.exists():
            inventory(staging)  # Reject redirects in an existing staging tree.
        original_copy = archive.CopyAllFilesToStagingDir
        original_create = archive.CreateArchiveFiles

        def copy_with_payloads(config, distribution, staged, *arguments):
            original_copy(config, distribution, staged, *arguments)
            stage_payloads(archive, options, Path(staged), directories, files)
            if options.build_time:
                archive.OverwriteStagingBuildTime(options.build_time)

        def create_after_verification(settings, staged, *arguments):
            report = verify_payloads(archive, options, Path(staged), directories, files)
            print(json.dumps(report, ensure_ascii=False))
            if wrapper_options.xenon_staging_only:
                raise StagingComplete()
            return original_create(settings, staged, *arguments)

        archive.CopyAllFilesToStagingDir = copy_with_payloads
        archive.CreateArchiveFiles = create_after_verification
        archive.main(options)
        return 0
    except StagingComplete:
        return 0
    except (OSError, ImportFailure) as error:
        print(f'error: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
