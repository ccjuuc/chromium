// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

const assert = require('node:assert/strict');
const path = require('node:path');
const test = require('node:test');
const util = require('node:util');
const vm = require('node:vm');
const {readBootstrap} = require('./bootstrap_test_support.cjs');

function load(kind, webCodecs = true) {
  const context = vm.createContext({
    TextEncoder, TextDecoder, URL, URLSearchParams, queueMicrotask,
    setTimeout, clearTimeout, setInterval, clearInterval, atob, btoa,
    console: {log() {}, warn() {}, error() {}},
    __xenonPaths: {platform: 'win32', arch: 'x64', endianness: 'LE'},
    location: {protocol: 'file:', pathname: '/C:/fixture/index.html'},
    xenonIpcRenderer: {
      getRuntimeConfig: () => ({appPath: 'C:\\fixture', exeDir: 'C:\\fixture',
        execPath: 'C:\\fixture\\host.exe'}),
      setDispatchHandler() {},
    },
    __xenonPlatform: 'win32', __xenonArch: 'x64', __xenonEndianness: 'LE',
    __xenonOsRelease: 'fixture', __xenonAppPath: 'C:\\fixture',
    __xenonRendererBaseUrl: '', __xenonRendererUrlMappings: [],
    __xenonAppName: 'fixture', __xenonAppVersion: '1', __xenonUserAgent: '',
    __xenonExecPath: 'C:\\fixture\\host.exe', __xenonPid: 1, __xenonEnv: {},
    __xenonChromeVersion: '153', __xenonV8Version: '', __xenonGetPath: () => '',
  });
  if (!webCodecs) { delete context.TextEncoder; delete context.TextDecoder; }
  vm.runInContext(readBootstrap(path.join(__dirname,
      `xenon_ipc_${kind}_bootstrap.js`)), context);
  return kind === 'main' ? context.__xenonUtil : context.require('node:util');
}

const outcome = operation => {
  try { return {value: operation()}; }
  catch (error) { return {name: error.name, code: error.code}; }
};

test('main fallback TextEncoder writes complete UTF-8 characters and counts UTF-16 reads', () => {
  const Encoder = load('main', false).TextEncoder;
  assert.notEqual(Encoder, TextEncoder, 'the real plain-V8 fallback must be exercised');
  const actualEncoder = new Encoder();
  const expectedEncoder = new TextEncoder();
  assert.equal(actualEncoder.encoding, expectedEncoder.encoding);
  const strings = ['', 'ASCII', 'é中文😀', '\ufeff\0end', '\ud800', '\udc00',
    '\ud800x\udc00', '\ud800\ud800\udc00\udc00', 'a😀b中c'];
  for (const value of strings) {
    for (let length = 0; length <= Buffer.byteLength(value) + 1; ++length) {
      const actual = new Uint8Array(length + 4).fill(0xa5);
      const expected = new Uint8Array(length + 4).fill(0xa5);
      const result = actualEncoder.encodeInto(value, actual.subarray(2, length + 2));
      const reference = expectedEncoder.encodeInto(value, expected.subarray(2, length + 2));
      assert.deepEqual({read: result.read, written: result.written}, reference,
        `capacity ${length}: ${JSON.stringify(value)}`);
      assert.deepEqual([...actual], [...expected]);
    }
  }
  const actual = Buffer.alloc(8), expected = Buffer.alloc(8);
  const result = actualEncoder.encodeInto('a😀中', actual);
  assert.deepEqual({read: result.read, written: result.written},
    expectedEncoder.encodeInto('a😀中', expected));
  assert.deepEqual(actual, expected);
});

test('main fallback TextEncoder preserves encode conversion and rejects invalid encodeInto arguments', () => {
  const actualEncoder = new (load('main', false).TextEncoder)();
  const expectedEncoder = new TextEncoder();
  for (const value of [undefined, null, false, 0, 123n, {}, ['a'], Symbol('x'),
    '\ud800x\udc00', '中😀', {toString: () => 'é'}]) {
    assert.deepEqual(outcome(() => [...actualEncoder.encode(value)]),
      outcome(() => [...expectedEncoder.encode(value)]));
  }
  for (const value of [undefined, null, false, 0, 123n, {}, Symbol('x'), new String('text')]) {
    assert.deepEqual(outcome(() => actualEncoder.encodeInto(value, new Uint8Array(8))),
      outcome(() => expectedEncoder.encodeInto(value, new Uint8Array(8))));
  }
  for (const destination of [undefined, null, [], new Uint8ClampedArray(8),
    new Uint16Array(4), new DataView(new ArrayBuffer(8)), new ArrayBuffer(8)]) {
    assert.deepEqual(outcome(() => actualEncoder.encodeInto('text', destination)),
      outcome(() => expectedEncoder.encodeInto('text', destination)));
  }
});

for (const kind of ['main', 'renderer']) {
  const hosted = load(kind);
  const strip = hosted.stripVTControlCharacters;
  function compare(value) {
    assert.deepEqual(outcome(() => strip(value)),
        outcome(() => util.stripVTControlCharacters(value)));
  }

  test(`${kind}: util strips CSI colors and cursor controls like Node`, () => {
    assert.equal(typeof strip, 'function');
    assert.equal(strip.name, 'stripVTControlCharacters');
    assert.equal(strip.length, 1);
    for (const prefix of ['\u001B[', '\u009B']) {
      for (const code of ['m', '0m', '31m', '1;4;31m', '38;5;123m',
        '38;2;1;2;3m', '2J', 'K', '1;1H', '?25l', '?25h', '2A', '5D', '9999~']) {
        compare(`${prefix}${code}中文🙂 text${prefix}0m`);
      }
    }
    assert.equal(strip('\u001B[31mred\u001B[0m'), 'red');
    assert.equal(strip('\u009B31mred\u009B0m'), 'red');
  });

  test(`${kind}: util strips OSC hyperlinks and all Node string terminators`, () => {
    for (const terminator of ['\u0007', '\u001B\\', '\u009C']) {
      const link = '\u001B]8;;https://example.test/path?q=1#part' + terminator +
          'click here' + '\u001B]8;;' + terminator;
      compare(link);
      assert.equal(strip(link), 'click here');
      for (const payload of ['0;title', '2;title_with-punctuation', '52;c;YWJj'])
        compare('prefix\u001B]' + payload + terminator + 'suffix');
    }
  });

  test(`${kind}: util preserves ordinary text and Node incomplete-sequence behavior`, () => {
    for (const value of ['', '中文🙂 e\u0301', 'line\n\r\tend', '\0\b\u0007\u009C',
      '\u001B', '\u009B', '\u001B[', '\u001B[31', '\u001B]8;;https://example.test',
      '\u001B[12345m', '\u001B[38:2:1:2:3m', '\u001B(Babc\u001B(0',
      '\u001B#8abc', '\u001B7saved\u001B8', '\u009D0;title\u009C']) compare(value);
    assert.equal(strip('line\n\r\tend\0\b'), 'line\n\r\tend\0\b');
  });

  test(`${kind}: util rejects non-strings without coercion`, () => {
    for (const value of [undefined, null, 0, NaN, true, 1n, {}, [], () => {},
      Symbol('test'), new String('boxed')]) compare(value);
    const value = {[Symbol.toPrimitive]() { assert.fail('must not coerce'); },
      toString() { assert.fail('must not stringify'); }};
    compare(value);
    assert.throws(() => strip(value), {name: 'TypeError', code: 'ERR_INVALID_ARG_TYPE'});
  });

  test(`${kind}: mixed VT sequences and repeated calls agree with Node`, () => {
    const fragments = ['hello', '中文', '🙂', '\u001B', '\u009B', '[31m', '[0m',
      ']8;;https://example.test', '\u0007', '\u001B\\', '\u009C', '\n', '[?25l',
      '\u001B[2J', '\u001B[1;1H', '\u009B32m', '\0', ';', '12345', '?'];
    let state = 0x13579bdf;
    for (let i = 0; i < 400; ++i) {
      let value = '';
      for (let j = 0; j < 12; ++j) {
        state = (Math.imul(state, 1664525) + 1013904223) >>> 0;
        value += fragments[state % fragments.length];
      }
      compare(value);
      compare('ordinary');
      compare('\u001B[31mred\u001B[0m');
    }
  });
}
