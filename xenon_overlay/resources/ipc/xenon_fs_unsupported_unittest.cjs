// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Run with: node --test xenon_overlay/resources/ipc/xenon_fs_unsupported_unittest.cjs
const assert = require('node:assert/strict');
const {readFileSync} = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');

const bootstrapPath = path.join(__dirname, 'xenon_ipc_renderer_bootstrap.js');
const bootstrapSource = readFileSync(bootstrapPath, 'utf8');

// Load the production module; only the native filesystem boundary is replaced.
function createRenderer() {
  const calls = [];
  const files = new Map();
  const perform = (kind, channel, request) => {
    assert.equal(channel, '__xenon:fs');
    calls.push({kind, ...request});
    if (request.operation === 'exists') return files.has(request.path);
    if (request.operation === 'write_file') {
      files.set(request.path, Buffer.from(request.dataBase64, 'base64'));
      return undefined;
    }
    if (request.operation === 'read_file' && files.has(request.path)) {
      return files.get(request.path).toString('base64');
    }
    throw new Error('ENOENT: no such file or directory');
  };
  const context = vm.createContext({
    xenonIpcRenderer: {
      getRuntimeConfig: () => ({appPath: 'C:\\test-app',
        exeDir: 'C:\\test-app', execPath: 'C:\\test-app\\host.exe'}),
      setDispatchHandler() {},
      sendSync: (channel, request) => perform('sync', channel, request),
      invoke: (channel, request) => {
        calls.push({kind: 'submit', ...request});
        return Promise.resolve().then(() => perform('async', channel, request));
      },
    },
    TextEncoder, TextDecoder, URL, URLSearchParams, queueMicrotask, atob, btoa,
    location: {protocol: 'chrome:', hostname: 'xenon-player-electron', search: ''},
    console: {log() {}, warn() {}, error() {}},
  });
  vm.runInContext(bootstrapSource, context, {filename: bootstrapPath});
  return {fs: context.require('node:fs'), context, calls, files};
}

test('unsupported renderer fs descriptor and watcher APIs fail explicitly', () => {
  const {fs, calls} = createRenderer();
  for (const method of [
    'openSync', 'closeSync', 'readSync', 'writeSync', 'chmodSync', 'chownSync',
    'watch', 'watchFile', 'unwatchFile',
  ]) {
    assert.throws(() => fs[method]('C:\\test-app\\unused'),
        {code: 'ERR_NOT_SUPPORTED'}, method);
    assert.throws(() => fs[method]('chrome://xenon-player-electron/unused'),
        {code: 'ERR_NOT_SUPPORTED'}, method + ' on a virtual path');
  }
  assert.deepEqual(calls, []);
});

test('unsupported renderer fs callbacks receive one asynchronous error', async () => {
  const {fs, calls} = createRenderer();
  const methods = {
    open: ['unused', 'r'], close: [3], read: [3, new Uint8Array(4), 0, 4, 0],
    write: [3, 'data'], chmod: ['unused', 0o644], chown: ['unused', 1, 1],
  };
  for (const [method, args] of Object.entries(methods)) {
    let returned = false;
    let callbackCount = 0;
    await new Promise((resolve, reject) => {
      const result = fs[method](...args, (error, ...values) => {
        ++callbackCount;
        try {
          assert.equal(returned, true, method);
          assert.equal(error.code, 'ERR_NOT_SUPPORTED', method);
          assert.deepEqual(values, [], method);
          resolve();
        } catch (failure) {
          reject(failure);
        }
      });
      assert.equal(result, undefined, method);
      returned = true;
    });
    assert.equal(callbackCount, 1, method);
  }
  assert.deepEqual(calls, []);
});

test('unsupported renderer fs callbacks validate callback arguments synchronously', () => {
  const {fs, calls} = createRenderer();
  for (const method of ['open', 'close', 'read', 'write', 'chmod', 'chown']) {
    for (const callback of [undefined, null, 1, 'callback', {}]) {
      assert.throws(() => fs[method]('unused', callback),
          {name: 'TypeError', code: 'ERR_INVALID_ARG_TYPE'}, method);
    }
  }
  assert.deepEqual(calls, []);
});

test('unsupported renderer fs Promise APIs reject instead of returning handles', async () => {
  const {fs, calls} = createRenderer();
  for (const method of ['open', 'chmod', 'chown']) {
    let result;
    assert.doesNotThrow(() => { result = fs.promises[method]('unused', 'r'); });
    assert.equal(typeof result.then, 'function');
    await assert.rejects(result, {code: 'ERR_NOT_SUPPORTED'}, method);
  }
  assert.deepEqual(calls, []);
});

test('invalid callbacks do not submit renderer filesystem operations', async () => {
  const {fs, calls} = createRenderer();
  const source = 'C:\\test-app\\source';
  const dest = 'C:\\test-app\\dest';
  const operations = [
    () => fs.readFile(source, null),
    () => fs.writeFile(source, 'data', null),
    () => fs.appendFile(source, 'data', null),
    () => fs.stat(source, null),
    () => fs.lstat(source, null),
    () => fs.readdir(source, null),
    () => fs.mkdir(source, null),
    () => fs.unlink(source, null),
    () => fs.rm(source, null),
    () => fs.rmdir(source, null),
    () => fs.access(source, null),
    () => fs.rename(source, dest, null),
    () => fs.copyFile(source, dest, null),
    () => fs.realpath(source, null),
    () => fs.exists(source, null),
  ];
  for (const operation of operations) {
    assert.throws(operation, {name: 'TypeError', code: 'ERR_INVALID_ARG_TYPE'});
  }
  await Promise.resolve();
  assert.deepEqual(calls, []);
});

test('real virtual filesystem data survives standardizing unsupported APIs', async () => {
  const {fs, context, calls} = createRenderer();
  const source = 'chrome://xenon-player-electron/source.bin';
  const copy = 'chrome://xenon-player-electron/copy.bin';
  const moved = 'chrome://xenon-player-electron/moved.bin';
  const bytes = vm.runInContext('Buffer.from([0, 255, 128, 65])', context);
  fs.writeFileSync(source, bytes);
  assert.deepEqual(Array.from(fs.readFileSync(source)), [0, 255, 128, 65]);
  await fs.promises.copyFile(source, copy);
  await fs.promises.rename(copy, moved);
  assert.deepEqual(Array.from(await fs.promises.readFile(moved)), [0, 255, 128, 65]);
  assert.equal((await fs.promises.stat(moved)).size, 4);
  const invalid = 'chrome://xenon-player-electron/invalid.bin';
  assert.throws(() => fs.writeFile(invalid, 'ignored', null),
      {code: 'ERR_INVALID_ARG_TYPE'});
  await Promise.resolve();
  assert.equal(fs.existsSync(invalid), false);
  assert.equal(fs.existsSync(copy), false);
  await fs.promises.unlink(moved);
  assert.equal(fs.existsSync(moved), false);
  assert.deepEqual(calls, []);
});

test('implemented native renderer file APIs still forward binary payloads', async () => {
  const {fs, context, files, calls} = createRenderer();
  const filename = 'C:\\test-app\\data.bin';
  const bytes = vm.runInContext('Buffer.from([0, 255, 128, 65])', context);
  await new Promise((resolve, reject) => fs.writeFile(filename, bytes,
      error => error ? reject(error) : resolve()));
  assert.deepEqual(Array.from(files.get(filename)), [0, 255, 128, 65]);
  assert.deepEqual(Array.from(await fs.promises.readFile(filename)), [0, 255, 128, 65]);
  assert.deepEqual(calls.filter(call => call.kind === 'async')
      .map(call => call.operation), ['write_file', 'read_file']);
});
