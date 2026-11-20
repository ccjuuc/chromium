#!/usr/bin/env python3
# Copyright 2026 The Xenon Overlay Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Download official macOS releases of draw.io, MarkText, and Motrix,
inspect their bundle structure, and test loading/running them in Xenon on macOS.
"""

import concurrent.futures
import json
import os
from pathlib import Path
import plistlib
import shutil
import signal
import subprocess
import sys
import time

APPS = {
    'drawio': {
        'name': 'draw.io',
        'version': '32.6.10',
        'url': 'https://github.com/jgraph/drawio-desktop/releases/download/v32.6.10/draw.io-arm64-32.6.10.dmg',
        'dmg_name': 'draw.io-arm64-32.6.10.dmg',
        'app_bundle_name': 'draw.io.app',
    },
    'marktext': {
        'name': 'MarkText',
        'version': '0.23.0',
        'url': 'https://github.com/marktext/marktext/releases/download/v0.23.0/marktext-mac-arm64-0.23.0.dmg',
        'dmg_name': 'marktext-mac-arm64-0.23.0.dmg',
        'app_bundle_name': 'MarkText.app',
    },
    'motrix': {
        'name': 'Motrix',
        'version': '2.4.1',
        'url': 'https://github.com/agalwood/Motrix/releases/download/v2.4.1/Motrix-2.4.1-arm64.dmg',
        'dmg_name': 'Motrix-2.4.1-arm64.dmg',
        'app_bundle_name': 'Motrix.app',
    },
}

BASE_DIR = Path(__file__).resolve().parent.parent / 'out' / 'mac_app_validation'
DOWNLOAD_DIR = BASE_DIR / 'downloads'
APPS_DIR = BASE_DIR / 'apps'
MNT_DIR = BASE_DIR / 'mnt'
RESULTS_DIR = BASE_DIR / 'results'
CHROMIUM_BIN = Path(__file__).resolve().parent.parent / 'out' / 'debug_arm64' / 'Chromium.app' / 'Contents' / 'MacOS' / 'Chromium'


def download_dmg(key, info):
    dest = DOWNLOAD_DIR / info['dmg_name']
    if dest.exists() and dest.stat().st_size > 10 * 1024 * 1024:
        print(f"[{key}] DMG already downloaded: {dest} ({dest.stat().st_size} bytes)")
        return dest
    print(f"[{key}] Downloading {info['url']} -> {dest}...")
    cmd = ['curl', '-L', '-C', '-', '-o', str(dest), info['url']]
    res = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    if res.returncode != 0:
        raise RuntimeError(f"Download failed for {key}: {res.stderr.decode('utf-8', 'replace')}")
    print(f"[{key}] Download complete ({dest.stat().st_size} bytes)")
    return dest


def extract_app(key, info, dmg_path):
    target_app = APPS_DIR / info['app_bundle_name']
    if target_app.exists() and (target_app / 'Contents' / 'Resources').exists():
        print(f"[{key}] App already extracted at {target_app}")
        return target_app

    mountpoint = MNT_DIR / key
    mountpoint.mkdir(parents=True, exist_ok=True)
    # Ensure any existing mount is unmounted
    subprocess.run(['hdiutil', 'detach', str(mountpoint), '-force'],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    print(f"[{key}] Mounting {dmg_path} at {mountpoint}...")
    attach_cmd = ['hdiutil', 'attach', str(dmg_path), '-mountpoint', str(mountpoint), '-nobrowse', '-readonly']
    res = subprocess.run(attach_cmd, capture_output=True, text=True)
    if res.returncode != 0:
        raise RuntimeError(f"Failed to attach DMG for {key}: {res.stderr}")

    try:
        source_app = mountpoint / info['app_bundle_name']
        if not source_app.exists():
            # Search for any *.app
            apps = list(mountpoint.glob('*.app'))
            if apps:
                source_app = apps[0]
            else:
                raise RuntimeError(f"No .app found in mounted DMG {mountpoint}")

        print(f"[{key}] Extracting essential app bundle to {target_app}...")
        if target_app.exists():
            shutil.rmtree(target_app)
        target_app.mkdir(parents=True, exist_ok=True)

        # Copy Contents/Info.plist and Contents/Resources
        src_contents = source_app / 'Contents'
        dst_contents = target_app / 'Contents'
        dst_contents.mkdir(parents=True, exist_ok=True)

        info_plist = src_contents / 'Info.plist'
        if info_plist.exists():
            shutil.copy2(info_plist, dst_contents / 'Info.plist')

        src_resources = src_contents / 'Resources'
        if src_resources.exists():
            shutil.copytree(src_resources, dst_contents / 'Resources', symlinks=True)

        # Also copy macOS binary if exists (e.g. for inspection)
        src_macos = src_contents / 'MacOS'
        if src_macos.exists():
            dst_macos = dst_contents / 'MacOS'
            dst_macos.mkdir(parents=True, exist_ok=True)
            for item in src_macos.iterdir():
                if item.is_file():
                    shutil.copy2(item, dst_macos / item.name)

        print(f"[{key}] Extracted {target_app}")
        return target_app
    finally:
        subprocess.run(['hdiutil', 'detach', str(mountpoint), '-force'],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def inspect_app_structure(key, app_path):
    contents = app_path / 'Contents'
    resources = contents / 'Resources'
    info_plist = contents / 'Info.plist'

    info = {}
    if info_plist.exists():
        try:
            info = plistlib.loads(info_plist.read_bytes())
        except Exception as e:
            info = {'error': str(e)}

    # Scan for native modules (.node, Mach-O binaries)
    native_nodes = []
    executables = []
    asar_present = (resources / 'app.asar').exists()
    asar_size = (resources / 'app.asar').stat().st_size if asar_present else 0

    for root, _, files in os.walk(resources):
        for f in files:
            p = Path(root) / f
            rel = str(p.relative_to(resources))
            if f.endswith('.node'):
                native_nodes.append(rel)
            elif os.access(p, os.X_OK) and not f.endswith(('.sh', '.js', '.json', '.html', '.css', '.svg', '.png')):
                executables.append(rel)

    return {
        'name': info.get('CFBundleName', key),
        'version': info.get('CFBundleShortVersionString', info.get('CFBundleVersion', 'unknown')),
        'identifier': info.get('CFBundleIdentifier', 'unknown'),
        'asar_present': asar_present,
        'asar_size_bytes': asar_size,
        'native_nodes': native_nodes,
        'executables': executables,
    }


def test_run_in_xenon(key, app_path):
    print(f"[{key}] Launching in Xenon host...")
    profile_dir = BASE_DIR / 'profiles' / key
    profile_dir.mkdir(parents=True, exist_ok=True)
    log_file = RESULTS_DIR / f"{key}_xenon.log"

    cmd = [
        str(CHROMIUM_BIN),
        '--headless=new',
        '--remote-debugging-port=0',
        f'--user-data-dir={profile_dir}',
        '--no-first-run',
        '--no-default-browser-check',
        '--disable-gpu',
        '--enable-logging=stderr',
        f'--xenon-electron-app={app_path}',
    ]

    with log_file.open('w') as out:
        proc = subprocess.Popen(cmd, stdout=out, stderr=subprocess.STDOUT, text=True)
        # Let it run for 8 seconds to initialize container, load main script, and dispatch initial events
        try:
            time.sleep(8)
        finally:
            if proc.poll() is None:
                proc.send_signal(signal.SIGTERM)
                try:
                    proc.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()

    output = log_file.read_text(encoding='utf-8', errors='replace')
    container_registered = "Registered Electron container configuration: default" in output or \
                           "Registered Electron container configuration" in output
    container_launched = "Launched isolated Electron container service" in output
    window_created = "XenonElectronWindowHost" in output or "waiting for Electron application window" in output or "CreateWindow" in output

    errors = []
    for line in output.splitlines():
        if "Uncaught " in line or "Error: " in line or "ERR_" in line or "Failed to load" in line:
            errors.append(line.strip())

    return {
        'container_registered': container_registered,
        'container_launched': container_launched,
        'window_created': window_created,
        'relevant_errors': errors[:10],
        'log_path': str(log_file),
    }


def main():
    DOWNLOAD_DIR.mkdir(parents=True, exist_ok=True)
    APPS_DIR.mkdir(parents=True, exist_ok=True)
    RESULTS_DIR.mkdir(parents=True, exist_ok=True)

    print("=== Step 1: Downloading macOS DMGs for the 3 apps ===")
    with concurrent.futures.ThreadPoolExecutor(max_workers=3) as executor:
        futures = {executor.submit(download_dmg, key, info): key for key, info in APPS.items()}
        dmg_paths = {}
        for future in concurrent.futures.as_completed(futures):
            key = futures[future]
            try:
                dmg_paths[key] = future.result()
            except Exception as e:
                print(f"Error downloading {key}: {e}")
                return 1

    print("\n=== Step 2: Extracting macOS application bundles ===")
    app_paths = {}
    for key, info in APPS.items():
        try:
            app_paths[key] = extract_app(key, info, dmg_paths[key])
        except Exception as e:
            print(f"Error extracting {key}: {e}")
            return 1

    print("\n=== Step 3: Inspecting app structures & native dependencies ===")
    inspection = {}
    for key, app_path in app_paths.items():
        inspection[key] = inspect_app_structure(key, app_path)
        print(f"[{key}] Structure: ASAR={inspection[key]['asar_present']} ({inspection[key]['asar_size_bytes']} bytes), Native nodes={len(inspection[key]['native_nodes'])}, Binaries={len(inspection[key]['executables'])}")
        if inspection[key]['native_nodes']:
            print(f"      Native nodes: {inspection[key]['native_nodes']}")
        if inspection[key]['executables']:
            print(f"      Executables: {inspection[key]['executables']}")

    print("\n=== Step 4: Testing execution in macOS Xenon Chromium host ===")
    test_results = {}
    for key, app_path in app_paths.items():
        test_results[key] = test_run_in_xenon(key, app_path)
        print(f"[{key}] Registered: {test_results[key]['container_registered']}, Launched: {test_results[key]['container_launched']}, Errors: {len(test_results[key]['relevant_errors'])}")

    report = {
        'timestamp': time.strftime('%Y-%m-%dT%H:%M:%SZ'),
        'apps': APPS,
        'inspection': inspection,
        'test_results': test_results,
    }

    report_path = BASE_DIR / 'validation_report.json'
    report_path.write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(f"\nValidation complete! Report written to {report_path}")
    return 0


if __name__ == '__main__':
    sys.exit(main())
