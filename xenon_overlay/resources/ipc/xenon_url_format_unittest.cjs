// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

const assert = require('node:assert/strict');
const test = require('node:test');
const vm = require('node:vm');
const nodeUrl = require('node:url');
const {readBootstrapPart} = require('./bootstrap_test_support.cjs');

function fixture(platform) {
  const context = vm.createContext({__xenonPlatform: platform,
    __xenonArch: 'x64', Buffer, queueMicrotask});
  vm.runInContext(['main/events.js', 'main/platform.js',
    'common/promisify.js', 'main/builtins.js'].map(readBootstrapPart).join('\n') +
    '\nglobalThis.url = urlModule;', context);
  return context;
}

for (const platform of ['win32', 'linux']) {
  test(`main ${platform}: url.format preserves packaged application query and file paths`, () => {
    const {url} = fixture(platform);
    const pathname = platform === 'win32' ? 'C:\\app dir\\resources\\app.asar/web/index.html' :
      '/opt/app dir/resources/app.asar/web/index.html';
    const input = {pathname, protocol: 'file:', slashes: true,
      query: {mode: 'device', offline: 1, endpoint: 'https://example.test/path',
        language: '简体 中文', feature: ['one', 'two'], enabled: true, none: null}};
    assert.equal(url.format(input), nodeUrl.format(input));
    assert.match(url.format(input), /feature=one&feature=two/);
    assert.match(url.format(input), /mode=device/);
  });

  test(`main ${platform}: legacy url.format matches Node field precedence and escaping`, () => {
    const {url} = fixture(platform);
    const cases = [
      {protocol: 'https', hostname: 'example.test', port: 8443, pathname: 'a?b#c', hash: 'end'},
      {protocol: 'https:', hostname: '::1', auth: 'user:p@ss', pathname: '/a', query: {a: 1}},
      {protocol: 'https:', hostname: '[::1]', port: '443', pathname: '/a'},
      {protocol: 'https:', host: 'chosen.test', hostname: 'ignored.test', search: 'q=a#b', query: {ignored: 1}},
      {protocol: 'mailto:', pathname: 'user@example.test', query: {subject: 'hello world'}},
      {pathname: '/relative?literal#name', query: {q: [null, Infinity, 42n, {}, '']}, hash: '#fragment'},
      {href: 'https://ignored.test/', pathname: '/selected', query: 'ignored=string'},
      {protocol: 'custom:', slashes: true, hostname: 'host', pathname: 'resource'},
      {protocol: 'file:', pathname: '/a', query: {empty: []}},
      {},
    ];
    for (const input of cases) assert.equal(url.format(input), nodeUrl.format(input));
    for (const input of [null, undefined, 1, false]) assert.throws(() => url.format(input), {code: 'ERR_INVALID_ARG_TYPE'});
  });

  test(`main ${platform}: WHATWG url.format supports omission options without mutating URL`, () => {
    const context = fixture(platform);
    vm.runInContext("globalThis.value = new URL('https://user:pass@example.test/a?q=one#part')", context);
    const native = new URL('https://user:pass@example.test/a?q=one#part');
    for (const options of [{}, {auth: false}, {search: false}, {fragment: false},
      {auth: false, search: false, fragment: false}])
      assert.equal(context.url.format(context.value, options), nodeUrl.format(native, options));
    assert.equal(context.value.href, native.href);
    assert.throws(() => context.url.format(context.value, {unicode: true}), {code: 'ERR_NOT_SUPPORTED'});
  });
}

test('main URL preserves encoded credentials through parsing, resolution and mutation', () => {
  const {URL: CompatURL} = fixture('win32');
  for (const input of ['http://us%65r:p%40ss@example.test/a',
    'https://user@name:p:ss@example.test/a?q=one#part',
    'http://用户:密 码@example.test/', 'http://:password@example.test/']) {
    const actual = new CompatURL(input), expected = new URL(input);
    for (const name of ['href', 'username', 'password', 'origin'])
      assert.equal(actual[name], expected[name], name);
    actual.searchParams.set('next', 'one two'); expected.searchParams.set('next', 'one two');
    assert.equal(actual.href, expected.href);
    assert.equal(new CompatURL('child', actual).href, new URL('child', expected).href);
  }
  const actual = new CompatURL('https://example.test/'), expected = new URL('https://example.test/');
  for (const [name, value] of [['username', 'u:@;=/[]%'], ['password', 'p@ss:中文😀'],
    ['username', ''], ['password', '']]) {
    actual[name] = value; expected[name] = value;
    assert.equal(actual.href, expected.href);
  }
});
