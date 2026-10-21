#!/usr/bin/env node
// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/** Build a macOS system-domain distribution PKG for /Applications. */
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const rootDir = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../../..');

function usage() {
  return `Usage: node xenon_overlay/tools/updater/make_mac_pkg.js --app <App.app> [options]

Options:
  --app <path>                 Application bundle to package (required)
  --out <path>                 Default: test_packages/<name>_installer_<version>.pkg
  --identifier <id>            Default: com.<appname>.installer
  --version <major.minor.build.patch>
                               Must match the app's CFBundleShortVersionString
  --install-location /Applications
                               Only supported location, in the localSystem domain
  --sign <identity>            Sign the final product archive with productbuild
`;
}

function runChecked(run, tool, args) {
  const result = run(tool, args, { encoding: 'utf8' });
  if (result.error || result.status !== 0) {
    throw new Error(`${tool} failed: ${result.error?.message || result.stderr || `exit ${result.status}`}`);
  }
  return result;
}

function readPlistString(run, plist, key) {
  return runChecked(run, 'plutil', ['-extract', key, 'raw', '-expect', 'string', plist]).stdout.trim();
}

function escapeXml(value) {
  return value.replace(/[&<>"']/g, character => ({
    '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&apos;'
  })[character]);
}

function systemDistribution(bundleName, bundleIdentifier, identifier, version) {
  return `<?xml version="1.0" encoding="UTF-8"?>
<installer-gui-script minSpecVersion="2">
  <title>${escapeXml(bundleName)}</title>
  <options customize="never" require-scripts="false" hostArchitectures="arm64,x86_64"/>
  <domains enable_currentUserHome="false" enable_localSystem="true" enable_anywhere="false"/>
  <choices-outline><line choice="install"/></choices-outline>
  <choice id="install" title="${escapeXml(bundleName)}" visible="false">
    <pkg-ref id="${escapeXml(identifier)}"/>
  </choice>
  <pkg-ref id="${escapeXml(identifier)}" version="${escapeXml(version)}">component.pkg</pkg-ref>
  <pkg-ref id="${escapeXml(identifier)}">
    <must-close><app id="${escapeXml(bundleIdentifier)}"/></must-close>
  </pkg-ref>
</installer-gui-script>
`;
}

function sha256File(file) {
  return new Promise((resolve, reject) => {
    const hash = crypto.createHash('sha256');
    const stream = fs.createReadStream(file);
    stream.on('data', chunk => hash.update(chunk));
    stream.on('error', reject);
    stream.on('end', () => resolve(hash.digest('hex').toUpperCase()));
  });
}

function canonicalOutput(output) {
  let ancestor = output;
  const suffix = [];
  while (!fs.existsSync(ancestor)) {
    suffix.unshift(path.basename(ancestor));
    ancestor = path.dirname(ancestor);
  }
  return path.join(fs.realpathSync(ancestor), ...suffix);
}

export async function buildMacPkg(options, { run = spawnSync, platform = process.platform,
                                            tempRoot = os.tmpdir(), projectRoot = rootDir } = {}) {
  if (!options.app) throw new Error('Missing --app');
  if (platform !== 'darwin') throw new Error('pkgbuild and productbuild are only available on macOS');
  const app = fs.realpathSync(options.app);
  const plist = path.join(app, 'Contents/Info.plist');
  if (!fs.statSync(app).isDirectory() || !fs.existsSync(plist)) {
    throw new Error(`Not an application bundle (missing Contents/Info.plist): ${app}`);
  }
  const version = readPlistString(run, plist, 'CFBundleShortVersionString');
  if (!/^(0|[1-9]\d*)(\.(0|[1-9]\d*)){3}$/.test(version) ||
      version.split('.').some(part => BigInt(part) > 0xffffffffn)) {
    throw new Error(`CFBundleShortVersionString must be MAJOR.MINOR.BUILD.PATCH, got "${version}"`);
  }
  if (options.version && options.version !== version) {
    throw new Error(`Requested package version ${options.version} differs from app version ${version}`);
  }
  // Installer matches running applications by their bundle ID, not the
  // component package's receipt ID. Never publish a package without this guard.
  const bundleIdentifier = readPlistString(run, plist, 'CFBundleIdentifier');
  if (!bundleIdentifier) throw new Error('CFBundleIdentifier must be a non-empty string');
  const nameResult = run('plutil', ['-extract', 'CFBundleName', 'raw', plist], { encoding: 'utf8' });
  const bundleName = (nameResult.status === 0 && nameResult.stdout.trim()) || path.basename(app, '.app');
  if (bundleName === '.' || bundleName === '..' || /[/\\\0]/.test(bundleName)) {
    throw new Error(`Invalid CFBundleName: ${bundleName}`);
  }
  const identifier = options.identifier || `com.${bundleName.toLowerCase()}.installer`;
  const installLocation = options.installLocation || '/Applications';
  // System packages always install into the boot volume's Applications folder.
  if (installLocation !== '/Applications') {
    throw new Error('System packages require --install-location /Applications');
  }
  const output = canonicalOutput(path.resolve(options.out || path.join(projectRoot,
    'test_packages', `${bundleName}_installer_${version}.pkg`)));
  const relativeOutput = path.relative(app, output);
  if (!relativeOutput || (!relativeOutput.startsWith(`..${path.sep}`) &&
      relativeOutput !== '..' && !path.isAbsolute(relativeOutput))) {
    throw new Error('Output must be outside the input application');
  }

  const stage = fs.mkdtempSync(path.join(tempRoot, 'xenon-pkg-'));
  let publishTemp;
  try {
    const payloadRoot = path.join(stage, 'root');
    const components = path.join(stage, 'components.plist');
    const componentPath = path.join(stage, 'component.pkg');
    const distribution = path.join(stage, 'Distribution');
    const packagePath = path.join(stage, 'installer.pkg');
    fs.mkdirSync(payloadRoot);
    runChecked(run, 'ditto', [app, path.join(payloadRoot, `${bundleName}.app`)]);
    runChecked(run, 'pkgbuild', ['--analyze', '--root', payloadRoot, components]);
    // A shared Chromium bundle ID must not relocate this product or suppress
    // installation based on Chromium's unrelated CFBundleVersion.
    runChecked(run, 'python3', ['-c', `
import plistlib
import sys
path = sys.argv[1]
with open(path, 'rb') as fh:
    items = plistlib.load(fh)
for item in items:
    item['BundleIsRelocatable'] = False
    item['BundleIsVersionChecked'] = False
    item['BundleHasStrictIdentifier'] = False
    item['BundleOverwriteAction'] = 'upgrade'
with open(path, 'wb') as fh:
    plistlib.dump(items, fh)
`, components]);
    const args = ['--root', payloadRoot, '--component-plist', components,
      '--install-location', installLocation, '--identifier', identifier,
      '--version', version, '--ownership', 'recommended'];
    args.push(componentPath);
    runChecked(run, 'pkgbuild', args);
    fs.writeFileSync(distribution, systemDistribution(bundleName, bundleIdentifier, identifier, version));
    const productArgs = ['--distribution', distribution, '--package-path', stage,
      '--identifier', identifier, '--version', version];
    if (options.sign) productArgs.push('--sign', options.sign);
    productArgs.push(packagePath);
    runChecked(run, 'productbuild', productArgs);
    const metadata = fs.statSync(packagePath);
    if (!metadata.isFile() || metadata.size === 0) throw new Error('productbuild produced an empty package');
    const sha256 = await sha256File(packagePath);
    fs.mkdirSync(path.dirname(output), { recursive: true });
    publishTemp = path.join(path.dirname(output), `.xenon-pkg-${crypto.randomUUID()}.tmp`);
    fs.copyFileSync(packagePath, publishTemp, fs.constants.COPYFILE_EXCL);
    // Complete staging cleanup before replacing an existing distribution file.
    fs.rmSync(stage, { recursive: true, force: true });
    fs.renameSync(publishTemp, output);
    return { output, version, bundleName, bundleIdentifier, identifier, installLocation,
      installDomain: 'localSystem', ownership: 'root:wheel', size: metadata.size, sha256 };
  } finally {
    try {
      if (publishTemp) fs.rmSync(publishTemp, { force: true });
    } finally {
      fs.rmSync(stage, { recursive: true, force: true });
    }
  }
}

function parseArgs(argv) {
  const options = {};
  const names = { '--app': 'app', '--out': 'out', '--identifier': 'identifier',
    '--version': 'version', '--install-location': 'installLocation', '--sign': 'sign' };
  for (let i = 0; i < argv.length; i++) {
    const key = names[argv[i]];
    if (!key || !argv[i + 1] || argv[i + 1].startsWith('--')) {
      throw new Error(`Unknown or incomplete argument: ${argv[i]}\n${usage()}`);
    }
    options[key] = argv[++i];
  }
  return options;
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  try {
    if (process.argv.slice(2).some(arg => arg === '--help' || arg === '-h')) {
      console.log(usage());
    } else {
      const result = await buildMacPkg(parseArgs(process.argv.slice(2)));
      console.log(`[+] Installer package created: ${result.output}`);
      console.log(`    ${result.bundleName} ${result.version}, ${result.size} bytes`);
      console.log(`    Package id: ${result.identifier}, install root: ${result.installLocation} (${result.installDomain}, ${result.ownership})`);
      console.log(`    SHA256: ${result.sha256}`);
    }
  } catch (error) {
    console.error(`[ERROR] ${error.message}`);
    process.exitCode = 1;
  }
}
