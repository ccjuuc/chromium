// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');

function load(platform = process.platform) {
  const context = vm.createContext({});
  vm.runInContext(fs.readFileSync(path.join(__dirname, 'common/blob.js'), 'utf8'), context);
  return vm.runInContext(`createBlobConstructor({platform:${JSON.stringify(platform)}})`, context);
}
const HostedBlob = load();
const bytes = async blob => Array.from(new Uint8Array(await blob.arrayBuffer()));
async function compare(parts, options) {
  const actual = new HostedBlob(parts, options);
  const expected = new Blob(parts, options);
  assert.equal(actual.size, expected.size);
  assert.equal(actual.type, expected.type);
  assert.equal(await actual.text(), await expected.text());
  assert.deepEqual(await bytes(actual), await bytes(expected));
  return {actual, expected};
}

test('Blob copies BufferSource subviews and handles Unicode like Node', async () => {
  const source = new Uint8Array([1, 2, 3, 4]);
  const view = new DataView(source.buffer, 1, 2);
  const {actual} = await compare([source.subarray(1, 3), view, '中🙂\ud800a\udc00', 42, null]);
  const initial = await bytes(actual);
  source.fill(255);
  assert.deepEqual(await bytes(actual), initial);
  const exposed = await actual.bytes();
  exposed.fill(0);
  assert.deepEqual(await bytes(actual), initial);
});

test('Blob immutable nesting and slices preserve bytes, type and boundaries', async () => {
  const parent = new HostedBlob(['first', new Uint8Array([0, 255]), '🙂last'], {type: 'Text/Plain'});
  const nested = new HostedBlob([parent, parent.slice(2, -1)]);
  const reference = new Blob([await parent.arrayBuffer(), await parent.slice(2, -1).arrayBuffer()]);
  for (const start of [-Infinity, -100, -1, 0, 1.5, 8, 100, Infinity, NaN, 2 ** 64]) {
    for (const end of [undefined, -2, 0, 8, 100, Infinity, NaN]) {
      const actual = nested.slice(start, end, 'IMAGE/PNG');
      const expected = reference.slice(start, end, 'IMAGE/PNG');
      assert.equal(actual.size, expected.size);
      assert.equal(actual.type, expected.type);
      assert.deepEqual(await bytes(actual), await bytes(expected));
    }
  }
});

test('Blob UTF-8 replacement decoding agrees with Node for invalid bytes', async () => {
  for (const input of [[0xef, 0xbb, 0xbf, 0x61], [0xef, 0xbb, 0xbf, 0xef, 0xbb, 0xbf],
    [0xc0, 0xaf], [0xe0, 0x80, 0x80], [0xed, 0xa0, 0x80], [0xf4, 0x90, 0x80, 0x80],
    [0xf0, 0x90, 0x61], [0xc2], [0xe2, 0x82], [0xff, 0x61, 0xfe]]) {
    await compare([new Uint8Array(input)]);
  }
  let state = 0x12345678;
  for (let n = 0; n < 300; ++n) {
    const input = new Uint8Array(100);
    for (let i = 0; i < input.length; ++i) {
      state = (Math.imul(state, 1664525) + 1013904223) >>> 0;
      input[i] = state >>> 24;
    }
    await compare([input]);
  }
});

test('Blob native endings and MIME normalization match the host platform', async () => {
  for (const type of ['', 'TEXT/HTML', 'Text/Plain; charset=UTF-8', 'invalid\n', '中', null, 42])
    await compare(['a\rb\nc\r\nd'], {type, endings: 'native'});
  for (const platform of ['win32', 'darwin', 'linux']) {
    const Constructor = load(platform);
    assert.equal(await new Constructor(['a\rb\nc\r\nd'], {endings: 'native'}).text(),
        platform === 'win32' ? 'a\rb\r\nc\r\nd' : 'a\rb\nc\nd');
  }
});

test('Blob accepts iterables and rejects invalid sequences/options like Node', async () => {
  await compare(new Set(['a', 'b']));
  const outcome = fn => { try { fn(); return null; } catch (e) { return [e.name, e.code]; } };
  for (const [parts, options] of [[null], ['abc'], [{}], [[Symbol('part')]], [[], 1],
    [[], {endings: null}], [[], {endings: 'bad'}], [[], {type: Symbol('type')}], [[], null]])
    assert.deepEqual(outcome(() => new HostedBlob(parts, options)), outcome(() => new Blob(parts, options)));
});

test('Blob brand checks and async APIs reject forged receivers', async () => {
  assert.equal(Object.prototype.toString.call(new HostedBlob()), '[object Blob]');
  const prototype = HostedBlob.prototype;
  for (const name of ['size', 'type'])
    assert.throws(() => Object.getOwnPropertyDescriptor(prototype, name).get.call({}),
        {code: 'ERR_INVALID_THIS'});
  assert.throws(() => prototype.slice.call({}), {code: 'ERR_INVALID_THIS'});
  for (const name of ['arrayBuffer', 'bytes', 'text'])
    await assert.rejects(prototype[name].call({}), {code: 'ERR_INVALID_THIS'});
  assert.throws(() => new HostedBlob(['abc']).stream(), {code: 'ERR_NOT_SUPPORTED'});
});

test('Blob reads iterable and options getters once, in Node order', async () => {
  function run(Constructor, invalidEndings) {
    const reads = [];
    const parts = {
      get [Symbol.iterator]() {
        reads.push('iterator');
        return function* () { assert.equal(this, parts); reads.push('iterate'); yield 'value'; };
      },
    };
    const options = {
      get endings() { reads.push('endings'); return invalidEndings ? 'bad' : 'transparent'; },
      get type() { reads.push('type'); return 'TEXT/PLAIN'; },
    };
    try {
      const blob = new Constructor(parts, options);
      return {reads, size: blob.size, type: blob.type};
    } catch (error) { return {reads, code: error.code}; }
  }
  assert.deepEqual(run(HostedBlob, false), run(Blob, false));
  assert.deepEqual(run(HostedBlob, true), run(Blob, true));
});
