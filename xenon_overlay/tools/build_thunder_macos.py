#!/usr/bin/env python3
# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""从只读 TH 源码快照构建明确标记的 153 兼容版 Thunder.app。"""

import argparse
import ctypes
import datetime
import fcntl
import hashlib
import json
import os
from pathlib import Path
import platform
import plistlib
import shutil
import subprocess
import sys
import tarfile
import tempfile

import archive_thunder_macos as archive_tool


SUPPORT = Path(__file__).resolve().parent / 'thunder_macos'
MANIFESTS = (SUPPORT / 'patches/native-compatibility.json',
             SUPPORT / 'js-compatibility.json')
# 仅排除已知构建输出；node_modules 中的 build/dist 是已安装依赖，必须复制。
EXCLUDED_ROOTS = (
    'dist-mac', 'component-ui', 'components-ui', 'e2e',
    'cppsrc/build', 'app/dist', 'app/build', 'app/dist-asar',
    'app/src/player-plugin/build', 'app/src/thunder-pan-plugin/build',
    'bin/natives', 'bin/Release/mac-app',
)
EXCLUDED_NAMES = {'.git', '.DS_Store', '.cache', '.rsbuild'}


class BuildError(RuntimeError):
    pass


def sha256(path):
    if not path.is_file():
        return None
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def write_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n')


def safe_build_log_line(line):
    # node-gyp 的 Python 探测以 %j 在单行打印整个 opts.env；不保留这条诊断的任何值。
    if 'execFile: opts =' in line and '"env"' in line:
        return '[node-gyp diagnostic environment omitted]\n'
    return line


def sanitize_upstream_build_logs(workspace):
    """仅清理本次隔离工作流的已知日志，不扫描或改写输入源码。"""
    directory = workspace / 'dist-mac/logs'
    try:
        if directory.is_symlink() or directory.parent.is_symlink():
            raise OSError('Unexpected workflow log directory symlink')
        filenames = sorted(directory.glob('*/build.log'))
    except OSError:
        print(f'警告：无法检查上游日志环境诊断，请在分享前检查: {directory}',
              file=sys.stderr)
        return
    for filename in filenames:
        temporary = None
        try:
            if filename.is_symlink() or filename.parent.is_symlink():
                raise OSError('Unexpected workflow log symlink')
            with filename.open('r', encoding='utf-8', errors='surrogateescape',
                               newline='') as original:
                with tempfile.NamedTemporaryFile(
                        mode='w', encoding='utf-8', errors='surrogateescape',
                        newline='', dir=filename.parent, delete=False) as cleaned:
                    temporary = Path(cleaned.name)
                    changed = False
                    for line in original:
                        safe = safe_build_log_line(line)
                        changed |= safe != line
                        cleaned.write(safe)
            if changed:
                temporary.chmod(filename.stat().st_mode & 0o777)
                temporary.replace(filename)
            else:
                temporary.unlink()
            temporary = None
        except OSError:
            # 不用日志内容或底层异常覆盖真正的编译错误，原诊断文件仍留在工作快照中。
            print(f'警告：上游日志环境诊断清理失败，请在分享前检查: {filename}',
                  file=sys.stderr)
        finally:
            if temporary is not None:
                try:
                    temporary.unlink(missing_ok=True)
                except OSError:
                    pass


def run(command, cwd=None, env=None, log=None):
    if log is not None:
        # 先按行过滤再写磁盘，避免分块输出绕过检查或运行中的 tail 读到完整环境。
        with subprocess.Popen([str(item) for item in command], cwd=cwd, env=env,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              text=True, encoding='utf-8', errors='replace') as child:
            try:
                for line in child.stdout:
                    log.write(safe_build_log_line(line))
                    log.flush()
                returncode = child.wait()
            except BaseException:
                child.kill()
                child.wait()
                raise
        if returncode:
            raise BuildError(f'{Path(command[0]).name} 失败（{returncode}）；请查看 build.log')
        return ''
    result = subprocess.run([str(item) for item in command], cwd=cwd, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if result.returncode:
        detail = (result.stderr or result.stdout or '').strip()
        raise BuildError(f'{Path(command[0]).name} 失败（{result.returncode}）'
                         + (f': {detail}' if detail else '；请查看 build.log'))
    return result.stdout.strip()


def validate_locations(source, output, work_root=None):
    source, output = source.resolve(), output.resolve()
    if source == output or source in output.parents or output in source.parents:
        raise BuildError('源码与输出目录不得包含或重叠，避免构建改写输入')
    if work_root is not None:
        work_root = work_root.resolve()
        if (work_root == source or source in work_root.parents or
                work_root == output or output in work_root.parents):
            raise BuildError('临时工作目录必须位于源码和输出目录之外')


def _excluded(relative):
    return (relative.name in EXCLUDED_NAMES or
            any(relative == Path(item) or Path(item) in relative.parents
                for item in EXCLUDED_ROOTS))


def source_entries(source):
    """不跟随目录软链，且在任何复制/编译之前拒绝指向输入树外的链接。"""
    source = source.resolve()
    def visit(directory):
        for path in sorted(directory.iterdir()):
            relative = path.relative_to(source)
            if _excluded(relative):
                continue
            if path.is_symlink():
                resolved = path.resolve()
                if resolved != source and source not in resolved.parents:
                    raise BuildError(f'源码软链接越出输入目录: {relative}')
                yield relative, 'link', os.readlink(path)
            elif path.is_dir():
                yield relative, 'dir', None
                yield from visit(path)
            elif path.is_file():
                yield relative, 'file', None
            else:
                raise BuildError(f'源码含非普通文件: {relative}')
    yield from visit(source)


def fingerprint_source(source):
    digest = hashlib.sha256()
    for relative, kind, target in source_entries(source):
        value = sha256(source / relative) if kind == 'file' else target or ''
        digest.update((str(relative) + '\0' + kind + '\0' + value + '\0').encode())
    return digest.hexdigest()


def _copy_file(source, target):
    # APFS 克隆保留独立 inode；禁止 hardlink，编译器可重写依赖文件。
    copied = False
    if sys.platform == 'darwin':
        libc = ctypes.CDLL(None, use_errno=True)
        clonefile = libc.clonefile
        clonefile.argtypes = (ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int)
        clonefile.restype = ctypes.c_int
        copied = clonefile(os.fsencode(source), os.fsencode(target), 0) == 0
    if not copied:
        shutil.copy2(source, target)


def snapshot_source(source, destination):
    source = source.resolve()
    if destination.exists():
        raise BuildError('源码快照目标必须不存在')
    entries = list(source_entries(source))  # 全部路径检查通过后再开始写。
    destination.mkdir(parents=True)
    for relative, kind, target in entries:
        original, copied = source / relative, destination / relative
        if kind == 'dir':
            copied.mkdir()
        elif kind == 'file':
            _copy_file(original, copied)
        else:
            if os.path.isabs(target):
                # 输入树内部的绝对链接也要改指向快照，避免构建写回原仓库。
                target = os.path.relpath(
                    destination / original.resolve().relative_to(source), copied.parent)
            copied.symlink_to(target)


def _safe_child(root, relative):
    candidate = root / relative
    if Path(relative).is_absolute() or '..' in Path(relative).parts:
        raise BuildError(f'补丁清单路径不合法: {relative}')
    if candidate.resolve() != root.resolve() and root.resolve() not in candidate.resolve().parents:
        raise BuildError(f'补丁路径越界: {relative}')
    return candidate


def load_patch_specs(manifests=MANIFESTS):
    specs = []
    for manifest in manifests:
        data = json.loads(manifest.read_text())
        if data.get('schema_version') != 1:
            raise BuildError(f'不支持的补丁清单版本: {manifest}')
        for entry in data['patches']:
            spec = dict(entry)
            spec['patch_file'] = _safe_child(manifest.parent, entry['path'])
            if sha256(spec['patch_file']) != entry['sha256']:
                raise BuildError(f'补丁文件哈希不匹配: {entry["path"]}')
            specs.append(spec)
    return specs


def classify_patch(source, spec):
    repository = _safe_child(source, spec['repository'])
    before, after, unknown = True, True, []
    for file in spec['files']:
        path = _safe_child(repository, file['path'])
        actual = sha256(path)
        before &= actual == file['before_sha256']
        after &= actual == file['after_sha256']
        if actual not in (file['before_sha256'], file['after_sha256']):
            unknown.append(file['path'])
    if before:
        return 'apply'
    if after:
        return 'already-applied'
    reason = ', '.join(unknown) if unknown else '存在混合的原版与修复后文件'
    raise BuildError(f'源码不匹配兼容补丁 {spec["path"]}: {reason}。'
                     f'支持基线 {spec["base_revision"]}，请先审查版本差异；不会强制覆盖。')


def apply_patch_set(workspace, specs):
    states = [classify_patch(workspace, spec) for spec in specs]
    result = []
    env = {**os.environ, 'GIT_CEILING_DIRECTORIES': str(workspace.parent)}
    for spec, state in zip(specs, states):
        if state == 'apply':
            repository = _safe_child(workspace, spec['repository'])
            args = ['git', 'apply', '--whitespace=error-all', str(spec['patch_file'])]
            run(args[:2] + ['--check'] + args[2:], cwd=repository, env=env)
            run(args, cwd=repository, env=env)
            if classify_patch(workspace, spec) != 'already-applied':
                raise BuildError(f'应用后的文件哈希不匹配: {spec["path"]}')
        result.append({key: value for key, value in spec.items() if key != 'patch_file'}
                      | {'action': state})
    return result


def source_metadata(source):
    result = {}
    for relative in ('.', 'app/src/player-comp', 'cppsrc/player_comp_native', 'cppsrc/native_core'):
        path = source / relative
        try:
            root = Path(run(['git', 'rev-parse', '--show-toplevel'], cwd=path)).resolve()
            if root != path.resolve():
                result[relative] = {'revision': None}
                continue
            result[relative] = {'revision': run(['git', 'rev-parse', 'HEAD'], cwd=path),
                                'dirty': bool(run(['git', 'status', '--porcelain',
                                                   '--untracked-files=normal'], cwd=path))}
        except BuildError:
            result[relative] = {'revision': None}
    return result


def resolve_tools(options):
    home = Path.home()
    def select(explicit, env_name, candidates, check):
        requested = explicit or os.environ.get(env_name)
        for candidate in ([Path(requested)] if requested else candidates):
            if not candidate or not candidate.is_file():
                continue
            try:
                if check(candidate):
                    return candidate.resolve()
            except BuildError:
                pass
        raise BuildError(f'找不到所需工具 {env_name}；使用对应命令行选项或环境变量指定')
    node_candidates = ([Path(shutil.which('node'))] if shutil.which('node') else [])
    node_candidates += sorted((home / 'toolchains').glob('node-v16.17.1-*/bin/node'))
    node_candidates += [home / '.nvm/versions/node/v16.17.1/bin/node']
    node = select(options.node, 'THUNDER_MAC_NODE', node_candidates,
                  lambda p: run([p, '--version']) == 'v16.17.1')
    pnpm_candidates = [home / '.node/corepack/pnpm/8.15.9/bin/pnpm.cjs',
                       home / '.cache/node/corepack/v1/pnpm/8.15.9/bin/pnpm.cjs']
    if shutil.which('pnpm'):
        pnpm_candidates.insert(0, Path(shutil.which('pnpm')))
    pnpm = select(options.pnpm_cli, 'THUNDER_MAC_PNPM_CLI', pnpm_candidates,
                  lambda p: run([node, p, '--version']) == '8.15.9')
    cmake_candidates = ([Path(shutil.which('cmake'))] if shutil.which('cmake') else [])
    cmake_candidates += sorted((home / 'toolchains').glob('cmake-*/CMake.app/Contents/bin/cmake'))
    cmake_candidates += [Path('/Applications/CMake.app/Contents/bin/cmake'),
                         Path('/opt/homebrew/bin/cmake')]
    cmake = select(options.cmake, 'THUNDER_MAC_CMAKE', cmake_candidates,
                   lambda p: run([p, '--version']).startswith('cmake version'))
    return node, pnpm, cmake


def verify_native_imports(bundle):
    app = bundle / 'Contents/Resources/app'
    results = {}
    for name in ('thunder_helper.node', 'player_helper.node', 'lkhb.node'):
        matches = list(app.rglob(name))
        if len(matches) != 1:
            raise BuildError(f'原生模块缺失或不唯一: {name}')
        symbols = run(['nm', '-u', matches[0]])
        if name.endswith('helper.node') and ('__ZN2v8' in symbols or '__ZN4node' in symbols):
            raise BuildError(f'{name} 仍依赖不兼容的 V8/Node C++ ABI')
        if name == 'lkhb.node' and '_g_xl_logger' in symbols:
            raise BuildError('lkhb.node 仍缺失 logger 定义')
        results[name] = {'sha256': sha256(matches[0]), 'imports_checked': True}
    return results


def publish(staged, output, staged_archive=None, staged_checksum=None):
    """只替换本工具成功发布的目录；目录、归档和校验文件一起回滚。"""
    try:
        staged = archive_tool.checked_output(staged)
        output = archive_tool.checked_output(output)
    except ValueError as error:
        raise BuildError(str(error)) from error
    if not staged.is_dir():
        raise BuildError('待发布产物必须是目录')
    if output.exists():
        marker = output / 'build-report.json'
        try:
            report = json.loads(marker.read_text())
            owned = (not marker.is_symlink() and isinstance(report, dict) and
                     report.get('builder') == 'xenon-thunder-macos-v1' and
                     report.get('status') == 'success')
        except (OSError, ValueError):
            owned = False
        if not owned:
            raise BuildError(f'输出目录已存在且不属于本工具，拒绝覆盖: {output}')
    pairs = [(staged, output)]
    if (staged_archive is None) != (staged_checksum is None):
        raise BuildError('归档和校验文件必须一起发布')
    if staged_archive is not None:
        archive, checksum = archive_tool.archive_paths(output)
        archive_tool.validate_existing_pair(archive, checksum)
        archive_tool.validate_existing_pair(staged_archive, staged_checksum)
        if (staged_archive.name != archive.name or
                staged_checksum.name != checksum.name):
            raise BuildError('暂存归档和校验文件名称必须与最终名称一致')
        pairs += [(staged_archive, archive), (staged_checksum, checksum)]
    archive_tool.publish_outputs(pairs)


def build(options):
    if sys.platform != 'darwin':
        raise BuildError('此入口只用于 macOS')
    source, output = options.source.resolve(), archive_tool.checked_output(options.output)
    validate_locations(source, output)
    if not (source / 'app/scripts/mac/workflow.js').is_file():
        raise BuildError('输入源码缺少当前 Mac 官方构建入口 app/scripts/mac/workflow.js；'
                         '需要近期 feature/mac_25.1.13 源码及其匹配子模块')
    specs = load_patch_specs()
    states = [classify_patch(source, spec) for spec in specs]
    node, pnpm, cmake = resolve_tools(options)
    if not (source / 'app/node_modules/.modules.yaml').is_file():
        raise BuildError('TH 源码尚未安装锁定依赖；请先准备其 Node 16 / pnpm 8.15.9 依赖和 Mac SDK')
    if options.check:
        print(json.dumps({'source': str(source), 'patches': states,
                          'node': str(node), 'pnpm': str(pnpm), 'cmake': str(cmake)}, indent=2))
        return
    output.parent.mkdir(parents=True, exist_ok=True)
    lock_path = output.parent / ('.' + output.name + '.build.lock')
    archive_tool.checked_output(lock_path)
    with lock_path.open('a') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise BuildError(f'同一输出目录已有构建: {output}') from None
        archive, checksum = archive_tool.archive_paths(output)
        archive_tool.validate_existing_pair(archive, checksum)
        work = Path(tempfile.mkdtemp(prefix='.thunder-build-', dir=output.parent))
        workspace, artifacts = work / 'source', work / 'artifacts'
        report = {'builder': 'xenon-thunder-macos-v1', 'status': 'building',
                  'product': 'Thunder for Chromium 153 (compatibility build)',
                  'upstream_unmodified_binary': False, 'source': str(source),
                  'source_metadata': source_metadata(source), 'architecture': options.arch,
                  'config': 'release', 'signing': 'ad-hoc local build',
                  'started_at': datetime.datetime.now(datetime.timezone.utc).isoformat(),
                  'tools': {'node': str(node), 'pnpm': str(pnpm), 'cmake': str(cmake)}}
        try:
            print('[1/6] 校验输入并复制独立构建快照', flush=True)
            before = fingerprint_source(source)
            report['source_fingerprint_before'] = before
            snapshot_source(source, workspace)
            report['patches'] = apply_patch_set(workspace, specs)
            env = {**os.environ,
                   'PATH': os.pathsep.join([str(node.parent), str(cmake.parent), os.environ.get('PATH', '')]),
                   'GIT_CEILING_DIRECTORIES': str(work)}
            print('[2/6] 执行 TH 官方 Release 导出（编译日志写入 build.log）', flush=True)
            with (work / 'build.log').open('w') as log:
                try:
                    run([node, workspace / 'app/scripts/mac/workflow.js', '--action', 'export',
                         '--config', 'release', '--arch', options.arch, '--sign', 'adhoc',
                         '--repo', workspace, '--node', node, '--pnpm-cli', pnpm,
                         '--output', artifacts], env=env, cwd=workspace, log=log)
                finally:
                    sanitize_upstream_build_logs(workspace)
                print('[3/6] 装配 Xenon 声明和原版插件静态资源', flush=True)
                run([node, SUPPORT / 'package_adapter.cjs', '--source', workspace,
                     '--bundle', artifacts / 'Thunder.app', '--report', work / 'packaging.json'],
                    env=env, cwd=work, log=log)
            print('[4/6] 移除 Electron 运行时并校验资源包', flush=True)
            bundle = artifacts / 'Thunder.app'
            run([sys.executable, SUPPORT / 'strip_electron_runtime.py',
                 '--bundle', bundle, '--staging-root', artifacts,
                 '--report', work / 'runtime-pruning.json'])
            # 子框架和扩展保留官方签名/entitlements，仅重签资源包自身。
            run(['codesign', '--force', '--sign', '-', bundle])
            run(['codesign', '--verify', '--deep', '--strict', bundle])
            report['native_modules'] = verify_native_imports(bundle)
            report['runtime'] = json.loads((work / 'runtime-pruning.json').read_text())
            report['bundle_sha256'] = fingerprint_source(bundle)
            report['deep_strict_codesign'] = True
            report['packaging'] = json.loads((work / 'packaging.json').read_text())
            info = plistlib.loads((bundle / 'Contents/Info.plist').read_bytes())
            write_json(artifacts / 'thunder_2025.xenon.json', {
                'application': './Thunder.app', 'name': 'Thunder',
                'version': info.get('CFBundleShortVersionString', '25.1.13'),
                'main': 'out.asar/main.js',
                'privilegedSchemes': [
                    {'scheme': 'miniapp', 'privileges': {
                        'standard': True, 'secure': True,
                        'supportFetchAPI': True, 'corsEnabled': True}},
                    {'scheme': 'crx', 'privileges': {'bypassCSP': True}},
                ],
            })
            print('[5/6] 核对输入源码保持不变', flush=True)
            after = fingerprint_source(source)
            report['source_fingerprint_after'] = after
            report['source_unchanged'] = before == after
            if before != after:
                raise BuildError('构建期间输入源码发生变化；拒绝发布，请确认没有其他程序同时修改输入')
            reports = sorted((workspace / 'dist-mac/logs').glob('*/result.json'))
            if reports:
                shutil.copy2(reports[-1], artifacts / 'upstream-build-result.json')
            shutil.copy2(work / 'build.log', artifacts / 'build.log')
            shutil.copy2(SUPPORT / 'README.md', artifacts / 'COMPATIBILITY.md')
            staged_archive = work / archive.name
            staged_checksum = work / checksum.name
            report['archive'] = archive_tool.create_archive(
                artifacts, staged_archive, staged_checksum)
            report['archive'].update(filename=archive.name, checksum=checksum.name)
            report['status'] = 'success'
            report['finished_at'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
            report['output'] = str(output / 'Thunder.app')
            write_json(artifacts / 'build-report.json', report)
            print('[6/6] 一起发布经过校验的目录、归档和 SHA256', flush=True)
            publish(artifacts, output, staged_archive, staged_checksum)
        except BaseException as error:
            report['status'] = 'failed'
            report['error'] = str(error)
            write_json(work / 'build-report.json', report)
            if isinstance(error, archive_tool.PublicationRollbackError):
                print(f'发布失败且回滚未完成；请保留备份和恢复材料: {work}', file=sys.stderr)
            else:
                print(f'构建未发布；诊断保留于 {work}', file=sys.stderr)
            raise
        else:
            if options.keep_work_dir:
                print(f'保留构建快照: {workspace}')
            else:
                try:
                    shutil.rmtree(work)
                except OSError:
                    # 产物已发布；快照清理失败仅保留诊断，不能回报构建失败。
                    print(f'警告：产物已发布，工作目录尚未清理: {work}', file=sys.stderr)
            print(f'完成: {output / "Thunder.app"}\n说明与补丁记录: {output / "build-report.json"}')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True, help='TH 源码根目录；只读')
    parser.add_argument('--output', type=Path,
                        default=Path(__file__).resolve().parents[1] / 'resources/thunder-2025-mac',
                        help='产物目录，默认 xenon_overlay/resources/thunder-2025-mac')
    parser.add_argument('--arch', choices=('arm64', 'x64'),
                        default='arm64' if platform.machine() == 'arm64' else 'x64')
    parser.add_argument('--node', help='Node 16.17.1 可执行文件')
    parser.add_argument('--pnpm-cli', help='pnpm 8.15.9 CLI 文件')
    parser.add_argument('--cmake', help='CMake 可执行文件')
    parser.add_argument('--check', action='store_true', help='只检查源码补丁和工具版本，不构建')
    parser.add_argument('--keep-work-dir', action='store_true', help='成功后保留构建快照供审查')
    try:
        build(parser.parse_args(argv))
        return 0
    except (RuntimeError, OSError, ValueError, tarfile.TarError) as error:
        print(f'错误: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
