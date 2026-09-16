// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const {spawnSync} = require('node:child_process');
const vm = require('node:vm');
const {test} = require('node:test');
const {normalizePreloads} = require('./normalize_player_preloads.cjs');

function fixture(t, decoded = 'module.exports = "restored";') {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'xenon-preload-'));
  t.after(() => fs.rmSync(root, {recursive: true, force: true}));
  fs.mkdirSync(path.join(root, 'preload'));
  const source = Buffer.from([0x1b, 0x87, 0xea, 0xc8, 0x00, 0xff]);
  fs.writeFileSync(path.join(root, 'preload/index.js'), source);
  const archive = path.join(root, 'source.asar');
  fs.writeFileSync(archive, Buffer.concat([Buffer.alloc(24), source]));
  const asarModule = path.join(root, 'asar.cjs');
  fs.writeFileSync(asarModule, `
    exports.statFile = () => {
      console.log('VENDOR-PRIVATE-MATERIAL');
      return {size: ${source.length}, encrypted: true, offset: '0'};
    };
    exports.extractFile = () => Buffer.from(${JSON.stringify(decoded)});
  `);
  fs.writeFileSync(path.join(root, 'disk.js'),
      'exports.readFilesystemSync = () => ({headerSize: 16});');
  return {root, source, archive, asarModule};
}

test('plain UTF-8, BOM, hashbang and Windows source paths remain unchanged', t => {
  const f = fixture(t);
  const source = Buffer.from('\ufeff#!node\nmodule.exports = "中文 C:\\\\player";');
  fs.writeFileSync(path.join(f.root, 'preload/index.js'), source);
  const result = normalizePreloads(f.root, 'missing.asar', 'missing.cjs');
  assert.equal(result.normalized, 0);
  assert.deepEqual(Buffer.from(result.scripts['preload/index.js'], 'base64'), source);
});

test('matching vendor archive supplies compile-checked original source without execution', t => {
  const f = fixture(t, 'throw new Error("packaging must not execute me");');
  const result = normalizePreloads(f.root, f.archive, f.asarModule);
  assert.equal(result.normalized, 1);
  assert.match(Buffer.from(result.scripts['preload/index.js'], 'base64').toString(),
      /packaging must not execute me/);
  assert.deepEqual(fs.readFileSync(path.join(f.root, 'preload/index.js')), f.source);
});

test('encoded preload without matching archive/tool fails explicitly', t => {
  const f = fixture(t);
  assert.throws(() => normalizePreloads(f.root, 'missing.asar', 'missing.cjs'),
      /requires the matching vendor archive/);
});

test('stale archive cannot silently replace another build preload', t => {
  const f = fixture(t);
  fs.writeFileSync(f.archive, Buffer.alloc(24 + f.source.length));
  assert.throws(() => normalizePreloads(f.root, f.archive, f.asarModule),
      /does not match the encoded source/);
});

test('invalid decoded source remains a failure and vendor console is restored', t => {
  const f = fixture(t, 'const invalid = ;');
  const log = console.log;
  assert.throws(() => normalizePreloads(f.root, f.archive, f.asarModule), SyntaxError);
  assert.equal(console.log, log);
  assert.deepEqual(fs.readFileSync(path.join(f.root, 'preload/index.js')), f.source);
});

test('CLI returns machine-readable results without vendor key/header output', t => {
  const f = fixture(t);
  const result = spawnSync(process.execPath, [
    path.join(__dirname, 'normalize_player_preloads.cjs'),
    f.root, f.archive, f.asarModule,
  ], {encoding: 'utf8', windowsHide: true});
  assert.equal(result.status, 0, result.stderr);
  assert.equal(JSON.parse(result.stdout).normalized, 1);
  assert.doesNotMatch(result.stdout + result.stderr, /VENDOR-PRIVATE-MATERIAL/);
});

test('failed validation leaves an existing output preload unchanged', t => {
  const f = fixture(t, 'const invalid = ;');
  const output = path.join(f.root, 'output');
  fs.mkdirSync(path.join(output, 'preload'), {recursive: true});
  fs.writeFileSync(path.join(output, 'preload/index.js'), 'previous runtime');
  const result = spawnSync(process.execPath, [
    path.join(__dirname, 'normalize_player_preloads.cjs'),
    f.root, f.archive, f.asarModule, '--write-to', output,
  ], {encoding: 'utf8', windowsHide: true});
  assert.notEqual(result.status, 0);
  assert.equal(fs.readFileSync(path.join(output, 'preload/index.js'), 'utf8'),
      'previous runtime');
});

test('optional real packaged preload installs APIs and forwards IPC with an explicit Electron mock',
    {skip: !process.env.XENON_PLAYER_PRELOAD_ROOT}, () => {
  const root = process.env.XENON_PLAYER_PRELOAD_ROOT;
  const filename = path.join(root, 'preload/index.js');
  const source = fs.readFileSync(filename, 'utf8');
  const listeners = [];
  const context = vm.createContext({
    window: {}, process: {contextIsolated: false, versions: {electron: '22.3.27'}},
    console: {log() {}},
    require(name) {
      assert.equal(name, 'electron');
      return {ipcRenderer: {on: (channel, listener) => listeners.push({channel, listener})}};
    },
  });
  vm.runInContext(`(new Function('require', 'process', 'window', ${JSON.stringify(
      source + '\n//# sourceURL=' + filename.replace(/\\/g, '/'))}))(require, process, window)`, context);
  assert.equal(typeof context.window.electron.ipcRenderer.send, 'function');
  let received;
  context.window.api.onMessageFromMain(data => { received = data; });
  assert.equal(listeners[0].channel, 'main-to-renderer');
  listeners[0].listener({}, 'real preload callback');
  assert.equal(received, 'real preload callback');
  new vm.Script(fs.readFileSync(path.join(root, 'preload-native/index.js'), 'utf8'));
});
