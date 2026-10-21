#!/usr/bin/env node
// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/**
 * macOS component installer (.pkg) builder.
 *
 * Wraps an .app with pkgbuild so Installer.app can place it in /Applications.
 * Version comes from CFBundleShortVersionString. CFBundleVersion is only the
 * last two components on this product and is not used as the package version.
 *
 * This pkg is a distribution installer. The in-app updater still consumes
 * patch.zip / package.zip, not this pkg.
 */

import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
const rootDir = path.resolve(__dirname, '..', '..', '..');

function usage() {
  return `Usage: node xenon_overlay/tools/updater/make_mac_pkg.js --app <App.app> [options]

Options:
  --app <path>                 Application bundle to package (required)
  --out <path>                 Output .pkg. Default: test_packages/<name>_installer_<version>.pkg
  --identifier <id>            Package id. Default: com.<appname>.installer
                               Do not reuse org.chromium.Chromium: Installer
                               will treat 1.x as older than an existing Chromium.app.
  --version <major.minor.build.patch>
                               Package version. Default: CFBundleShortVersionString
  --install-location <path>    Install root. Default: /Applications
  --sign <identity>            Installer signing identity passed to pkgbuild
`;
}

function fail(message) {
  console.error(`[ERROR] ${message}`);
  process.exit(1);
}

function parseArgs(argv) {
  const options = {
    app: '',
    out: '',
    identifier: '',
    version: '',
    installLocation: '/Applications',
    sign: '',
  };
  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];
    const next = argv[i + 1];
    if (arg === '--help' || arg === '-h') {
      console.log(usage());
      process.exit(0);
    } else if (arg === '--app' && next) {
      options.app = next;
      i++;
    } else if (arg === '--out' && next) {
      options.out = next;
      i++;
    } else if (arg === '--identifier' && next) {
      options.identifier = next;
      i++;
    } else if (arg === '--version' && next) {
      options.version = next;
      i++;
    } else if (arg === '--install-location' && next) {
      options.installLocation = next;
      i++;
    } else if (arg === '--sign' && next) {
      options.sign = next;
      i++;
    } else {
      fail(`Unknown argument: ${arg}\n${usage()}`);
    }
  }
  return options;
}

function readPlistString(plistPath, key) {
  const result = spawnSync('plutil', ['-extract', key, 'raw', plistPath], {
    encoding: 'utf8',
  });
  if (result.status !== 0) {
    return '';
  }
  return (result.stdout || '').trim();
}

function sha256File(filePath) {
  return new Promise((resolve, reject) => {
    const hash = crypto.createHash('sha256');
    const stream = fs.createReadStream(filePath);
    stream.on('data', (chunk) => hash.update(chunk));
    stream.on('error', reject);
    stream.on('end', () => resolve(hash.digest('hex').toUpperCase()));
  });
}

function main() {
  const options = parseArgs(process.argv.slice(2));
  if (!options.app) {
    fail(`Missing --app\n${usage()}`);
  }
  if (process.platform !== 'darwin') {
    fail('pkgbuild is only available on macOS');
  }

  const appPath = path.resolve(options.app);
  const plistPath = path.join(appPath, 'Contents', 'Info.plist');
  if (!fs.existsSync(plistPath)) {
    fail(`Not an application bundle (missing Contents/Info.plist): ${appPath}`);
  }

  const bundleName = readPlistString(plistPath, 'CFBundleName') ||
      path.basename(appPath, '.app');
  const version = options.version ||
      readPlistString(plistPath, 'CFBundleShortVersionString');
  const packageId = options.identifier ||
      `com.${bundleName.toLowerCase()}.installer`;
  if (!version) {
    fail('CFBundleShortVersionString is empty. Pass --version.');
  }
  if (!/^\d+(\.\d+){3}$/.test(version)) {
    fail(`Package version must be MAJOR.MINOR.BUILD.PATCH, got "${version}"`);
  }

  const outputPath = path.resolve(
      options.out ||
      path.join(rootDir, 'test_packages',
                `${bundleName}_installer_${version}.pkg`));
  fs.mkdirSync(path.dirname(outputPath), { recursive: true });
  if (fs.existsSync(outputPath)) {
    fs.unlinkSync(outputPath);
  }

  const stage = fs.mkdtempSync(path.join(os.tmpdir(), 'xl153-pkg-'));
  const payloadRoot = path.join(stage, 'root');
  const componentPlist = path.join(stage, 'components.plist');
  fs.mkdirSync(payloadRoot);
  const stagedApp = path.join(payloadRoot, `${bundleName}.app`);
  const ditto = spawnSync('ditto', [appPath, stagedApp], { stdio: 'inherit' });
  if (ditto.status !== 0) {
    fail('ditto failed while staging the application bundle');
  }
  const analyzed = spawnSync(
      'pkgbuild', ['--analyze', '--root', payloadRoot, componentPlist],
      { stdio: 'inherit' });
  if (analyzed.status !== 0) {
    fail('pkgbuild --analyze failed');
  }
  // An existing Chromium.app with the same bundle id and a higher
  // CFBundleVersion (153.x) makes Installer skip or relocate this package.
  const edited = spawnSync('python3', ['-c', `
import plistlib
path = ${JSON.stringify(componentPlist)}
with open(path, 'rb') as fh:
    items = plistlib.load(fh)
for item in items:
    item['BundleIsRelocatable'] = False
    item['BundleIsVersionChecked'] = False
    item['BundleHasStrictIdentifier'] = False
    item['BundleOverwriteAction'] = 'upgrade'
with open(path, 'wb') as fh:
    plistlib.dump(items, fh)
`], { stdio: 'inherit' });
  if (edited.status !== 0) {
    fail('Failed to disable bundle relocation in the component plist');
  }

  const pkgbuildArgs = [
    '--root', payloadRoot,
    '--component-plist', componentPlist,
    '--install-location', options.installLocation,
    '--identifier', packageId,
    '--version', version,
    '--ownership', 'recommended',
  ];
  if (options.sign) {
    pkgbuildArgs.push('--sign', options.sign);
  }
  pkgbuildArgs.push(outputPath);

  console.log(`[*] Packaging ${bundleName} ${version}`);
  console.log(`    App:        ${appPath}`);
  console.log(`    Package id: ${packageId}`);
  console.log(`    Install to: ${options.installLocation}`);
  console.log(`    Output:     ${outputPath}`);

  const result = spawnSync('pkgbuild', pkgbuildArgs, { stdio: 'inherit' });
  if (result.error && result.error.code === 'ENOENT') {
    fail('pkgbuild was not found. Install Xcode command line tools.');
  }
  if (result.status !== 0) {
    fail(`pkgbuild failed with code ${result.status}`);
  }

  return sha256File(outputPath).then((sha) => {
    const size = fs.statSync(outputPath).size;
    console.log('\n[+] Installer package created');
    console.log(`    File:   ${outputPath}`);
    console.log(`    Size:   ${(size / 1024 / 1024).toFixed(2)} MB (${size} bytes)`);
    console.log(`    SHA256: ${sha}`);
  });
}

main().catch((err) => fail(err.message));
