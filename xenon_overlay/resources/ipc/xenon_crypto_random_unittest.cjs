// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
const {readBootstrap, readBootstrapPart} = require('./bootstrap_test_support.cjs');
const assert = require('node:assert/strict');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');
const filename = path.join(__dirname, 'xenon_ipc_renderer_bootstrap.js');
const source = readBootstrap(filename);

function renderer(secureSource) {
  const context = vm.createContext({
    __xenonPaths: {platform: 'win32', arch: 'x64', endianness: 'LE'},
    crypto: secureSource,
    TextEncoder, TextDecoder, URL, URLSearchParams, queueMicrotask, atob, btoa,
    xenonIpcRenderer: {
      getRuntimeConfig: () => ({appPath: 'C:\\fixture', exeDir: 'C:\\fixture',
        execPath: 'C:\\fixture\\host.exe'}), setDispatchHandler() {},
    },
    location: {protocol: 'chrome:', hostname: 'xenon-player-electron', search: ''},
    console: {log() {}, warn() {}, error() {}},
  });
  vm.runInContext(source, context, {filename});
  return {context, crypto: context.require('crypto')};
}

function asynchronousCall(operation) {
  let returned = false;
  let calls = 0;
  const pending = new Promise((resolve, reject) => {
    assert.equal(operation((error, value) => {
      ++calls;
      try {
        assert.equal(returned, true);
        assert.equal(calls, 1);
        resolve({error, value});
      } catch (failure) { reject(failure); }
    }), undefined);
    returned = true;
  });
  return pending;
}

test('random operations fail explicitly without a secure source', async () => {
  const {crypto, context} = renderer(undefined);
  const target = vm.runInContext('Buffer.alloc(4)', context);
  assert.throws(() => crypto.randomBytes(4), {code: 'ERR_NOT_SUPPORTED'});
  assert.throws(() => crypto.randomFillSync(target), {code: 'ERR_NOT_SUPPORTED'});
  for (const operation of [cb => crypto.randomBytes(4, cb), cb => crypto.randomFill(target, cb)]) {
    const result = await asynchronousCall(operation);
    assert.equal(result.error.code, 'ERR_NOT_SUPPORTED');
    assert.equal(result.value, undefined);
  }
});

test('secure random bytes use quota-sized chunks and preserve callback results', async () => {
  const calls = [];
  const {crypto} = renderer({getRandomValues(bytes) {
    assert.ok(bytes.byteLength <= 65536);
    calls.push(bytes.byteLength);
    bytes.fill(173);
    return bytes;
  }});
  const bytes = crypto.randomBytes(131073);
  assert.deepEqual(calls, [65536, 65536, 1]);
  assert.equal(bytes.length, 131073);
  assert.ok(bytes.every(value => value === 173));
  const asyncBytes = await asynchronousCall(cb => crypto.randomBytes(65537, cb));
  assert.equal(asyncBytes.error, null);
  assert.equal(asyncBytes.value.length, 65537);
  assert.deepEqual(calls.slice(3), [65536, 1]);
});

test('random fills honor typed-array elements and DataView byte ranges', async () => {
  const {crypto, context} = renderer({getRandomValues(bytes) { bytes.fill(255); return bytes; }});
  const view = vm.runInContext('new Uint16Array(new ArrayBuffer(14), 2, 5)', context);
  assert.equal(crypto.randomFillSync(view, 1, 2), view);
  assert.deepEqual(Array.from(new Uint8Array(view.buffer)),
      [0, 0, 0, 0, 255, 255, 255, 255, 0, 0, 0, 0, 0, 0]);
  const data = vm.runInContext('new DataView(new ArrayBuffer(8), 2, 4)', context);
  const filled = await asynchronousCall(cb => crypto.randomFill(data, 1, 2, cb));
  assert.equal(filled.error, null);
  assert.equal(filled.value, data);
  assert.deepEqual(Array.from(new Uint8Array(data.buffer)), [0, 0, 0, 255, 255, 0, 0, 0]);
  for (const [offset, size] of [[-1, 1], [6, 0], [4, 2], [0, Infinity], [NaN, 1]]) {
    assert.throws(() => crypto.randomFillSync(view, offset, size), {code: 'ERR_OUT_OF_RANGE'});
  }
  assert.throws(() => crypto.randomFillSync(view, '1', 1), {code: 'ERR_INVALID_ARG_TYPE'});
});

test('invalid callbacks and lengths fail before random-source use', () => {
  let calls = 0;
  const {crypto, context} = renderer({getRandomValues() { ++calls; }});
  const target = vm.runInContext('Buffer.alloc(8)', context);
  for (const callback of [null, 1, {}, 'callback']) {
    assert.throws(() => crypto.randomBytes(8, callback), {code: 'ERR_INVALID_ARG_TYPE'});
    assert.throws(() => crypto.randomFill(target, 0, 4, callback), {code: 'ERR_INVALID_ARG_TYPE'});
  }
  for (const size of [-1, NaN, Infinity, 0x80000000]) {
    assert.throws(() => crypto.randomBytes(size), {code: 'ERR_OUT_OF_RANGE'});
  }
  assert.throws(() => crypto.randomBytes('8'), {code: 'ERR_INVALID_ARG_TYPE'});
  assert.equal(calls, 0);
});

test('secure-source failures reach the callback once without fake success', async () => {
  const failure = new Error('source failed');
  const {crypto, context} = renderer({getRandomValues() { throw failure; }});
  const target = vm.runInContext('Buffer.alloc(4)', context);
  const result = await asynchronousCall(cb => crypto.randomFill(target, cb));
  assert.equal(result.error, failure);
  assert.equal(result.value, undefined);
});

function main(source) {
  const context = vm.createContext({__xenonPlatform: 'win32', __xenonArch: 'x64',
    Buffer, queueMicrotask, __xenonCryptoRandom: source});
  vm.runInContext(['main/events.js', 'main/platform.js', 'common/promisify.js',
    'common/crypto_random.js', 'main/builtins.js'].map(readBootstrapPart).join('\n') +
    '\nglobalThis.api = cryptoModule;', context);
  return {context, crypto: context.api};
}

test('main randomFillSync writes exact view ranges using native secure randomness', () => {
  const calls = [];
  const {context, crypto} = main(size => { calls.push(size); return Buffer.alloc(size, 255); });
  for (const expression of ['new Uint16Array(new ArrayBuffer(14), 2, 5)',
    'new DataView(new ArrayBuffer(14), 2, 10)', 'Buffer.alloc(14).subarray(2, 12)',
    'new ArrayBuffer(10)', 'new SharedArrayBuffer(10)']) {
    const actual = vm.runInContext(expression, context);
    assert.equal(crypto.randomFillSync(actual, 1, 2), actual);
    const backing = new Uint8Array(actual.buffer || actual);
    const offset = (actual.byteOffset || 0) + (actual.BYTES_PER_ELEMENT || 1);
    const length = 2 * (actual.BYTES_PER_ELEMENT || 1);
    assert.deepEqual([...backing], Array.from({length: backing.length}, (_, i) =>
      i >= offset && i < offset + length ? 255 : 0));
    assert.equal(calls.at(-1), length);
    const before = calls.length;
    assert.equal(crypto.randomFillSync(actual, actual.byteLength / (actual.BYTES_PER_ELEMENT || 1), 0), actual);
    assert.equal(calls.length, before);
  }
});

test('main randomFill validates arguments before native work and completes asynchronously once', async () => {
  let calls = 0;
  const {crypto} = main(size => { ++calls; return Buffer.alloc(size, 173); });
  const target = new Uint16Array(5);
  for (const [offset, size] of [[-1, 1], [6, 0], [4, 2], [0, Infinity], [NaN, 1]])
    assert.throws(() => crypto.randomFillSync(target, offset, size), {code: 'ERR_OUT_OF_RANGE'});
  assert.throws(() => crypto.randomFillSync(target, '1', 1), {code: 'ERR_INVALID_ARG_TYPE'});
  assert.throws(() => crypto.randomFillSync('data'), {code: 'ERR_INVALID_ARG_TYPE'});
  assert.throws(() => crypto.randomFill(target, null), {code: 'ERR_INVALID_ARG_TYPE'});
  assert.equal(calls, 0);
  const result = await asynchronousCall(callback => crypto.randomFill(target, 1, 2, callback));
  assert.equal(result.error, null); assert.equal(result.value, target);
  assert.deepEqual([...new Uint8Array(target.buffer)], [0, 0, 173, 173, 173, 173, 0, 0, 0, 0]);
  assert.equal(calls, 1);
});

test('main randomFill preserves source failure and never invokes a callback twice', async () => {
  const failure = new Error('native random source unavailable');
  for (const source of [undefined, () => { throw failure; }]) {
    const {crypto} = main(source);
    const target = Buffer.alloc(4);
    assert.throws(() => crypto.randomFillSync(target), source ? error => error === failure :
      {code: 'ERR_NOT_SUPPORTED'});
    const result = await asynchronousCall(callback => crypto.randomFill(target, callback));
    if (source) assert.equal(result.error, failure);
    else assert.equal(result.error.code, 'ERR_NOT_SUPPORTED');
    assert.equal(result.value, undefined);
    assert.deepEqual([...target], [0, 0, 0, 0]);
  }
});
