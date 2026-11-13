// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
const {readBootstrap} = require('./bootstrap_test_support.cjs');
const assert = require('node:assert/strict');
const nativeCrypto = require('node:crypto');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');
function runtime(kind = 'renderer') {
  const filename = path.join(__dirname, `xenon_ipc_${kind}_bootstrap.js`);
  const context = vm.createContext({
    __xenonPaths: {platform: 'win32', arch: 'x64', endianness: 'LE'},
    TextEncoder, TextDecoder, URL, URLSearchParams, queueMicrotask, atob, btoa,
    setTimeout, clearTimeout, setInterval, clearInterval,
    xenonIpcRenderer: {
      getRuntimeConfig: () => ({appPath: 'C:\\fixture', exeDir: 'C:\\fixture',
        execPath: 'C:\\fixture\\host.exe'}), setDispatchHandler() {},
    },
    location: {protocol: 'chrome:', hostname: 'xenon-player-electron', search: ''},
    console: {log() {}, warn() {}, error() {}},
    __xenonPlatform: 'win32', __xenonArch: 'x64', __xenonEndianness: 'LE',
    __xenonOsRelease: '10.0', __xenonAppPath: 'C:\\fixture',
    __xenonRendererBaseUrl: '', __xenonRendererUrlMappings: [],
    __xenonAppName: 'fixture', __xenonAppVersion: '1', __xenonUserAgent: '',
    __xenonExecPath: 'C:\\fixture\\host.exe', __xenonPid: 1, __xenonEnv: {},
    __xenonChromeVersion: '153', __xenonV8Version: '', __xenonGetPath: () => '',
    __xenonCryptoDigest(algorithm, bytes, key) {
      const digest = key === null ? nativeCrypto.createHash(algorithm) :
          nativeCrypto.createHmac(algorithm, key);
      return digest.update(bytes).digest();
    },
  });
  vm.runInContext(readBootstrap(filename), context, {filename});
  return {context, crypto: kind === 'main' ? context.__xenonCrypto : context.require('crypto')};
}

for (const kind of ['main', 'renderer']) {
  test(`${kind}: every advertised hash computes the Node digest, with independent result arrays`, () => {
    const {crypto} = runtime(kind);
    const algorithms = crypto.getHashes();
    const names = [...algorithms];
    assert.ok(names.length > 0);
    assert.equal(new Set(names).size, names.length);
    // Capability discovery must not accidentally claim algorithms that are
    // only available in asynchronous WebCrypto or Node's separate OpenSSL.
    assert.equal(names.includes('sha512'), kind === 'main');
    for (const name of names) {
      assert.ok(nativeCrypto.getHashes().includes(name));
      const input = Buffer.from([0, 255, 17, 128, 99]);
      assert.equal(crypto.createHash(name).update(input).digest('hex'),
          nativeCrypto.createHash(name).update(input).digest('hex'), name);
    }
    algorithms.splice(0, algorithms.length, 'not-a-real-digest');
    assert.deepEqual([...crypto.getHashes()], names);
    assert.throws(() => crypto.createHash('not-a-real-digest').update('abc').digest());
  });
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
    const {context, crypto} = runtime();
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
    const {context, crypto} = runtime();
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
