#!/usr/bin/env python3
"""Install a Mac TH payload bundle and its relative descriptor into xl153.

Run after building Chromium, then sign the outer host bundle as usual.
Framework symlinks inside Thunder.app are preserved; escaping links are rejected.
Previous deployments are backed up outside the signed host bundle.
"""

import argparse
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import tempfile


def _exists(path: Path) -> bool:
    return os.path.lexists(path)


def _remove(path: Path) -> None:
    if path.is_dir() and not path.is_symlink():
        shutil.rmtree(path)
    elif _exists(path):
        path.unlink()


def _copy(source: Path, destination: Path) -> None:
    if source.is_symlink():
        destination.symlink_to(os.readlink(source))
    elif source.is_dir():
        # Clone on APFS when possible; an explicit backup directory may be on
        # another volume, where ordinary recursive copying is required.
        result = subprocess.run(['cp', '-cRp', str(source), str(destination)],
                                stdout=subprocess.DEVNULL,
                                stderr=subprocess.PIPE)
        if result.returncode:
            _remove(destination)
            subprocess.run(['cp', '-Rp', str(source), str(destination)],
                           check=True)
    else:
        shutil.copy2(source, destination, follow_symlinks=False)


def deploy(host: Path, thunder: Path, backup_dir: Path = None) -> Path:
    host, thunder = host.resolve(strict=True), thunder.resolve(strict=True)
    for app in (host, thunder):
        if app.suffix != '.app' or not (app / 'Contents/Info.plist').is_file():
            raise ValueError(f'Not a complete app bundle: {app}')
    if host == thunder or host in thunder.parents or thunder in host.parents:
        raise ValueError('Host and source bundles must not overlap')
    for relative in ('Contents', 'Contents/MacOS', 'Contents/Resources',
                     'Contents/Resources/xenon-apps'):
        directory = host / relative
        if not directory.resolve().is_relative_to(host):
            raise ValueError(f'Host deployment directory escapes its bundle: {relative}')
    resources = thunder / 'Contents/Resources/app'
    if not (resources / 'out.asar').is_file():
        raise ValueError('Expected the complete TH app with Resources/app/out.asar')
    declaration = thunder / 'Contents/Resources/xenon-native-player.json'
    expected_adapter = {
        'schemaVersion': 1,
        'adapter': {'id': 'xunlei.mac-player', 'version': 1},
    }
    if (not declaration.is_file() or
            json.loads(declaration.read_text()) != expected_adapter):
        raise ValueError('TH bundle lacks the supported native player adapter declaration')
    for root, dirs, files in os.walk(thunder, followlinks=False):
        for name in dirs + files:
            item = Path(root) / name
            if item.is_symlink():
                if os.path.isabs(os.readlink(item)):
                    raise ValueError(f'Bundle symlink must remain relocatable: {item}')
                if not item.resolve().is_relative_to(thunder):
                    raise ValueError(f'Bundle symlink escapes its root: {item}')
    info = plistlib.loads((thunder / 'Contents/Info.plist').read_bytes())
    executable = info.get('CFBundleExecutable')
    if executable is not None:
        if Path(executable).name != executable or not (thunder / 'Contents/MacOS' / executable).is_file():
            raise ValueError('Invalid Thunder executable identity')
    elif info.get('CFBundlePackageType') != 'BNDL':
        raise ValueError('A payload without an executable must be a resource bundle')
    if not (thunder / 'Contents/MacOS').is_dir():
        raise ValueError('TH payload must preserve its runtime working directory')
    if not (host / 'Contents/MacOS').is_dir():
        raise ValueError('Host bundle has no Contents/MacOS directory')
    backup_dir = (backup_dir if backup_dir is not None else
                  Path(__file__).resolve().parents[2] / 'test_packages' /
                  'thunder-deployment-backups').resolve()
    if any(backup_dir == app or app in backup_dir.parents
           for app in (host, thunder)):
        raise ValueError('Backup directory must be outside both app bundles')

    destination = host / 'Contents/Resources/xenon-apps/Thunder.app'
    legacy_backup = destination.with_name('Thunder.previous.app')
    config = destination.parent / 'thunder_2025.xenon.json'
    legacy_config = host / 'Contents/MacOS/thunder_2025.xenon.json'
    relative = './Thunder.app'
    descriptor = {
        'application': relative,
        # The browser supplies the executable/runtime. The application resolver
        # independently preserves TH's Resources and MacOS working directory.
        'name': 'Thunder',
        'version': info.get('CFBundleShortVersionString', '25.1.13'),
        'main': 'out.asar/main.js',
        'privilegedSchemes': [
            {'scheme': 'miniapp', 'privileges': {
                'standard': True, 'secure': True,
                'supportFetchAPI': True, 'corsEnabled': True}},
            {'scheme': 'crx', 'privileges': {'bypassCSP': True}},
        ],
    }
    descriptor_bytes = (json.dumps(descriptor, indent=2) + '\n').encode()
    had_destination, had_config = _exists(destination), _exists(config)
    had_legacy_backup = _exists(legacy_backup)
    had_legacy_config = _exists(legacy_config)
    backup = None
    if had_destination or had_config or had_legacy_backup or had_legacy_config:
        backup_dir.mkdir(parents=True, exist_ok=True)
        backup = Path(tempfile.mkdtemp(prefix='thunder-deploy-', dir=backup_dir))
        if had_destination:
            _copy(destination, backup / 'Thunder.app')
        if had_config:
            _copy(config, backup / config.name)
        if had_legacy_backup:
            _copy(legacy_backup, backup / legacy_backup.name)
        if had_legacy_config:
            _copy(legacy_config, backup / 'legacy-macos-descriptor.json')

    destination.parent.mkdir(parents=True, exist_ok=True)
    # Keep all temporary/previous bundles outside the host's code signature.
    stage = Path(tempfile.mkdtemp(prefix='.thunder-stage-', dir=host.parent))
    previous = stage / 'previous.app'
    previous_config = stage / 'previous.json'
    previous_legacy_backup = stage / 'legacy-previous.app'
    previous_legacy_config = stage / 'legacy-macos-descriptor.json'
    destination_installed = config_installed = False
    cleanup_stage = True
    try:
        staged = stage / 'Thunder.app'
        _copy(thunder, staged)
        temporary = stage / 'descriptor.json'
        temporary.write_bytes(descriptor_bytes)
        if had_destination:
            destination.rename(previous)
        if had_config:
            config.rename(previous_config)
        if had_legacy_backup:
            legacy_backup.rename(previous_legacy_backup)
        if had_legacy_config:
            legacy_config.rename(previous_legacy_config)
        staged.rename(destination)
        destination_installed = True
        temporary.replace(config)
        config_installed = True
        return config
    except BaseException:
        try:
            if config_installed:
                _remove(config)
            if _exists(previous_config):
                previous_config.rename(config)
            if destination_installed:
                _remove(destination)
            if _exists(previous):
                previous.rename(destination)
            if _exists(previous_legacy_backup):
                previous_legacy_backup.rename(legacy_backup)
            if _exists(previous_legacy_config):
                previous_legacy_config.rename(legacy_config)
        except BaseException as rollback_error:
            cleanup_stage = False
            raise RuntimeError(
                f'Deployment rollback failed; originals remain in {stage}, '
                f'backup: {backup}') from rollback_error
        raise
    finally:
        if cleanup_stage:
            shutil.rmtree(stage, ignore_errors=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', required=True, type=Path)
    parser.add_argument('--thunder', required=True, type=Path)
    parser.add_argument('--backup-dir', type=Path,
                        help='External backup root (default: test_packages/thunder-deployment-backups)')
    args = parser.parse_args()
    print(deploy(args.host, args.thunder, args.backup_dir))
