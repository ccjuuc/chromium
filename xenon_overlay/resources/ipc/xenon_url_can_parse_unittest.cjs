// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

const assert = require('node:assert/strict');
const test = require('node:test');
const vm = require('node:vm');
const {readBootstrapPart} = require('./bootstrap_test_support.cjs');

function fixture(parse = URL.canParse) {
  const context = vm.createContext({__xenonPlatform: 'win32', __xenonArch: 'x64',
    __xenonCanParseURL: parse});
  vm.runInContext(['main/events.js', 'main/platform.js']
      .map(readBootstrapPart).join('\n'), context);
  return context.URL;
}

function outcome(call) {
  try { return {value: call()}; }
  catch (error) { return {name: error.name, code: error.code}; }
}

test('main URL.canParse preserves Node argument coercion and optional-base semantics', () => {
  // Inject the Node parser to isolate the JavaScript boundary. Native parser
  // validity is exercised separately in the native/runtime differential probe.
  const CompatURL = fixture();
  const cases = [[], [undefined], [null], [true], [12], [42n], [{}], [[]],
    [''], ['electron'], ['@scope/package'], ['node:fs'], ['file:///tmp/a'],
    ['https://example.test/'], ['child', 'https://example.test/base/'],
    [undefined, 'https://example.test/'], [null, 'https://example.test/'],
    ['https://example.test/', undefined], ['https://example.test/', null],
    ['https://example.test/', ''], ['https://example.test/', 'invalid base'],
    ['child', new URL('https://example.test/base/')],
    [Symbol('input')], ['child', Symbol('base')], [Object(Symbol('input'))],
    ['child', Object(Symbol('base'))], [Object.create(null)],
    ['child', Object.create(null)], ['https://example.test/\ud800'],
    ['child', 'https://example.test/\udc00'],
    [new URL('file:///tmp/a')]];
  for (const args of cases) {
    assert.deepEqual(outcome(() => CompatURL.canParse(...args)),
        outcome(() => URL.canParse(...args)));
  }
  assert.equal(CompatURL.canParse.length, URL.canParse.length);
  assert.equal(CompatURL.canParse.call(null, 'node:fs'), URL.canParse.call(null, 'node:fs'));
});

test('main URL.canParse converts arguments once in order before native parsing', () => {
  const trace = [];
  const CompatURL = fixture((input, base) => {
    trace.push(['parse', input, base]);
    return URL.canParse(input, base);
  });
  const input = {[Symbol.toPrimitive](hint) { trace.push(['input', hint]); return 'child\ud800'; }};
  const base = {[Symbol.toPrimitive](hint) { trace.push(['base', hint]); return 'https://example.test/\udc00'; }};
  assert.equal(CompatURL.canParse(input, base), true);
  assert.deepEqual(trace, [['input', 'string'], ['base', 'string'],
    ['parse', 'child\ufffd', 'https://example.test/\ufffd']]);

  const error = new RangeError('conversion sentinel');
  const throwing = {[Symbol.toPrimitive]() { throw error; }};
  trace.length = 0;
  assert.throws(() => CompatURL.canParse('not a URL', throwing), value => value === error);
  assert.deepEqual(trace, [], 'invalid input must not suppress base coercion errors');
  assert.throws(() => CompatURL.canParse(throwing, base), value => value === error);
  assert.deepEqual(trace, [], 'input coercion failure must not convert the base');
});

test('main URL.canParse distinguishes bare module specifiers from URL specifiers', () => {
  const CompatURL = fixture();
  const isBare = specifier => !CompatURL.canParse(specifier) &&
      !specifier.startsWith('.') && !specifier.startsWith('/');
  for (const name of ['electron', 'fs', '@scope/package', 'package/subpath'])
    assert.equal(isBare(name), true, name);
  for (const name of ['node:fs', 'file:///app/main.js', 'data:text/javascript,export{}',
    './main.js', '../main.js', '/app/main.js', 'https://example.test/module.js'])
    assert.equal(isBare(name), false, name);
});

test('main URL.canParse does not silently use the permissive legacy constructor', () => {
  const CompatURL = fixture(null);
  assert.throws(() => CompatURL.canParse('electron'), {code: 'ERR_NOT_SUPPORTED'});
});
