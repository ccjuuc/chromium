// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { gzipSync, gunzipSync } from 'node:zlib';
import test from 'node:test';
import { buildPackage, createManifest, findExecutable, validateVersion } from './make_directory_diff.js';

function fixture(t) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'xenon-package-test-'));
  t.after(() => fs.rmSync(root, { recursive: true, force: true }));
  const oldRoot = path.join(root, 'Old.app');
  const newRoot = path.join(root, 'New.app');
  const bundleDir = path.join(root, 'bundle');
  const tempRoot = path.join(root, 'temp');
  for (const dir of [oldRoot, newRoot, bundleDir, tempRoot]) fs.mkdirSync(dir);
  const write = (dir, relative, data, mode = 0o644) => {
    const file = path.join(dir, relative);
    fs.mkdirSync(path.dirname(file), { recursive: true });
    fs.writeFileSync(file, data, { mode });
    fs.chmodSync(file, mode);
    return file;
  };
  const manifest = extra => createManifest({ oldRoot, newRoot, bundleDir, stagingDir: tempRoot,
    baseVersion: '1.0.0.1', targetVersion: '1.0.0.2', isMac: true, ...extra });
  const plist = version => `<?xml version="1.0" encoding="UTF-8"?><plist version="1.0"><dict><key>CFBundleShortVersionString</key><string>${version}</string></dict></plist>`;
  return { root, oldRoot, newRoot, bundleDir, tempRoot, write, manifest, plist };
}

test('target permissions are recorded for copy, add and patch actions', t => {
  const f = fixture(t);
  f.write(f.oldRoot, 'copy', 'unchanged', 0o644);
  f.write(f.newRoot, 'copy', 'unchanged', 0o755);
  f.write(f.newRoot, 'add', 'new executable', 0o751);
  f.write(f.oldRoot, 'patch', 'a'.repeat(100000), 0o755);
  f.write(f.newRoot, 'patch', 'b'.repeat(100000), 0o750);
  // A deterministic test codec exercises the generator and roundtrip contract;
  // the C++ tests cover actual Zucchini generation/application.
  const run = (_tool, args) => {
    if (args[0] === '-gen') fs.writeFileSync(args.at(-1), gzipSync(fs.readFileSync(args.at(-2))));
    else fs.writeFileSync(args.at(-1), gunzipSync(fs.readFileSync(args.at(-2))));
    return { status: 0 };
  };
  const actions = f.manifest({ zucchini: 'test-codec', run }).actions;
  assert.deepEqual(actions.map(({ target, action, mode }) => ({ target, action, mode })), [
    { target: 'add', action: 'add', mode: 0o751 },
    { target: 'copy', action: 'copy', mode: 0o755 },
    { target: 'patch', action: 'patch', mode: 0o750 },
  ]);
  for (const action of actions) assert.match(action.sha256, /^[A-F0-9]{64}$/);
});

test('terminated patch generators report the signal instead of a null exit code', t => {
  const f = fixture(t);
  f.write(f.oldRoot, 'binary', 'a'.repeat(100000));
  f.write(f.newRoot, 'binary', 'b'.repeat(100000));
  assert.throws(() => f.manifest({ zucchini: 'test-codec',
    run: () => ({ status: null, signal: 'SIGTRAP', stderr: '' }) }), /signal SIGTRAP/);
});

test('Mac framework version changes produce copy and patch actions with actual paths', t => {
  const f = fixture(t);
  const framework = 'Contents/Frameworks/Browser Framework.framework/Versions';
  const before = `${framework}/1.0.0.1`;
  const after = `${framework}/1.0.0.2`;
  f.write(f.oldRoot, `${before}/Resources/resources.pak`, 'unchanged resources');
  f.write(f.newRoot, `${after}/Resources/resources.pak`, 'unchanged resources');
  f.write(f.oldRoot, `${before}/Resources/removed.pak`, 'removed resource');
  const oldBinary = f.write(f.oldRoot, `${before}/Browser Framework`, 'a'.repeat(100000), 0o755);
  const newBinary = f.write(f.newRoot, `${after}/Browser Framework`, 'b'.repeat(100000), 0o755);
  const calls = [];
  const run = (_tool, args) => {
    calls.push(args);
    if (args[0] === '-gen') fs.writeFileSync(args.at(-1), gzipSync(fs.readFileSync(args.at(-2))));
    else fs.writeFileSync(args.at(-1), gunzipSync(fs.readFileSync(args.at(-2))));
    return { status: 0 };
  };
  const manifest = f.manifest({ zucchini: 'test-codec', run });
  const resource = manifest.actions.find(action => action.target.endsWith('/resources.pak'));
  assert.equal(resource.action, 'copy');
  assert.equal(resource.base, `${before}/Resources/resources.pak`);
  assert.equal(resource.target, `${after}/Resources/resources.pak`);
  const binary = manifest.actions.find(action => action.target.endsWith('/Browser Framework'));
  assert.equal(binary.action, 'patch');
  assert.equal(binary.base, `${before}/Browser Framework`);
  assert.equal(binary.target, `${after}/Browser Framework`);
  assert.equal(binary.patch, `patches/${after}/Browser Framework.zucc`);
  assert.deepEqual(calls[0], ['-gen', '-raw', oldBinary, newBinary, path.join(f.bundleDir, binary.patch)]);
  assert.deepEqual(manifest.actions.map(action => action.action).sort(), ['copy', 'patch']);
  assert.deepEqual(manifest.deletions, [`${before}/Resources/removed.pak`]);
});

test('Mac mapping does not conflate resource directories or unrelated framework versions', t => {
  const f = fixture(t);
  const changedPaths = [
    ['Contents/Resources/1.0.0.1/data', 'Contents/Resources/1.0.0.2/data'],
    ['Contents/Frameworks/Plain/Versions/1.0.0.1/data', 'Contents/Frameworks/Plain/Versions/1.0.0.2/data'],
    ['Vendor.framework/Versions/4.0.0.1/data', 'Vendor.framework/Versions/4.0.0.2/data'],
    ['LongVersion.framework/Versions/1.0.0.10/data', 'LongVersion.framework/Versions/1.0.0.20/data'],
    ['OtherLayout.framework/Releases/1.0.0.1/data', 'OtherLayout.framework/Releases/1.0.0.2/data'],
  ];
  for (const [before, after] of changedPaths) {
    f.write(f.oldRoot, before, 'same content');
    f.write(f.newRoot, after, 'same content');
  }
  const fixed = 'Contents/Frameworks/Fixed.framework/Versions/153.0.0.0/data';
  f.write(f.oldRoot, fixed, 'same content');
  f.write(f.newRoot, fixed, 'same content');
  const manifest = f.manifest();
  for (const [, after] of changedPaths) {
    assert.equal(manifest.actions.find(action => action.target === after).action, 'add');
  }
  assert.deepEqual(manifest.deletions.sort(), changedPaths.map(([before]) => before).sort());
  const unchanged = manifest.actions.find(action => action.target === fixed);
  assert.equal(unchanged.action, 'copy');
  assert.equal(unchanged.base, fixed);
});

test('Mac framework Current aliases retain the new real version after path matching', { skip: process.platform === 'win32' }, t => {
  const f = fixture(t);
  const framework = 'Contents/Frameworks/Browser.framework/Versions';
  f.write(f.oldRoot, `${framework}/1.0.0.1/Browser`, 'unchanged binary', 0o755);
  f.write(f.newRoot, `${framework}/1.0.0.2/Browser`, 'unchanged binary', 0o755);
  fs.symlinkSync('1.0.0.1', path.join(f.oldRoot, framework, 'Current'));
  fs.symlinkSync('1.0.0.2', path.join(f.newRoot, framework, 'Current'));
  const manifest = f.manifest();
  assert.deepEqual(manifest.actions.find(action => action.target.endsWith('/Current')),
    { target: `${framework}/Current`, action: 'symlink', link: '1.0.0.2' });
  const binary = manifest.actions.find(action => action.target.endsWith('/Browser'));
  assert.equal(binary.action, 'copy');
  assert.equal(binary.base, `${framework}/1.0.0.1/Browser`);
  assert.equal(binary.target, `${framework}/1.0.0.2/Browser`);
  assert.deepEqual(manifest.deletions, []);
});

test('an old symlink becoming a regular file is an add even with identical contents', { skip: process.platform === 'win32' }, t => {
  const f = fixture(t);
  f.write(f.oldRoot, 'old.data', 'same');
  fs.symlinkSync('old.data', path.join(f.oldRoot, 'resource'));
  f.write(f.newRoot, 'resource', 'same', 0o755);
  const manifest = f.manifest();
  assert.equal(manifest.actions[0].action, 'add');
  assert.equal(fs.lstatSync(path.join(f.bundleDir, manifest.actions[0].source)).isSymbolicLink(), false);
  assert.equal(fs.readFileSync(path.join(f.bundleDir, manifest.actions[0].source), 'utf8'), 'same');
  assert.deepEqual(manifest.deletions, ['old.data']);
});

test('new symlinks preserve their target, while links outside the app are rejected', { skip: process.platform === 'win32' }, t => {
  const f = fixture(t);
  f.write(f.newRoot, 'real', 'content');
  fs.symlinkSync('real', path.join(f.newRoot, 'alias'));
  assert.deepEqual(f.manifest().actions[0], { target: 'alias', action: 'symlink', link: 'real' });
  fs.symlinkSync('../outside', path.join(f.newRoot, 'escape'));
  assert.throws(() => f.manifest(), /escapes the input tree/);
});

test('patch tool failures, corrupt roundtrips and missing tools cannot silently succeed', t => {
  const f = fixture(t);
  f.write(f.oldRoot, 'large', 'a'.repeat(70000));
  f.write(f.newRoot, 'large', 'b'.repeat(70000));
  assert.throws(() => f.manifest(), /Zucchini is required/);
  assert.throws(() => f.manifest({ zucchini: 'bad', run: () => ({ status: 1, stderr: 'bad codec' }) }), /bad codec/);
  assert.throws(() => f.manifest({ zucchini: 'corrupt', run: (_tool, args) => {
    fs.writeFileSync(args.at(-1), 'corrupt');
    return { status: 0 };
  } }), /roundtrip failed/);
});

test('version ambiguity, malformed versions and downgrades are rejected', t => {
  const f = fixture(t);
  for (const version of ['1.2', '1.2.3.x', '01.2.3.4', '1.2.3.4294967296']) {
    assert.throws(() => validateVersion(version), /four-component/);
  }
  assert.throws(() => f.manifest({ targetVersion: '1.0.0.1' }), /must be newer/);
  assert.throws(() => f.manifest({ targetVersion: '0.9.9.9' }), /must be newer/);
  const oldRoot = path.join(f.root, 'old-win');
  const newRoot = path.join(f.root, 'new-win');
  f.write(oldRoot, '1.0.0.1/file', 'old');
  f.write(oldRoot, '1.0.0.0/file', 'older');
  f.write(newRoot, '1.0.0.2/file', 'new');
  assert.throws(() => buildPackage({ old: oldRoot, new: newRoot, out: path.join(f.root, 'out.zip'), sevenZip: process.execPath },
    { platform: 'win32', tempRoot: f.tempRoot, projectRoot: f.root }), /exactly one version/);
  assert.deepEqual(fs.readdirSync(f.tempRoot), []);
});

test('Windows setup is added virtually without modifying either input tree', t => {
  const f = fixture(t);
  f.write(f.oldRoot, '1.0.0.1/file', 'same');
  f.write(f.oldRoot, 'setup.exe', 'old setup');
  f.write(f.newRoot, '1.0.0.2/file', 'same');
  f.write(f.newRoot, 'setup.exe', 'new setup');
  const manifest = f.manifest({ isMac: false });
  assert.equal(manifest.actions.find(a => a.target === '1.0.0.2/Installer/setup.exe').action, 'add');
  assert.equal(fs.existsSync(path.join(f.oldRoot, '1.0.0.1/Installer')), false);
  assert.equal(fs.existsSync(path.join(f.newRoot, '1.0.0.2/Installer')), false);
});

test('native ZIP package roundtrips permissions and leaves no staging files', { skip: process.platform === 'win32' }, t => {
  const f = fixture(t);
  f.write(f.oldRoot, 'Contents/Info.plist', f.plist('1.0.0.1'));
  f.write(f.newRoot, 'Contents/Info.plist', f.plist('1.0.0.2'));
  f.write(f.newRoot, 'Contents/MacOS/browser', 'executable', 0o755);
  if (process.platform === 'darwin') {
    for (const app of [f.oldRoot, f.newRoot]) {
      const converted = spawnSync('/usr/bin/plutil', ['-convert', 'binary1', path.join(app, 'Contents/Info.plist')]);
      assert.equal(converted.status, 0);
    }
  }
  const zip = findExecutable(['/usr/bin/zip', 'zip']);
  const unzip = findExecutable(['/usr/bin/unzip', 'unzip']);
  assert.ok(zip && unzip, 'native zip/unzip are required for this integration test');
  const result = buildPackage({ old: f.oldRoot, new: f.newRoot, out: path.join(f.root, 'patch.zip'), zip },
    { tempRoot: f.tempRoot, projectRoot: f.root });
  const extracted = path.join(f.root, 'extracted');
  const unzipped = spawnSync(unzip, ['-q', result.output, '-d', extracted], { encoding: 'utf8' });
  assert.equal(unzipped.status, 0, unzipped.stderr);
  const manifest = JSON.parse(fs.readFileSync(path.join(extracted, 'manifest.json')));
  assert.equal(manifest.target_version, '1.0.0.2');
  assert.equal(manifest.actions.find(a => a.target === 'Contents/MacOS/browser').mode, 0o755);
  assert.equal(fs.statSync(path.join(extracted, 'files/Contents/MacOS/browser')).mode & 0o777, 0o755);
  assert.deepEqual(fs.readdirSync(f.tempRoot), []);
});

test('failed archiving cleans its unique staging and preserves a previous output', t => {
  const f = fixture(t);
  f.write(f.oldRoot, 'Contents/Info.plist', f.plist('1.0.0.1'));
  f.write(f.newRoot, 'Contents/Info.plist', f.plist('1.0.0.2'));
  const output = f.write(f.root, 'patch.zip', 'previous package');
  for (let attempt = 0; attempt < 2; attempt++) {
    assert.throws(() => buildPackage({ old: f.oldRoot, new: f.newRoot, out: output, zip: process.execPath },
      { platform: 'linux', tempRoot: f.tempRoot, projectRoot: f.root,
        run: () => ({ status: 1, stderr: 'archiving failed' }) }), /archiving failed/);
    assert.deepEqual(fs.readdirSync(f.tempRoot), []);
    assert.equal(fs.readFileSync(output, 'utf8'), 'previous package');
  }
});

test('output inside an input is rejected before any input changes', t => {
  const f = fixture(t);
  assert.throws(() => buildPackage({ old: f.oldRoot, new: f.newRoot, out: path.join(f.newRoot, 'patch.zip') }), /outside both input/);
  assert.deepEqual(fs.readdirSync(f.newRoot), []);
});

test('missing version does not fall back to an invented baseline', t => {
  const f = fixture(t);
  f.write(f.oldRoot, 'Contents/Info.plist', '<plist><dict/></plist>');
  f.write(f.newRoot, 'Contents/Info.plist', f.plist('1.0.0.2'));
  assert.throws(() => buildPackage({ old: f.oldRoot, new: f.newRoot, zip: process.execPath, out: path.join(f.root, 'out.zip') },
    { platform: 'linux', tempRoot: f.tempRoot, projectRoot: f.root }), /Cannot read CFBundleShortVersionString/);
  assert.deepEqual(fs.readdirSync(f.tempRoot), []);
});
