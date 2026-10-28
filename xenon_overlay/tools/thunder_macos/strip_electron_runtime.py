#!/usr/bin/env python3
# Copyright 2026 The Xenon Overlay Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Turn an unpublished TH app into a resource bundle using the browser runtime."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import tempfile


RUNTIME_FRAMEWORKS = {
    'Electron Framework.framework': 'com.github.Electron.framework',
    'Squirrel.framework': 'com.github.Squirrel',
    'Mantle.framework': 'org.mantle.Mantle',
    'ReactiveObjC.framework': 'com.electron.reactive',
}
MACHO_MAGICS = {
    b'\xfe\xed\xfa\xce', b'\xce\xfa\xed\xfe',
    b'\xfe\xed\xfa\xcf', b'\xcf\xfa\xed\xfe',
    b'\xca\xfe\xba\xbe', b'\xbe\xba\xfe\xca',
    b'\xca\xfe\xba\xbf', b'\xbf\xba\xfe\xca',
}


def _within(path, root):
    return path == root or root in path.parents


def _regular_file(path):
    if path.is_symlink() or not path.is_file():
        raise ValueError(f'Expected a regular file: {path}')


def _directory(path):
    if path.is_symlink() or not path.is_dir():
        raise ValueError(f'Expected a real directory: {path}')


def _files(root):
    for directory, directories, files in os.walk(root, followlinks=False):
        for name in directories + files:
            item = Path(directory) / name
            if item.is_symlink():
                if not _within(item.resolve(strict=True), root):
                    raise ValueError(f'Bundle symlink escapes its root: {item}')
            elif item.is_file():
                yield item


def _is_macho(path):
    with path.open('rb') as stream:
        return stream.read(4) in MACHO_MAGICS


def _hash(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def _dependencies(path):
    # otool 把末尾括号当成 archive(member)，官方 GPU/Renderer Helper 名称需只读别名。
    with tempfile.TemporaryDirectory(prefix='xenon-otool-') as temporary:
        alias = Path(temporary) / 'image'
        alias.symlink_to(path.resolve(strict=True))
        result = subprocess.run(['otool', '-L', str(alias)], check=True,
                                text=True, capture_output=True)
    return sorted({line.strip().split(' (compatibility version', 1)[0]
                   for line in result.stdout.splitlines()
                   if line.startswith('\t')})


def _executable(bundle, info):
    name = info.get('CFBundleExecutable')
    if not isinstance(name, str) or not name or Path(name).name != name:
        raise ValueError(f'Invalid executable identity: {bundle}')
    executable = bundle / 'Contents/MacOS' / name
    _regular_file(executable)
    if not _is_macho(executable):
        raise ValueError(f'Expected an Electron Mach-O launcher: {executable}')
    return executable


def _uses_electron(dependencies):
    return any('Electron Framework.framework' in Path(dep).parts
               for dep in dependencies)


def inspect_runtime(bundle, staging_root):
    """Validate removal and retained dependencies without changing the bundle."""
    bundle, staging_root = Path(bundle), Path(staging_root)
    _directory(bundle)
    _directory(staging_root)
    bundle, staging_root = bundle.resolve(strict=True), staging_root.resolve(strict=True)
    if bundle == staging_root or not _within(bundle, staging_root):
        raise ValueError('Bundle must be inside the explicit unpublished staging root')
    if bundle.suffix != '.app':
        raise ValueError('Expected a staged Thunder.app resource bundle')
    contents = bundle / 'Contents'
    for directory in (contents, contents / 'MacOS', contents / 'Frameworks',
                      contents / 'Resources', contents / 'Resources/app'):
        _directory(directory)
    info_path = contents / 'Info.plist'
    _regular_file(info_path)
    _regular_file(contents / 'Resources/app/out.asar')
    info = plistlib.loads(info_path.read_bytes())
    identity = info.get('CFBundleIdentifier')
    if not isinstance(identity, str) or not identity:
        raise ValueError('Bundle lacks CFBundleIdentifier')
    files = list(_files(bundle))
    native_files = [file for file in files if _is_macho(file)]
    dependencies = {}

    def deps(file):
        if file not in dependencies:
            dependencies[file] = _dependencies(file)
        return dependencies[file]

    frameworks = contents / 'Frameworks'
    runtime_paths = [frameworks / name for name in RUNTIME_FRAMEWORKS
                     if os.path.lexists(frameworks / name)]
    already_stripped = (info.get('CFBundlePackageType') == 'BNDL' and
                        'CFBundleExecutable' not in info and not runtime_paths)
    remove = []
    if not already_stripped:
        if info.get('CFBundlePackageType') != 'APPL':
            raise ValueError('Expected an official APPL export or a completed BNDL resource bundle')
        executable = _executable(bundle, info)
        if not _uses_electron(deps(executable)):
            raise ValueError('Refusing to remove a launcher that does not link Electron')
        if frameworks / 'Electron Framework.framework' not in runtime_paths:
            raise ValueError('Official Electron Framework is missing')
        remove.append(executable)
        for framework in runtime_paths:
            _directory(framework)
            framework_info = framework / 'Resources/Info.plist'
            framework_id = plistlib.loads(framework_info.read_bytes()).get(
                'CFBundleIdentifier')
            if framework_id != RUNTIME_FRAMEWORKS[framework.name]:
                raise ValueError(f'Unexpected runtime framework identity: {framework}')
            remove.append(framework)
        for helper in sorted(frameworks.glob('*.app')):
            _directory(helper)
            helper_info = plistlib.loads((helper / 'Contents/Info.plist').read_bytes())
            helper_id = helper_info.get('CFBundleIdentifier', '')
            if helper_id == identity + '.helper' or helper_id.startswith(identity + '.helper.'):
                helper_executable = _executable(helper, helper_info)
                if not _uses_electron(deps(helper_executable)):
                    raise ValueError(f'Unexpected non-Electron helper: {helper}')
                remove.append(helper)
        for optional in (contents / 'Resources/electron.icns', contents / 'PkgInfo',
                         contents / '_CodeSignature'):
            if os.path.lexists(optional):
                if optional.is_symlink():
                    raise ValueError(f'Unexpected shell-resource symlink: {optional}')
                remove.append(optional)

    def removed(file):
        return any(_within(file, root) for root in remove)

    removed_native_names = {file.name for file in native_files if removed(file)}
    retained = []
    for file in native_files:
        if removed(file):
            continue
        links = deps(file)
        for dependency in links:
            parts = Path(dependency).parts
            if (any(name in parts for name in RUNTIME_FRAMEWORKS) or
                    Path(dependency).name in removed_native_names):
                raise ValueError('Retained native code requires removed runtime: '
                                 f'{file}: {dependency}')
        retained.append({'path': str(file.relative_to(bundle)),
                         'sha256': _hash(file), 'dependencies': links})
    updated = dict(info)
    updated['CFBundlePackageType'] = 'BNDL'
    updated.pop('CFBundleExecutable', None)
    updated.pop('NSPrincipalClass', None)
    return {
        'bundle': bundle,
        'staging_root': staging_root,
        'updated_info': updated,
        'remove': remove,
        'removed_bytes': sum(file.stat().st_size for file in files if removed(file)),
        'retained_native': retained,
        'changed': not already_stripped,
    }


def strip_runtime(bundle, staging_root, check_only=False):
    """Remove only verified Electron components from the staging copy."""
    plan = inspect_runtime(bundle, staging_root)
    bundle = plan['bundle']
    report = {
        'bundle': str(bundle), 'runtimeProvider': 'browser',
        'independentlyLaunchable': False, 'checkOnly': check_only,
        'changed': plan['changed'], 'removedBytes': plan['removed_bytes'],
        'removed': [str(file.relative_to(bundle)) for file in plan['remove']],
        'retainedNative': plan['retained_native'],
    }
    if check_only or not plan['changed']:
        return report
    # 只移动 staging 内已验证的运行时；失败可恢复，绝不修改输入仓库或运行中的 App。
    backup = Path(tempfile.mkdtemp(prefix='.electron-runtime-', dir=plan['staging_root']))
    moved = []
    info = bundle / 'Contents/Info.plist'
    original_info = backup / 'Info.original'
    succeeded = False
    try:
        replacement = backup / 'Info.replacement'
        replacement.write_bytes(plistlib.dumps(plan['updated_info'], sort_keys=False))
        replacement.chmod(info.stat().st_mode & 0o777)
        for index, source in enumerate(plan['remove']):
            destination = backup / f'removed-{index}'
            source.rename(destination)
            moved.append((source, destination))
        info.rename(original_info)
        replacement.rename(info)
        # 空 MacOS 目录继续作为宿主执行上下文；BNDL 没有独立启动入口。
        for native in plan['retained_native']:
            if _hash(bundle / native['path']) != native['sha256']:
                raise ValueError(f'Retained native code changed: {native["path"]}')
        succeeded = True
    except BaseException:
        if original_info.exists():
            os.replace(original_info, info)
        for original, saved in reversed(moved):
            saved.rename(original)
        raise
    finally:
        # 若回滚本身失败则保留备份，避免删除唯一剩余的文件。
        restored = (not original_info.exists() and
                    all(not saved.exists() for _, saved in moved))
        if succeeded or restored:
            shutil.rmtree(backup)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bundle', required=True, type=Path)
    parser.add_argument('--staging-root', required=True, type=Path)
    parser.add_argument('--report', type=Path)
    parser.add_argument('--check', action='store_true',
                        help='Inspect dependencies without modifying files')
    args = parser.parse_args()
    if args.report:
        parent = args.report.parent.resolve(strict=True)
        if _within(parent, args.bundle.resolve(strict=True)) or os.path.lexists(args.report):
            parser.error('Report must be a new file outside the resource bundle')
    report = strip_runtime(args.bundle, args.staging_root, args.check)
    text = json.dumps(report, indent=2) + '\n'
    if args.report:
        with args.report.open('x') as stream:
            stream.write(text)
    print(text, end='')


if __name__ == '__main__':
    main()
