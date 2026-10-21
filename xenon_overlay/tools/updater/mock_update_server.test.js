// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
import assert from 'node:assert/strict';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';
import { createServer, UpdateState } from './mock_update_server.js';

const digest = bytes => crypto.createHash('sha256').update(bytes).digest('hex').toUpperCase();

function fixture(t) {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'xenon-update-server-test-'));
  t.after(() => fs.rmSync(directory, { recursive: true, force: true }));
  const write = (name, content) => fs.writeFileSync(path.join(directory, name), content);
  return { directory, write, state: new UpdateState(directory) };
}

test('mac manifests use the actual PKG for full updates and differential fallback', t => {
  const { write, state } = fixture(t);
  const pkg = Buffer.from('fixture PKG archive bytes');
  const patch = Buffer.from('fixture differential ZIP bytes');
  write('package.pkg', pkg);
  write('patch.zip', patch);
  write('package.zip', 'Windows ZIP');
  write('package.dmg', 'legacy DMG');
  for (const mode of ['diff', 'full', 'latest']) {
    state.mode = mode;
    const manifest = state.buildManifest('1.0.0.1', 'mac');
    assert.deepEqual(manifest.full_package, {
      url: 'http://127.0.0.1:8999/downloads/package.pkg', size: pkg.length, sha256: digest(pkg)
    });
    assert.equal(manifest.is_diff, mode === 'diff');
    if (mode === 'diff') {
      assert.equal(manifest.diff_package.sha256, digest(patch));
      assert.equal(manifest.diff_package.size, patch.length);
      assert.match(manifest.diff_package.url, /\/patch\.zip$/);
    }
  }
});

test('missing mac PKG never falls back to ZIP or DMG and never creates package data', t => {
  const { directory, write, state } = fixture(t);
  write('package.zip', 'existing small Windows ZIP');
  write('package.dmg', 'existing legacy DMG');
  for (const mode of ['diff', 'full', 'latest', 'corrupt']) {
    state.mode = mode;
    assert.throws(() => state.buildManifest('1.0.0.1', 'mac'), /package\.pkg/);
  }
  assert.deepEqual(fs.readdirSync(directory).sort(), ['package.dmg', 'package.zip']);
  assert.equal(fs.readFileSync(path.join(directory, 'package.zip'), 'utf8'), 'existing small Windows ZIP');
  const missingDirectory = path.join(directory, 'not-created');
  assert.throws(() => new UpdateState(missingDirectory).buildManifest(null, 'mac'), /package\.pkg/);
  assert.equal(fs.existsSync(missingDirectory), false);
});

test('empty or non-file PKG is rejected instead of advertising invalid metadata', t => {
  const { directory, write, state } = fixture(t);
  state.mode = 'full';
  write('package.pkg', '');
  assert.throws(() => state.buildManifest(null, 'mac'), /non-empty package file/);
  fs.unlinkSync(path.join(directory, 'package.pkg'));
  fs.mkdirSync(path.join(directory, 'package.pkg'));
  assert.throws(() => state.buildManifest(null, 'mac'), /non-empty package file/);
});

test('Windows full packages remain ZIPs with exact size and hash', t => {
  const { directory, write, state } = fixture(t);
  const zip = Buffer.from('small real test input, do not overwrite');
  write('package.zip', zip);
  write('package.pkg', 'Mac-only PKG');
  state.mode = 'full';
  assert.deepEqual(state.buildManifest(null, 'win').full_package, {
    url: 'http://127.0.0.1:8999/downloads/package.zip', size: zip.length, sha256: digest(zip)
  });
  assert.deepEqual(fs.readFileSync(path.join(directory, 'package.zip')), zip);
});

test('HTTP reports missing PKG and remains available for later requests and downloads', async t => {
  const { write, state } = fixture(t);
  write('package.zip', 'Windows ZIP');
  state.mode = 'full';
  const { server } = createServer(state);
  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(0, '127.0.0.1', resolve);
  });
  t.after(() => new Promise(resolve => {
    server.close(resolve);
    server.closeAllConnections();
  }));
  state.port = server.address().port;
  const base = state.getBaseUrl();
  const missing = await fetch(`${base}/api/v1/update/check?platform=mac`);
  assert.equal(missing.status, 503);
  assert.match((await missing.json()).detail, /package\.pkg/);
  assert.equal((await fetch(`${base}/downloads/package.pkg`)).status, 404);
  const windows = await fetch(`${base}/api/v1/update/check?platform=win`);
  assert.equal(windows.status, 200);
  assert.match((await windows.json()).full_package.url, /\/package\.zip$/);

  const pkg = Buffer.from('later supplied PKG bytes');
  write('package.pkg', pkg);
  const ready = await fetch(`${base}/api/v1/update/check?platform=mac`);
  assert.equal(ready.status, 200);
  const manifest = await ready.json();
  assert.equal(manifest.full_package.sha256, digest(pkg));
  const download = await fetch(manifest.full_package.url);
  assert.equal(download.status, 200);
  assert.deepEqual(Buffer.from(await download.arrayBuffer()), pkg);
});
