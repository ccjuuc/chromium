// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import test from 'node:test';
import {
  buildDiffManifest, computeSha256, defaultTools, getMacAppVersion, packBundle,
} from './make_directory_diff.js';

function workspace(t) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'xenon-diff-test-'));
  t.after(() => fs.rmSync(root, { recursive: true, force: true }));
  return root;
}

function write(root, relative, data, mode = 0o644) {
  const file = path.join(root, relative);
  fs.mkdirSync(path.dirname(file), { recursive: true });
  fs.writeFileSync(file, data);
  fs.chmodSync(file, mode);
  return file;
}

function plist(version) {
  return `<plist><dict><key>CFBundleShortVersionString</key>` +
      `<string>${version}</string></dict></plist>`;
}

function fakePatcher({ corrupt = false } = {}) {
  const calls = [];
  const targets = new Map();
  return {
    calls,
    runCommand(command, args) {
      calls.push({ command, args });
      if (args[0] === '-gen') {
        const [base, target, output] = args.slice(-3);
        assert.ok(fs.existsSync(base));
        targets.set(output, target);
        fs.writeFileSync(output, 'small test delta');
      } else {
        assert.equal(args[0], '-apply');
        const [, , patch, output] = args;
        fs.writeFileSync(output, corrupt ? 'invalid reconstruction' :
            fs.readFileSync(targets.get(patch)));
      }
      return { status: 0 };
    },
  };
}

test('mac manifest preserves framework version mapping, hashes, modes and links', t => {
  const root = workspace(t);
  const oldRootDir = path.join(root, 'old.app');
  const newRootDir = path.join(root, 'new.app');
  const framework = 'Contents/Frameworks/Browser Framework.framework';
  for (const [dir, version, fill] of [[oldRootDir, '1.0.0.1', 65],
    [newRootDir, '1.0.0.2', 66]]) {
    write(dir, 'Contents/Info.plist', plist(version));
    write(dir, 'Contents/MacOS/Browser', 'same executable', 0o755);
    write(dir, `${framework}/Versions/${version}/Browser Framework`,
        Buffer.alloc(96 * 1024, fill), 0o755);
    write(dir, `${framework}/Versions/${version}/Resources/data.pak`, 'same resource');
    fs.symlinkSync(version, path.join(dir, framework, 'Versions/Current'), 'dir');
    fs.symlinkSync('Versions/Current/Resources',
        path.join(dir, framework, 'Resources'), 'dir');
    fs.symlinkSync('Versions/Current/Browser Framework',
        path.join(dir, framework, 'Browser Framework'), 'file');
  }
  const emptyDirectory = path.join(newRootDir, 'Contents/Resources/Empty');
  fs.mkdirSync(emptyDirectory, { recursive: true });
  fs.chmodSync(emptyDirectory, 0o750);
  fs.chmodSync(newRootDir, 0o750);
  write(newRootDir, 'Contents/Resources/new.txt', 'new asset', 0o640);
  write(oldRootDir, 'Contents/Resources/removed.txt', 'remove me');
  const patcher = fakePatcher();
  const manifest = buildDiffManifest({ oldRootDir, newRootDir,
    bundleDir: path.join(root, 'bundle'), baseVersion: '1.0.0.1',
    targetVersion: '1.0.0.2', isMacMode: true, zucchiniExe: '/native/zucchini',
    runCommand: patcher.runCommand });
  const actions = new Map(manifest.actions.map(action => [action.target, action]));
  assert.equal(manifest.schema_version, 2);
  assert.equal(manifest.platform, 'mac');
  assert.equal(manifest.root_mode, fs.statSync(newRootDir).mode & 0o777);
  const binary = actions.get(`${framework}/Versions/1.0.0.2/Browser Framework`);
  assert.equal(binary.action, 'patch');
  assert.equal(binary.base, `${framework}/Versions/1.0.0.1/Browser Framework`);
  assert.deepEqual(patcher.calls[0].args.slice(0, 2), ['-gen', '-raw']);
  assert.equal(patcher.calls[0].command, '/native/zucchini');
  assert.equal(actions.get(`${framework}/Versions/1.0.0.2/Resources/data.pak`).action,
      'copy');
  assert.deepEqual(actions.get(`${framework}/Versions/Current`), {
    action: 'symlink', target: `${framework}/Versions/Current`, link_target: '1.0.0.2',
  });
  assert.equal(actions.get(`${framework}/Resources`).action, 'symlink');
  assert.equal(actions.get('Contents/Resources/Empty').action, 'directory');
  assert.equal(actions.get('Contents/Resources/Empty').mode,
      fs.statSync(emptyDirectory).mode & 0o777);
  assert.ok(!manifest.actions.some(action => action.target.includes('/Current/')));
  for (const action of manifest.actions.filter(action => action.sha256)) {
    const file = path.join(newRootDir, action.target);
    assert.equal(action.sha256, computeSha256(file));
    assert.equal(action.size, fs.statSync(file).size);
    assert.equal(action.mode, fs.statSync(file).mode & 0o777);
  }
  assert.ok(manifest.deletions.includes('Contents/Resources/removed.txt'));
  assert.ok(!manifest.deletions.includes(
      `${framework}/Versions/1.0.0.1/Browser Framework`));
  assert.equal(fs.readFileSync(path.join(oldRootDir,
      framework, 'Versions/1.0.0.1/Browser Framework'))[0], 65);
});

test('invalid patch roundtrip uses complete payload and removes rejected patch', t => {
  const root = workspace(t);
  const oldRootDir = path.join(root, 'old');
  const newRootDir = path.join(root, 'new');
  write(oldRootDir, 'Contents/MacOS/Browser', Buffer.alloc(80 * 1024, 1), 0o755);
  const target = write(newRootDir, 'Contents/MacOS/Browser',
      Buffer.alloc(80 * 1024, 2), 0o755);
  const bundleDir = path.join(root, 'bundle');
  const manifest = buildDiffManifest({ oldRootDir, newRootDir, bundleDir,
    baseVersion: '1', targetVersion: '2', isMacMode: true,
    runCommand: fakePatcher({ corrupt: true }).runCommand });
  const action = manifest.actions.find(entry => entry.target === 'Contents/MacOS/Browser');
  assert.equal(action.action, 'add');
  assert.equal(computeSha256(path.join(bundleDir, action.source)), computeSha256(target));
  assert.ok(!fs.existsSync(path.join(bundleDir, 'patches/Contents/MacOS/Browser.zucc')));
});

test('Windows retains existing action schema and PE patch arguments', t => {
  const root = workspace(t);
  const oldRootDir = path.join(root, 'old');
  const newRootDir = path.join(root, 'new');
  write(oldRootDir, '1.0.0.1/chrome.dll', Buffer.alloc(80 * 1024, 1));
  write(newRootDir, '1.0.0.2/chrome.dll', Buffer.alloc(80 * 1024, 2));
  write(oldRootDir, '1.0.0.1/resources.pak', 'same');
  write(newRootDir, '1.0.0.2/resources.pak', 'same');
  const patcher = fakePatcher();
  const manifest = buildDiffManifest({ oldRootDir, newRootDir,
    bundleDir: path.join(root, 'bundle'), baseVersion: '1.0.0.1',
    targetVersion: '1.0.0.2', isMacMode: false, runCommand: patcher.runCommand });
  assert.equal(manifest.platform, 'win');
  assert.ok(!('schema_version' in manifest));
  assert.ok(!('root_mode' in manifest));
  assert.deepEqual(manifest.actions.map(action => action.action), ['patch', 'copy']);
  assert.ok(manifest.actions.every(action => !('mode' in action)));
  assert.equal(manifest.actions[1].base, '1.0.0.1/resources.pak');
  assert.equal(patcher.calls[0].args[0], '-gen');
  assert.ok(!patcher.calls[0].args.includes('-raw'));
});

test('mac traversal does not follow and rejects escaping links', t => {
  const root = workspace(t);
  const oldRootDir = path.join(root, 'old.app');
  const newRootDir = path.join(root, 'new.app');
  write(oldRootDir, 'Contents/Info.plist', plist('1'));
  write(newRootDir, 'Contents/Info.plist', plist('2'));
  fs.symlinkSync('../../outside', path.join(newRootDir, 'Contents/unsafe'), 'file');
  assert.throws(() => buildDiffManifest({ oldRootDir, newRootDir,
    bundleDir: path.join(root, 'bundle'), baseVersion: '1', targetVersion: '2',
    isMacMode: true }), /Symbolic link escapes app/);
});

test('version extraction supports XML and delegates binary plist to plutil', t => {
  const root = workspace(t);
  write(root, 'Contents/Info.plist', plist('3.2.1'));
  assert.equal(getMacAppVersion(root, { hostPlatform: 'linux' }), '3.2.1');
  write(root, 'Contents/Info.plist', 'bplist00binary fixture');
  const calls = [];
  assert.equal(getMacAppVersion(root, { hostPlatform: 'darwin',
    runCommand(command, args) {
      calls.push({ command, args });
      return args[1] === 'CFBundleShortVersionString'
          ? { status: 1, stdout: '' } : { status: 0, stdout: '4.5.6\n' };
    } }), '4.5.6');
  assert.equal(calls[0].command, '/usr/bin/plutil');
  assert.deepEqual(calls[1].args.slice(0, 5), ['-extract', 'CFBundleVersion', 'raw', '-o', '-']);
  assert.throws(() => getMacAppVersion(root, { hostPlatform: 'linux' }), /requires macOS/);
  write(root, 'Contents/Info.plist', '<plist><dict/></plist>');
  assert.throws(() => getMacAppVersion(root, { hostPlatform: 'linux' }), /No numeric/);
});

test('native defaults and zip commands are platform appropriate', t => {
  const root = workspace(t);
  assert.ok(defaultTools('darwin').zucchiniExe.endsWith('zucchini'));
  assert.equal(defaultTools('darwin').sevenZip, undefined);
  assert.ok(defaultTools('win32').zucchiniExe.endsWith('zucchini.exe'));
  const calls = [];
  const runCommand = (command, args) => {
    calls.push({ command, args });
    return { status: 0 };
  };
  packBundle(root, path.join(root, 'mac.zip'), { hostPlatform: 'darwin', runCommand });
  packBundle(root, path.join(root, 'custom.zip'), { sevenZip: '/opt/7zz', runCommand });
  packBundle(root, path.join(root, 'linux.zip'), { hostPlatform: 'linux', runCommand });
  assert.equal(calls[0].command, '/usr/bin/ditto');
  assert.ok(!calls[0].args.includes('--keepParent'));
  assert.equal(calls[1].command, '/opt/7zz');
  assert.equal(calls[2].command, 'zip');
  assert.ok(calls[2].args.includes('-y'));
});

test('import does not run the CLI or create output', t => {
  const root = workspace(t);
  const moduleUrl = new URL('./make_directory_diff.js', import.meta.url).href;
  const result = spawnSync(process.execPath, ['--input-type=module', '-e',
    `await import(${JSON.stringify(moduleUrl)});`], { cwd: root, encoding: 'utf8' });
  assert.equal(result.status, 0, result.stderr);
  assert.equal(result.stdout, '');
  assert.deepEqual(fs.readdirSync(root), []);
});
