// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const test = require('node:test');
const {File: NativeFile, Blob: NativeBlob} = require('node:buffer');
const {ReadableStream} = require('./third_party/web_streams_polyfill/dist/ponyfill.js');
const factories = vm.runInThisContext(fs.readFileSync(path.join(__dirname, 'common/blob.js'), 'utf8') +
    ';({createBlobConstructor, createFileConstructor})');
const Blob = factories.createBlobConstructor({platform: process.platform, ReadableStream});
const File = factories.createFileConstructor({Blob});
const bytes = async value => [...new Uint8Array(await value.arrayBuffer())];
const outcome = fn => { try { fn(); return null; } catch (e) { return [e.name, e.code]; } };

test('File copies sources, inherits Blob operations and slice returns a Blob', async () => {
  const input = new Uint8Array([1, 2, 3, 4]);
  const parts = [input.subarray(1), new DataView(input.buffer, 1, 2), '中🙂\ud800'];
  const options = {type: 'Text/Plain', lastModified: 123456};
  const file = new File(parts, 'folder/name.txt', options);
  const native = new NativeFile(parts, 'folder/name.txt', options);
  input.fill(255);
  assert.ok(file instanceof File); assert.ok(file instanceof Blob);
  assert.equal(file.name, native.name); assert.equal(file.type, native.type);
  assert.equal(file.lastModified, native.lastModified); assert.equal(file.size, native.size);
  assert.equal(await file.text(), await native.text());
  assert.deepEqual(await bytes(file), await bytes(native));
  const exposed = await file.bytes(); exposed.fill(0);
  assert.deepEqual(await bytes(file), await bytes(native));
  const slice = file.slice(2, -1, 'IMAGE/JPEG');
  assert.ok(slice instanceof Blob); assert.equal(slice instanceof File, false);
  assert.equal(slice.type, 'image/jpeg'); assert.equal(slice.name, undefined);
  assert.deepEqual(await bytes(slice), await bytes(native.slice(2, -1)));
  const nested = new File([file, file.slice(0, 3)], 'copy', {lastModified: 0});
  assert.deepEqual(await bytes(nested), [...await bytes(file), ...await bytes(file.slice(0, 3))]);
});

test('File metadata conversion and readonly descriptors agree with Node', () => {
  for (const name of ['a/b\\c', '', '\ud800A\udc00🙂', null, undefined, 12,
    {toString() { return 'named'; }}]) {
    const file = new File([], name, {lastModified: 0});
    assert.equal(file.name, new NativeFile([], name, {lastModified: 0}).name);
    assert.equal(Object.prototype.toString.call(file), '[object File]');
  }
  for (const lastModified of [0, -0, 1.9, -1.9, NaN, Infinity, -Infinity, 2 ** 64,
    null, true, '123.5', {valueOf() { return 3; }}]) {
    const file = new File([], 'x', {lastModified});
    assert.ok(Object.is(file.lastModified, new NativeFile([], 'x', {lastModified}).lastModified));
  }
  const before = Date.now(), file = new File([], 'x'), after = Date.now();
  assert.ok(file.lastModified >= before && file.lastModified <= after);
  for (const name of ['name', 'lastModified']) {
    const actual = Object.getOwnPropertyDescriptor(File.prototype, name);
    const expected = Object.getOwnPropertyDescriptor(NativeFile.prototype, name);
    assert.equal(actual.set, expected.set);
    assert.equal(actual.enumerable, expected.enumerable);
    assert.equal(actual.configurable, expected.configurable);
    assert.equal(Reflect.set(file, name, 'changed'), false);
    assert.throws(() => actual.get.call({}), {code: 'ERR_INVALID_THIS'});
    assert.throws(() => actual.get.call(new Blob()), {code: 'ERR_INVALID_THIS'});
  }
});

test('File rejects missing arguments and invalid WebIDL inputs like Node', () => {
  for (const args of [[], [[]], [null, 'x'], ['bits', 'x'], [[Symbol()], 'x'],
    [[], Symbol()], [[], 'x', 1], [[], 'x', {lastModified: 1n}],
    [[], 'x', {lastModified: Symbol()}], [[], 'x', {type: Symbol()}],
    [[], 'x', {endings: 'invalid'}], [undefined, 'x'], [[], undefined, null]]) {
    assert.deepEqual(outcome(() => new File(...args)), outcome(() => new NativeFile(...args)));
  }
  assert.throws(() => File([], 'x'), {name: 'TypeError'});
  class SubFile extends File {}
  const sub = new SubFile([], 'sub', {lastModified: 1});
  assert.equal(sub.name, 'sub'); assert.ok(sub instanceof Blob);
});

test('File getter/conversion order matches Node including throwing metadata', () => {
  function inspect(Constructor, throwModified = false) {
    const reads = [];
    const parts = {get [Symbol.iterator]() { reads.push('iterator'); return function* () {
      reads.push('bits'); yield {toString() { reads.push('bit-string'); return 'data'; }};
    }; }};
    const options = {
      get endings() { reads.push('endings'); return 'transparent'; },
      get type() { reads.push('type'); return 'TEXT/PLAIN'; },
      get lastModified() { reads.push('lastModified'); return {valueOf() {
        reads.push('modified-number'); if (throwModified) throw Error('modified'); return 12;
      }}; },
    };
    const name = {toString() { reads.push('name-string'); return 'file'; }};
    const failed = outcome(() => new Constructor(parts, name, options));
    return {reads, failed};
  }
  assert.deepEqual(inspect(File), inspect(NativeFile));
  assert.deepEqual(inspect(File, true), inspect(NativeFile, true));
});

test('File byte streams preserve immutable content with BYOB and cancellation', async () => {
  const input = new Uint8Array(150000); for (let i = 0; i < input.length; ++i) input[i] = i % 251;
  const file = new File([input], 'large.bin', {lastModified: 3});
  const native = new NativeFile([input], 'large.bin', {lastModified: 3});
  const reader = file.stream().getReader({mode: 'byob'});
  const output = [];
  for (;;) {
    const {done, value} = await reader.read(new Uint8Array(4096));
    if (done) break;
    output.push(...value); value.fill(0);
  }
  assert.deepEqual(output, await bytes(native));
  const other = file.stream().getReader();
  assert.equal((await other.read()).value.byteLength, 65536);
  await other.cancel('unused');
  assert.deepEqual(await bytes(file), await bytes(native));
  assert.equal(file.name, 'large.bin'); assert.equal(file.lastModified, 3);
});
