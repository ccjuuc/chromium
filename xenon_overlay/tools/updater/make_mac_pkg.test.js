// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
import assert from 'node:assert/strict';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';
import { spawnSync } from 'node:child_process';
import { buildMacPkg } from './make_mac_pkg.js';

function fixture(t, { failAt, version = '1.0.0.2', bundleName = 'xl153',
                     bundleIdentifier = 'org.chromium.Chromium', emptyPackage = false } = {}) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'xenon-pkg-test-'));
  t.after(() => fs.rmSync(root, { recursive: true, force: true }));
  const app = path.join(root, 'xl153.app');
  const tempRoot = path.join(root, 'temp');
  fs.mkdirSync(path.join(app, 'Contents'), { recursive: true });
  fs.mkdirSync(tempRoot);
  fs.writeFileSync(path.join(app, 'Contents/Info.plist'), 'original plist');
  const output = path.join(root, 'installer.pkg');
  fs.writeFileSync(output, 'previous package');
  const calls = [];
  const run = (tool, args) => {
    const call = { tool, args };
    calls.push(call);
    const phase = tool === 'pkgbuild' ? (args.includes('--analyze') ? 'analyze' : 'build') : tool;
    if (phase === failAt) {
      if (tool === 'productbuild') fs.writeFileSync(args.at(-1), 'partial failed product');
      return { status: 1, stderr: `${phase} failed` };
    }
    if (tool === 'plutil') {
      const value = { CFBundleName: bundleName, CFBundleIdentifier: bundleIdentifier,
        CFBundleShortVersionString: version }[args[1]];
      if (value === null) return { status: 1, stderr: `${args[1]} missing or not a string` };
      return { status: 0, stdout: `${value}\n` };
    }
    if (tool === 'ditto') fs.cpSync(args[0], args[1], { recursive: true });
    if (phase === 'analyze') fs.writeFileSync(args.at(-1), 'mock component plist');
    if (phase === 'build') fs.writeFileSync(args.at(-1), 'component package bytes');
    if (tool === 'productbuild') {
      call.distribution = fs.readFileSync(args[args.indexOf('--distribution') + 1], 'utf8');
      fs.writeFileSync(args.at(-1), emptyPackage ? '' : 'new package bytes');
    }
    return { status: 0, stdout: '' };
  };
  return { root, app, tempRoot, output, calls,
    options: { app, out: output }, dependencies: { run, tempRoot, platform: 'darwin', projectRoot: root } };
}

test('successful packaging publishes the result and cleans staging without editing the app', async t => {
  const f = fixture(t);
  const identity = 'Developer ID Installer: Test Identity';
  const result = await buildMacPkg({ ...f.options, sign: identity, version: '1.0.0.2' }, f.dependencies);
  assert.equal(fs.readFileSync(f.output, 'utf8'), 'new package bytes');
  assert.equal(result.version, '1.0.0.2');
  assert.equal(result.identifier, 'com.xl153.installer');
  assert.equal(result.bundleIdentifier, 'org.chromium.Chromium');
  assert.equal(result.installDomain, 'localSystem');
  assert.equal(result.installLocation, '/Applications');
  assert.equal(result.ownership, 'root:wheel');
  assert.equal(result.sha256, crypto.createHash('sha256').update('new package bytes').digest('hex').toUpperCase());
  assert.deepEqual(fs.readdirSync(f.tempRoot), []);
  assert.equal(fs.readFileSync(path.join(f.app, 'Contents/Info.plist'), 'utf8'), 'original plist');
  const build = f.calls.find(call => call.tool === 'pkgbuild' && !call.args.includes('--analyze'));
  assert.equal(build.args.includes('--sign'), false, 'sign the final archive, not just its component');
  assert.equal(build.args[build.args.indexOf('--ownership') + 1], 'recommended');
  assert.equal(build.args[build.args.indexOf('--install-location') + 1], '/Applications');
  assert.equal(build.args[build.args.indexOf('--version') + 1], '1.0.0.2');
  assert.notEqual(build.args.at(-1), f.output, 'pkgbuild must write into private staging');
  const product = f.calls.find(call => call.tool === 'productbuild');
  assert.equal(product.args[product.args.indexOf('--sign') + 1], identity);
  assert.equal(product.args[product.args.indexOf('--package-path') + 1], path.dirname(build.args.at(-1)));
  assert.notEqual(product.args.at(-1), f.output, 'productbuild must write into private staging');
  assert.notEqual(product.args.at(-1), build.args.at(-1));
  assert.match(product.distribution, /enable_currentUserHome="false"/);
  assert.match(product.distribution, /enable_localSystem="true"/);
  assert.match(product.distribution, /enable_anywhere="false"/);
  assert.match(product.distribution, /require-scripts="false"/);
  assert.match(product.distribution,
    /<pkg-ref id="com\.xl153\.installer">\s*<must-close><app id="org\.chromium\.Chromium"\/><\/must-close>\s*<\/pkg-ref>/);
  assert.equal((product.distribution.match(/<must-close>/g) || []).length, 1);
  assert.doesNotMatch(product.distribution, /<app id="com\.xl153\.installer"/);
  assert.doesNotMatch(product.distribution, /<script\b|auth="root"|\/Users\//);
  assert.equal(f.calls.some(call => call.args.includes('--scripts')), false);
  assert.equal(fs.readdirSync(f.root).some(name => name.startsWith('.xenon-pkg-')), false);
});

test('each packaging failure preserves existing output and removes temporary data', async t => {
  for (const failAt of ['ditto', 'analyze', 'python3', 'build', 'productbuild']) {
    const f = fixture(t, { failAt });
    await assert.rejects(buildMacPkg(f.options, f.dependencies), new RegExp(`${failAt} failed`));
    assert.equal(fs.readFileSync(f.output, 'utf8'), 'previous package');
    assert.deepEqual(fs.readdirSync(f.tempRoot), []);
    assert.equal(fs.readdirSync(f.root).some(name => name.startsWith('.xenon-pkg-')), false);
  }
});

test('the location option accepts only the system Applications directory', async t => {
  const f = fixture(t);
  for (const installLocation of ['~/Applications', '/Users/builder/Applications', '/Other', '/']) {
    await assert.rejects(buildMacPkg({ ...f.options, installLocation }, f.dependencies), /System packages require/);
    assert.equal(fs.readFileSync(f.output, 'utf8'), 'previous package');
  }
  const result = await buildMacPkg({ ...f.options, installLocation: '/Applications' }, f.dependencies);
  assert.equal(result.installLocation, '/Applications');
  assert.equal(f.calls.find(call => call.tool === 'productbuild').args.includes('--sign'), false);
});

test('distribution metadata is XML-escaped rather than interpreted as markup', async t => {
  const f = fixture(t, { bundleName: 'Xenon & "Friends" <Beta>', bundleIdentifier: 'app&"\'<id>' });
  await buildMacPkg({ ...f.options, identifier: 'com.xenon.test' }, f.dependencies);
  const product = f.calls.find(call => call.tool === 'productbuild');
  assert.match(product.distribution, /Xenon &amp; &quot;Friends&quot; &lt;Beta&gt;/);
  assert.doesNotMatch(product.distribution, /<Beta>/);
  assert.match(product.distribution, /<app id="app&amp;&quot;&apos;&lt;id&gt;"\/>/);
});

test('missing or empty application identity prevents publishing a package without a close guard', async t => {
  for (const bundleIdentifier of [null, '', '   ']) {
    const f = fixture(t, { bundleIdentifier });
    await assert.rejects(buildMacPkg(f.options, f.dependencies), /CFBundleIdentifier/);
    assert.equal(fs.readFileSync(f.output, 'utf8'), 'previous package');
    assert.deepEqual(fs.readdirSync(f.tempRoot), []);
    assert.equal(f.calls.some(call => call.tool === 'pkgbuild'), false);
  }
});

test('receipt version cannot differ from application version', async t => {
  const f = fixture(t);
  await assert.rejects(buildMacPkg({ ...f.options, version: '1.0.0.3' }, f.dependencies), /differs from app version/);
  assert.equal(fs.readFileSync(f.output, 'utf8'), 'previous package');
  assert.deepEqual(fs.readdirSync(f.tempRoot), []);
  assert.equal(f.calls.some(call => call.tool === 'pkgbuild'), false);
});

test('invalid app versions and empty package output fail without replacing an existing package', async t => {
  for (const version of ['', '1.0', '1.0.0.4294967296', '01.0.0.2']) {
    const f = fixture(t, { version });
    await assert.rejects(buildMacPkg(f.options, f.dependencies), /MAJOR.MINOR.BUILD.PATCH/);
    assert.equal(fs.readFileSync(f.output, 'utf8'), 'previous package');
    assert.deepEqual(fs.readdirSync(f.tempRoot), []);
  }
  const f = fixture(t, { emptyPackage: true });
  await assert.rejects(buildMacPkg(f.options, f.dependencies), /empty package/);
  assert.equal(fs.readFileSync(f.output, 'utf8'), 'previous package');
  assert.deepEqual(fs.readdirSync(f.tempRoot), []);
});

test('output inside the app is rejected and publish failures clean staging', async t => {
  const f = fixture(t);
  await assert.rejects(buildMacPkg({ ...f.options, out: path.join(f.app, 'installer.pkg') }, f.dependencies), /outside the input/);
  assert.equal(fs.existsSync(path.join(f.app, 'installer.pkg')), false);
  const directory = path.join(f.root, 'existing-directory.pkg');
  fs.mkdirSync(directory);
  fs.writeFileSync(path.join(directory, 'keep'), 'unchanged');
  await assert.rejects(buildMacPkg({ ...f.options, out: directory }, f.dependencies));
  assert.equal(fs.readFileSync(path.join(directory, 'keep'), 'utf8'), 'unchanged');
  assert.deepEqual(fs.readdirSync(f.tempRoot), []);
  assert.equal(fs.readdirSync(f.root).some(name => name.startsWith('.xenon-pkg-')), false);
});

test('native product archive exposes only the system domain and stores root-owned payload',
  { skip: process.platform !== 'darwin' }, async t => {
    const root = fs.mkdtempSync(path.join(os.tmpdir(), 'xenon-system-pkg-native-'));
    t.after(() => fs.rmSync(root, { recursive: true, force: true }));
    const app = path.join(root, 'xl153.app');
    const executable = path.join(app, 'Contents/MacOS/xl153');
    fs.mkdirSync(path.dirname(executable), { recursive: true });
    fs.writeFileSync(executable, 'native packaging fixture', { mode: 0o755 });
    fs.symlinkSync('MacOS/xl153', path.join(app, 'Contents/executable-link'));
    fs.mkdirSync(path.join(app, 'Contents/Empty'));
    fs.writeFileSync(path.join(app, 'Contents/Info.plist'), `<?xml version="1.0"?>
<plist version="1.0"><dict>
<key>CFBundleName</key><string>xl153</string>
<key>CFBundleIdentifier</key><string>org.xenon.user-pkg-test</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleExecutable</key><string>xl153</string>
<key>CFBundleShortVersionString</key><string>1.0.0.2</string>
<key>CFBundleVersion</key><string>1.0.0.2</string>
</dict></plist>`);
    const output = path.join(root, 'system-installer.pkg');
    const result = await buildMacPkg({ app, out: output });
    assert.equal(result.installDomain, 'localSystem');
    const checked = (tool, args) => {
      const response = spawnSync(tool, args, { encoding: 'utf8' });
      assert.equal(response.status, 0, response.error?.message || response.stderr || response.stdout);
      return response.stdout;
    };
    const expanded = path.join(root, 'expanded');
    checked('/usr/sbin/pkgutil', ['--expand-full', output, expanded]);
    const metadata = JSON.parse(checked('/usr/bin/python3', ['-c', `
import json, pathlib, sys, xml.etree.ElementTree as ET
root = pathlib.Path(sys.argv[1])
distribution = ET.parse(root / 'Distribution').getroot()
packages = list(root.rglob('PackageInfo'))
assert len(packages) == 1
package = ET.parse(packages[0]).getroot()
print(json.dumps({'domains': distribution.find('domains').attrib,
                  'must_close': [{'package': ref.get('id'), 'apps': [app.get('id') for app in ref.findall('must-close/app')]}
                                 for ref in distribution.findall('pkg-ref') if ref.find('must-close') is not None],
                  'install_location': package.get('install-location'),
                  'identifier': package.get('identifier'),
                  'version': package.get('version'),
                  'payload': str(packages[0].parent / 'Payload' / 'xl153.app'),
                  'scripts': [str(p) for p in root.rglob('Scripts')]}))
`, expanded]));
    assert.deepEqual(metadata.domains, {
      enable_currentUserHome: 'false', enable_localSystem: 'true', enable_anywhere: 'false'
    });
    assert.deepEqual(metadata.must_close, [{ package: 'com.xl153.installer', apps: ['org.xenon.user-pkg-test'] }]);
    assert.equal(metadata.install_location, '/Applications');
    assert.equal(metadata.identifier, 'com.xl153.installer');
    assert.equal(metadata.version, '1.0.0.2');
    assert.deepEqual(metadata.scripts, []);
    const distribution = fs.readFileSync(path.join(expanded, 'Distribution'), 'utf8');
    assert.equal(distribution.includes(os.homedir()), false);
    const payloadExecutable = path.join(metadata.payload, 'Contents/MacOS/xl153');
    assert.equal(fs.readFileSync(payloadExecutable, 'utf8'), 'native packaging fixture');
    assert.equal(fs.statSync(payloadExecutable).mode & 0o777, 0o755);
    assert.equal(fs.readlinkSync(path.join(metadata.payload, 'Contents/executable-link')), 'MacOS/xl153');
    assert.deepEqual(fs.readdirSync(path.join(metadata.payload, 'Contents/Empty')), []);
    const domains = checked('/usr/sbin/installer', ['-dominfo', '-pkg', output]);
    assert.match(domains, /LocalSystem/);
    assert.doesNotMatch(domains, /CurrentUserHomeDirectory|Anywhere/);
    const ownership = checked('/usr/bin/lsbom', ['-p', 'ug', path.join(expanded, 'component.pkg/Bom')]);
    const owners = ownership.trim().split('\n').map(line => line.trim().split(/\s+/));
    assert.ok(owners.length >= 5);
    assert.ok(owners.every(([uid, gid]) => uid === '0' && gid === '0'), ownership);
  });
