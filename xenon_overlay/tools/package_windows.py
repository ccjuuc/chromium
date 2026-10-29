#!/usr/bin/env python3
# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Build/repackage the Windows installer, then verify its complete archive chain."""

import argparse
import datetime
import json
from pathlib import Path, PureWindowsPath
import re
import shutil
import subprocess
import sys
import uuid

from import_electron_app import check_ancestors
from verify_windows_installer import verify


ROOT = Path(__file__).resolve().parents[2]


def generated_installer(build):
    path = build / 'obj/chrome/installer/mini_installer/mini_installer.ninja'
    body = path.read_text(encoding='utf-8')
    values = dict(re.findall(r'^[ \t]*(target_output_name|output_dir|output_extension) = (.+)$',
                             body, re.MULTILINE))
    name = values.get('target_output_name', '')
    if not re.fullmatch(r'[\w.-]+', name) or values.get('output_extension') != '.exe':
        raise ValueError('Unsupported generated installer name/extension')
    relative = Path(values['output_dir']) / (name + '.exe')
    if relative.is_absolute() or relative.drive or '..' in relative.parts:
        raise ValueError('Installer output must be inside the build directory')
    return relative, body


def repack_graph(toolchain, body):
    """Bounded adapter for GN's resource/link-only mini-installer graph.

    Existing object/library files remain real Ninja inputs. Only order-only
    phony build prerequisites are omitted in explicit repack-existing mode.
    A changed/unknown GN layout fails instead of silently dropping more edges.
    """
    if re.search(r'^(include|subninja)\s', body, re.MULTILINE):
        raise ValueError('Unsupported installer graph includes')
    edges = re.findall(r'^build .+?: (\w+)\b', body, re.MULTILINE)
    if edges != ['rc', 'link'] or 'packed_files.rc' not in body:
        raise ValueError('Expected exactly the packed-resource and installer-link edges')
    rules = []
    for name in ('rc', 'link'):
        matches = re.findall(r'^rule ' + name + r'\n(?:[ \t].*\n)*', toolchain, re.MULTILINE)
        if len(matches) != 1:
            raise ValueError(f'Expected one generated {name} rule')
        rules.append(matches[0])
    pools = set(re.findall(r'^  pool = (\w+)$', ''.join(rules), re.MULTILINE))
    if 'console' in pools:
        raise ValueError('Unexpected interactive rule in installer graph')
    header = ''.join(f'pool {name}\n  depth = 1\n' for name in sorted(pools))
    lines = [line.split(' || ', 1)[0] if line.startswith('build ') else line
             for line in body.splitlines()]
    return header + ''.join(rules) + '\n'.join(lines) + '\n'


def archive_command(toolchain):
    commands = [line.strip()[len('command = '):] for line in toolchain.splitlines()
                if line.lstrip().startswith('command = ') and
                'create_xenon_installer_archive.py --build_dir' in line]
    if len(commands) != 1:
        raise ValueError('Expected one Xenon installer archive action; enable_xenon_service is required')
    # Decode only Ninja literal escapes, never shell-expand this command.
    command = re.sub(r'\$([$: ])', lambda match: match[1], commands[0])
    if '$' in command or '--xenon-packaging-manifest=' not in command:
        raise ValueError('Unsupported or stale generated archive action; regenerate GN files')
    executable = re.match(r'(?:"([^"]+)"|(\S+))', command)
    if (not executable or PureWindowsPath(executable[1] or executable[2]).name.lower()
            not in ('python.exe', 'python3.exe')):
        raise ValueError('Archive action must directly invoke the configured Python executable')
    return command


def package(build, output, mode='build', sevenzip=None):
    build, output = Path(build).absolute(), Path(output).absolute()
    check_ancestors(build)
    check_ancestors(output)
    if not (build / 'args.gn').is_file():
        raise ValueError('Generate the Windows build directory with GN first')
    output.mkdir(parents=True, exist_ok=False)
    report = {'passed': False, 'mode': mode, 'commands': []}
    backup = None
    installer = None

    def run(label, command):
        print(f'[package] {label}', flush=True)
        report['commands'].append({'stage': label, 'command': command})
        with (output / (label + '.log')).open('wb') as log:
            subprocess.run(command, cwd=build, stdout=log, stderr=subprocess.STDOUT, check=True)

    try:
        ninja = str(ROOT / 'third_party/ninja/ninja.exe')
        if mode != 'verify':
            if sys.platform != 'win32':
                raise ValueError('Building/repackaging Windows installers requires Windows')
            run('generate', [str(ROOT / 'buildtools/win/gn.exe'), 'gen', str(build),
                             '--script-executable=' + sys.executable])
        relative, body = generated_installer(build)
        installer = build / relative
        check_ancestors(installer)
        if mode == 'build':
            launcher = shutil.which('autoninja')
            helper = Path(launcher).with_name('autoninja.py') if launcher else None
            command = ([sys.executable, str(helper)] if helper and helper.is_file()
                       else [ninja])
            run('build-installer', [*command, '-C', str(build), 'mini_installer'])
        elif mode == 'repack':
            report['scope'] = 'Reuses existing browser/object files; does not establish source-to-binary freshness.'
            toolchain = (build / 'toolchain.ninja').read_text(encoding='utf-8')
            graph = output / 'installer-only.ninja'
            graph.write_text(repack_graph(toolchain, body), encoding='utf-8')
            action = archive_command(toolchain)
            link = [ninja, '-C', str(build), '-f', str(graph), relative.as_posix()]
            # Missing objects/libraries fail before replacing an archive.
            run('preflight-link', [*link, '-n'])
            if installer.exists():
                backup = output / 'previous-installer.exe'
                shutil.copy2(installer, backup)
            run('create-archive', action)
            run('link-installer', link)
        elif mode != 'verify':
            raise ValueError(f'Unknown packaging mode: {mode}')
        print('[package] verify-installer', flush=True)
        verification = verify(build, installer, output / 'verification', sevenzip)
        report['verification'] = str(output / 'verification/report.json')
        report['passed'] = verification['passed']
        if not verification['passed']:
            raise ValueError(verification.get('error', 'Installer verification failed'))
        report['installer'] = verification['installer']
    except (OSError, ValueError, subprocess.CalledProcessError, KeyboardInterrupt) as error:
        report['error'] = 'Packaging interrupted' if isinstance(error, KeyboardInterrupt) else str(error)
        if backup is not None:
            shutil.copy2(backup, installer)
            report['previous_installer_restored'] = True
    finally:
        (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'out/Release_64')
    parser.add_argument('--output-dir', type=Path, help='New report directory; default: build/packaging-reports/<unique>')
    parser.add_argument('--sevenzip', type=Path, help='Optional compatible 7-Zip CLI')
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument('--repack-existing', action='store_true', help='Reuse compiled binaries; only archive and relink the installer')
    modes.add_argument('--verify-only', action='store_true', help='Read-only validation of an existing package and manifest')
    args = parser.parse_args()
    mode = 'verify' if args.verify_only else 'repack' if args.repack_existing else 'build'
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ')
    output = args.output_dir or args.build_dir / 'packaging-reports' / (stamp + '-' + uuid.uuid4().hex[:8])
    try:
        report = package(args.build_dir, output, mode, args.sevenzip)
    except (OSError, ValueError) as error:
        parser.exit(1, f'error: {error}\n')
    print(json.dumps(report))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
