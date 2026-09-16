// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
const assert = require('node:assert/strict');
const nativeCrypto = require('node:crypto');
const {readFileSync} = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');
const filename = path.join(__dirname, 'xenon_ipc_renderer_bootstrap.js');
const source = readFileSync(filename, 'utf8');

function renderer() {
  const context = vm.createContext({
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

function input(context, kind) {
  return vm.runInContext(`(() => {
    const bytes = Buffer.from([17, 34, 0, 255, 128, 65, 18, 52, 85, 102]);
    if ('${kind}' === 'buffer') return bytes.subarray(2, 8);
    if ('${kind}' === 'uint16') return new Uint16Array(bytes.buffer, 2, 3);
    if ('${kind}' === 'dataview') return new DataView(bytes.buffer, 2, 6);
    return bytes.buffer.slice(2, 8);
  })()`, context);
}

for (const algorithm of ['md5', 'sha1', 'sha256']) {
  test(`${algorithm}: hash updates capture raw typed-array and DataView bytes`, () => {
    const {context, crypto} = renderer();
    const expected = nativeCrypto.createHash(algorithm);
    const actual = crypto.createHash(algorithm);
    for (const kind of ['buffer', 'uint16', 'dataview']) {
      const bytes = input(context, kind);
      // The native implementation is the oracle for both raw view bytes and
      // update-time ownership; mutating the input later must change neither.
      expected.update(bytes);
      assert.equal(actual.update(bytes), actual);
      new Uint8Array(bytes.buffer).fill(9);
    }
    expected.update('tail');
    actual.update('tail');
    assert.equal(actual.digest('hex'), expected.digest('hex'));
  });

  test(`${algorithm}: HMAC captures key at creation and raw message bytes at update`, () => {
    const {context, crypto} = renderer();
    for (const kind of ['buffer', 'uint16', 'dataview', 'arraybuffer']) {
      const key = input(context, kind);
      const expected = nativeCrypto.createHmac(algorithm, key);
      const actual = crypto.createHmac(algorithm, key);
      new Uint8Array(key.buffer || key).fill(7);
      for (const messageKind of ['uint16', 'dataview']) {
        const bytes = input(context, messageKind);
        expected.update(bytes);
        assert.equal(actual.update(bytes), actual);
        new Uint8Array(bytes.buffer).fill(8);
      }
      assert.equal(actual.digest('hex'), expected.digest('hex'), kind);
    }
  });
}
