// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
'use strict';
const assert = require('node:assert/strict');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');
const {readBootstrap} = require('./bootstrap_test_support.cjs');

function runtime({legacyRead = false} = {}) {
  const files = new Map(), calls = [];
  const context = vm.createContext(vm.constants.DONT_CONTEXTIFY);
  const perform = request => {
    if (request.operation === 'read_file') {
      assert.equal(request.returnBytes, true);
      if (!files.has(request.path)) throw new Error('ENOENT: missing file');
      const bytes = files.get(request.path);
      if (legacyRead) return bytes.toString('base64');
      const result = new context.ArrayBuffer(bytes.length);
      new context.Uint8Array(result).set(bytes);
      return result;
    }
    assert.ok(['write_file', 'append_file'].includes(request.operation));
    assert.equal(request.dataBase64, undefined);
    assert.ok(ArrayBuffer.isView(request.data));
    const bytes = Buffer.from(request.data.buffer, request.data.byteOffset, request.data.byteLength);
    files.set(request.path, request.operation === 'append_file' ?
        Buffer.concat([files.get(request.path) || Buffer.alloc(0), bytes]) : Buffer.from(bytes));
  };
  const submit = (channel, request, async) => {
    assert.equal(channel, '__xenon:fs');
    calls.push({async, ...request});
    // Native conversion snapshots views before scheduling worker I/O.
    const snapshot = {...request};
    if (request.data) snapshot.data = Buffer.from(request.data);
    return async ? Promise.resolve().then(() => perform(snapshot)) : perform(snapshot);
  };
  Object.assign(context, {
    TextEncoder, TextDecoder, URL, URLSearchParams, queueMicrotask, atob, btoa,
    setTimeout, clearTimeout, setInterval, clearInterval,
    __xenonPaths: {platform: 'win32', arch: 'x64', endianness: 'LE'},
    location: {protocol: 'chrome:', hostname: 'fixture', search: ''},
    console: {log() {}, warn() {}, error() {}},
    xenonIpcRenderer: {
      getRuntimeConfig: () => ({appPath: 'C:\\fixture', exeDir: 'C:\\fixture',
        execPath: 'C:\\fixture\\host.exe'}),
      setDispatchHandler() {},
      sendSync: (channel, request) => submit(channel, request, false),
      invoke: (channel, request) => submit(channel, request, true),
    },
  });
  vm.runInContext(readBootstrap(path.join(__dirname, 'xenon_ipc_renderer_bootstrap.js')), context);
  return {fs: context.require('fs'), context, files, calls};
}

for (const legacyRead of [false, true]) {
  test(`renderer fs preserves binary data and read encodings (legacyRead=${legacyRead})`, async () => {
    const {fs, context, calls} = runtime({legacyRead});
    const filename = 'C:\\fixture\\bytes.bin';
    const expected = Buffer.from([0, 255, 128, 65]);
    fs.writeFileSync(filename, context.Buffer.from(expected));
    const result = fs.readFileSync(filename);
    assert.ok(context.Buffer.isBuffer(result));
    assert.deepEqual(Buffer.from(result), expected);
    assert.equal(fs.readFileSync(filename, 'hex'), expected.toString('hex'));
    assert.equal(await fs.promises.readFile(filename, {encoding: 'base64'}), expected.toString('base64'));
    const callbackResult = await new Promise((resolve, reject) => fs.readFile(filename,
        (error, value) => error ? reject(error) : resolve(value)));
    assert.ok(context.Buffer.isBuffer(callbackResult));
    assert.deepEqual(Buffer.from(callbackResult), expected);
    for (const call of calls.filter(call => call.operation === 'read_file')) assert.equal(call.returnBytes, true);
    fs.writeFileSync(filename, '中文\0text');
    assert.equal(await fs.promises.readFile(filename, 'utf8'), '中文\0text');
    fs.writeFileSync(filename, context.Buffer.alloc(0));
    assert.equal(fs.readFileSync(filename).length, 0);
  });
}

test('renderer fs snapshots exact Buffer, TypedArray and DataView ranges for async writes', async () => {
  const {fs, context, calls} = runtime();
  for (const expression of [
    'Buffer.from([9, 255, 0, 128, 9]).subarray(1, 4)',
    'new Uint8Array([9, 255, 0, 128, 9]).subarray(1, 4)',
    'new Uint16Array(new Uint8Array([9, 9, 255, 0, 128, 65, 9, 9]).buffer, 2, 2)',
    'new DataView(new Uint8Array([9, 255, 0, 128, 9]).buffer, 1, 3)',
  ]) {
    for (const method of ['writeFile', 'appendFile']) {
      const filename = 'C:\\fixture\\range.bin';
      fs.writeFileSync(filename, context.Buffer.alloc(0));
      const data = vm.runInContext(expression, context);
      const expected = Buffer.from(new Uint8Array(data.buffer, data.byteOffset, data.byteLength));
      const pending = method === 'writeFile' ? fs.promises.writeFile(filename, data) :
          new Promise((resolve, reject) => fs.appendFile(filename, data,
              error => error ? reject(error) : resolve()));
      new Uint8Array(data.buffer).fill(42);
      await pending;
      assert.deepEqual(Buffer.from(fs.readFileSync(filename)), expected);
      const write = calls.filter(call => call.operation ===
          (method === 'writeFile' ? 'write_file' : 'append_file')).at(-1);
      assert.deepEqual(Buffer.from(write.data), expected);
      assert.equal(write.data.byteLength, expected.length);
    }
  }
});

test('renderer fs sync append and callback write use the binary request field', async () => {
  const {fs, calls} = runtime();
  const filename = 'C:\\fixture\\append.bin';
  await new Promise((resolve, reject) => fs.writeFile(filename, 'first',
      error => error ? reject(error) : resolve()));
  fs.appendFileSync(filename, new Uint8Array([0, 255]));
  await fs.promises.appendFile(filename, 'last');
  assert.deepEqual(Buffer.from(fs.readFileSync(filename)), Buffer.concat([
    Buffer.from('first'), Buffer.from([0, 255]), Buffer.from('last'),
  ]));
  assert.deepEqual(calls.filter(call => call.data).map(call => call.operation),
      ['write_file', 'append_file', 'append_file']);
});

test('renderer fs binary reads keep synchronous and asynchronous errors', async () => {
  const {fs} = runtime();
  const filename = 'C:\\fixture\\missing.bin';
  const expected = {code: 'ENOENT', syscall: 'read_file', path: filename};
  assert.throws(() => fs.readFileSync(filename), expected);
  await assert.rejects(fs.promises.readFile(filename), expected);
  let returned = false;
  await new Promise(resolve => {
    fs.readFile(filename, error => {
      assert.ok(returned);
      for (const [key, value] of Object.entries(expected)) assert.equal(error[key], value);
      resolve();
    });
    returned = true;
  });
});

test('renderer fs binary transport is bypassed for the existing virtual mount', async () => {
  const {fs, calls} = runtime();
  const filename = 'chrome://fixture/data.bin';
  fs.writeFileSync(filename, new Uint8Array([0, 255]));
  await fs.promises.appendFile(filename, 'A');
  assert.deepEqual(Buffer.from(await fs.promises.readFile(filename)), Buffer.from([0, 255, 65]));
  assert.deepEqual(calls, []);
});
