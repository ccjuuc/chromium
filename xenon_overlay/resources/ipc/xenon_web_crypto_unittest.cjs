// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
'use strict';
const assert = require('node:assert/strict');
const test = require('node:test');
const vm = require('node:vm');
const native = require('node:crypto');
const {readBootstrapPart} = require('./bootstrap_test_support.cjs');
function fixture() {
  const realm = vm.createContext({native, DOMException});
  vm.runInContext(readBootstrapPart('main/web_crypto.js') + `
    globalThis.api = createMainWebCrypto({randomBytes: size => native.randomBytes(size),
      randomUUID: () => native.randomUUID(), digest: (name, input) => native.createHash(name).update(input).digest(), DOMException});
  `, realm);
  return {realm, ...realm.api};
}
test('WebCrypto hashes exact subviews using the same algorithm and bytes as Node', async () => {
  const {crypto} = fixture();
  for (const name of ['SHA-1','SHA-256','SHA-384','SHA-512']) {
    const input = new Uint8Array([1,2,3,4,5]).subarray(1,4);
    const expected = await native.webcrypto.subtle.digest(name, input);
    const result = crypto.subtle.digest({name}, input);
    input.fill(99);
    assert.deepEqual(Buffer.from(await result), Buffer.from(expected));
  }
  await assert.rejects(crypto.subtle.digest('MD5', new Uint8Array()), {name:'NotSupportedError'});
  const dataView = new DataView(new Uint8Array([9,1,2,3,9]).buffer,1,3);
  assert.deepEqual(Buffer.from(await crypto.subtle.digest('SHA-256', dataView)),
    Buffer.from(await native.webcrypto.subtle.digest('SHA-256', dataView)));
  await assert.rejects(crypto.subtle.importKey(), {name:'NotSupportedError'});
});
test('secure random fills preserve integer view identity and untouched surrounding bytes', () => {
  const {crypto, realm} = fixture();
  const view = vm.runInContext('new BigUint64Array(new ArrayBuffer(80),8,8)', realm);
  assert.equal(crypto.getRandomValues(view), view);
  const bytes = new Uint8Array(view.buffer);
  assert.deepEqual([...bytes.subarray(0,8)], new Array(8).fill(0));
  assert.deepEqual([...bytes.subarray(72)], new Array(8).fill(0));
  assert.ok(bytes.subarray(8,72).some(byte => byte !== 0));
  for (const value of [new Float32Array(4), new DataView(new ArrayBuffer(4)), {}, []])
    assert.throws(() => crypto.getRandomValues(value), {name:'TypeMismatchError'});
  assert.throws(() => crypto.getRandomValues(new Uint8Array(65537)), {name:'QuotaExceededError'});
});
test('Crypto brands reject forged receivers and UUIDs have standard version and variant', () => {
  const {crypto, Crypto, SubtleCrypto} = fixture();
  assert.equal(Object.prototype.toString.call(crypto), '[object Crypto]');
  assert.equal(Object.prototype.toString.call(crypto.subtle), '[object SubtleCrypto]');
  assert.throws(() => new Crypto(), {code:'ERR_ILLEGAL_CONSTRUCTOR'});
  assert.throws(() => new SubtleCrypto(), {code:'ERR_ILLEGAL_CONSTRUCTOR'});
  assert.throws(() => crypto.randomUUID.call({}), {code:'ERR_INVALID_THIS'});
  const values = new Set(Array.from({length:16}, () => crypto.randomUUID()));
  assert.equal(values.size,16);
  for (const value of values) assert.match(value, /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/);
});
