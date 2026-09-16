// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
const assert = require('node:assert/strict');
const {readFileSync} = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');
const native = require('node:zlib');

function renderer() {
  const context = vm.createContext({TextEncoder, TextDecoder, URL, URLSearchParams,
    queueMicrotask, atob, btoa,
    xenonIpcRenderer: {
      getRuntimeConfig: () => ({appPath: 'C:\\fixture', exeDir: 'C:\\fixture',
        execPath: 'C:\\fixture\\host.exe'}), setDispatchHandler() {},
      sendSync(channel, request) {
        assert.equal(channel, '__xenon:zlib');
        return native[request.operation + 'Sync'](
            Buffer.from(request.dataBase64, 'base64'), request.options).toString('base64');
      },
      invoke(channel, request) {
        assert.equal(channel, '__xenon:zlib');
        return new Promise((resolve, reject) => native[request.operation](
            Buffer.from(request.dataBase64, 'base64'), request.options,
            (error, output) => error ? reject(error) : resolve(output.toString('base64'))));
      },
    },
    location: {protocol: 'chrome:', hostname: 'xenon-player-electron', search: ''},
    console: {log() {}, warn() {}, error() {}},
  });
  vm.runInContext(readFileSync(path.join(__dirname, 'xenon_ipc_renderer_bootstrap.js'), 'utf8'), context);
  return {context, zlib: context.require('zlib')};
}

function main() {
  const source = readFileSync(path.join(__dirname, 'xenon_ipc_main_bootstrap.js'), 'utf8');
  const factory = source.slice(source.indexOf('  function createZlibModule(call) {'),
      source.indexOf('  const zlibModule = createZlibModule('));
  const context = vm.createContext({Buffer, queueMicrotask,
    call(operation, input, options, asynchronous) {
      if (!asynchronous) return native[operation + 'Sync'](input, options);
      return new Promise((resolve, reject) => native[operation](input, options,
          (error, output) => error ? reject(error) : resolve(output)));
    },
  });
  return {context, zlib: vm.runInContext(factory + '\ncreateZlibModule(call)', context)};
}

for (const [label, create] of [['main', main], ['renderer', renderer]]) {
  test(`${label} zlib sync operations round trip binary subviews and strings`, () => {
    const {context, zlib} = create();
    const input = vm.runInContext('new Uint8Array([99, 0, 255, 128, 65, 99]).subarray(1, 5)', context);
    for (const [compress, decompress] of [['gzip', 'gunzip'], ['deflate', 'inflate'],
        ['deflateRaw', 'inflateRaw']]) {
      const encoded = zlib[compress + 'Sync'](input, {level: 9});
      assert.deepEqual(Buffer.from(zlib[decompress + 'Sync'](encoded)), Buffer.from([0, 255, 128, 65]));
      assert.equal(Buffer.from(native[decompress + 'Sync'](encoded)).toString('hex'), '00ff8041');
    }
    assert.equal(zlib.unzipSync(zlib.gzipSync('中文')).toString(), '中文');
    assert.equal(zlib.unzipSync(zlib.deflateSync('zlib')).toString(), 'zlib');
    assert.equal(zlib.codes[-3], 'Z_DATA_ERROR');
  });

  test(`${label} zlib callbacks are asynchronous and snapshot the input`, async () => {
    const {context, zlib} = create();
    const input = vm.runInContext('new Uint8Array([0, 255, 128, 65])', context);
    let returned = false;
    const pending = new Promise((resolve, reject) => zlib.gzip(input, (error, packed) => {
      if (error) return reject(error);
      assert.equal(returned, true);
      resolve(packed);
    }));
    input.fill(9);
    returned = true;
    assert.equal(zlib.gunzipSync(await pending).toString('hex'), '00ff8041');
  });

  test(`${label} zlib corruption and unsupported operations do not report success`, async () => {
    const {zlib} = create();
    assert.throws(() => zlib.gunzipSync('bad'), {code: 'Z_DATA_ERROR'});
    assert.throws(() => zlib.gzipSync('payload', {maxOutputLength: 2}), {code: 'ERR_BUFFER_TOO_LARGE'});
    assert.throws(() => zlib.createGzip(), {code: 'ERR_NOT_SUPPORTED'});
    assert.throws(() => new zlib.Gunzip(), {code: 'ERR_NOT_SUPPORTED'});
    assert.throws(() => zlib.gzipSync('x', {dictionary: new Uint8Array(1)}), {code: 'ERR_NOT_SUPPORTED'});
    assert.throws(() => zlib.gzipSync('x', {level: 10}), {code: 'ERR_OUT_OF_RANGE'});
    assert.throws(() => zlib.gzip('x'), {code: 'ERR_INVALID_ARG_TYPE'});
    const failure = await new Promise(resolve => zlib.gunzip('bad', error => resolve(error)));
    assert.equal(failure.code, 'Z_DATA_ERROR');
  });
}

test('renderer zlib aliases preserve module identity', () => {
  const {context, zlib} = renderer();
  assert.equal(context.require('node:zlib'), zlib);
});
